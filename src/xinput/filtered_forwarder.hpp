#pragma once
#include "xinput_poll.hpp"
#include <ViGEm/Client.h>
#include <Xinput.h>
#include <mutex>
#include <atomic>
#include <array>
#include <string>
#include <cstdlib>

// Applies ghost filtering (short pulse suppression & analog spike suppression)
// before sending to ViGEm.
class FilteredForwarder : public XInputPoller::IControllerSink {
public:
    // Gated Digital Filtering Semantics:
    // Every digital input (all standard buttons and triggers in digital mode) is treated with a
    // pending -> promoted state machine. On a rising edge we record the time but we DO NOT expose
    // a HIGH state yet. Only after the press has lasted at least _digital_max seconds does it
    // become "promoted" (active) and appear in the outgoing button mask (and trigger analog
    // value 1.0). If released before promotion, the pulse is fully suppressed (never visible to
    // consumers nor virtual device). This eliminates transient micro-presses/ghost pulses at the
    // cost of adding up to _digital_max latency before a legitimate press becomes visible.
    // Analog spike suppression still applies independently to stick axes and to triggers when
    // they are in analog mode.
    FilteredForwarder()
        : _filtered_rings{ SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
                           SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
                           SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
                           SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
                           SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19) } {
        _client = vigem_alloc();
        if (!_client) { _status = "alloc failed"; return; }
        VIGEM_ERROR err = vigem_connect(_client);
        if (!VIGEM_SUCCESS(err)) { _status = "connect failed"; return; }
        _target = vigem_target_x360_alloc();
        if (!_target) { _status = "target alloc failed"; return; }
        err = vigem_target_add(_client, _target);
        if (!VIGEM_SUCCESS(err)) { _status = "target add failed"; return; }
        _ready = true; _status = "Ready";
        for (auto &f : _signal_filter) f.store(true, std::memory_order_relaxed);
    }
    ~FilteredForwarder() override {
        if (_target && _client) {
            vigem_target_remove(_client, _target);
            vigem_target_free(_target);
        }
        if (_client) vigem_free(_client);
    }
    void set_params(float analog_delta, double digital_max_sec) {
        std::lock_guard<std::mutex> lk(_mtx);
        _analog_delta = analog_delta;
        _digital_max = digital_max_sec;
    }
    void set_filter_signals(const std::array<bool, SignalCount>& arr) {
        for (size_t i=0;i<SignalCount;++i) _signal_filter[i].store(arr[i], std::memory_order_release);
    }
    void set_trigger_modes(bool left_digital, bool right_digital) {
        _lt_digital.store(left_digital, std::memory_order_release);
        _rt_digital.store(right_digital, std::memory_order_release);
    }
    void enable_filter(bool e) { _filter_enabled.store(e, std::memory_order_release); }
    void enable_output(bool e) { if (_ready) _enabled.store(e, std::memory_order_release); else _enabled.store(false, std::memory_order_release); }
    bool output_enabled() const { return _enabled.load(std::memory_order_acquire); }
    const char* backend_status() const { return _status.c_str(); }
    const char* last_update_status() const { return _last_update_status.c_str(); }
    void trigger_test_pulse() { _inject_test.store(true, std::memory_order_release); }
    void set_window_seconds(double w) { _window_seconds.store(w, std::memory_order_release); }
    double window_seconds() const { return _window_seconds.load(std::memory_order_acquire); }
    void snapshot_filtered(Signal sig, std::vector<Sample>& out) const {
        double lt = _latest_time_filtered.load(std::memory_order_acquire);
        double win = _window_seconds.load(std::memory_order_acquire);
        _filtered_rings[(size_t)sig].snapshot(lt, win, out);
    }
    void snapshot_filtered_with_baseline(Signal sig, std::vector<Sample>& out) const {
        double lt = _latest_time_filtered.load(std::memory_order_acquire);
        double win = _window_seconds.load(std::memory_order_acquire);
        _filtered_rings[(size_t)sig].snapshot_with_baseline(lt, win, out);
    }
    double latest_filtered_time() const { return _latest_time_filtered.load(std::memory_order_acquire); }
    void clear_filtered() {
        for (auto &r : _filtered_rings) r.clear();
        _latest_time_filtered.store(0.0, std::memory_order_release);
    }

    void process(double t, const XInputPoller::ControllerState& s) override {
        if (!_ready || !_enabled.load(std::memory_order_acquire)) return;
        XInputPoller::ControllerState cur = s;
        if (_inject_test.exchange(false, std::memory_order_acq_rel)) {
            // Overwrite with a recognizable pattern
            cur.lx = -1.0f; cur.ly = 1.0f; // extreme corners
            cur.rx = 1.0f; cur.ry = -1.0f;
            cur.lt = 1.0f; cur.rt = 1.0f;
            // Set A+B+X+Y and shoulders
            cur.buttons |= (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
                            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        }
        // If trigger digital mode active, threshold first so analog spike clamp doesn't treat binary edge as spike.
        bool ltDig = _lt_digital.load(std::memory_order_acquire);
        bool rtDig = _rt_digital.load(std::memory_order_acquire);
        if (ltDig) cur.lt = cur.lt >= 0.5f ? 1.0f : 0.0f;
        if (rtDig) cur.rt = cur.rt >= 0.5f ? 1.0f : 0.0f;
        if (_filter_enabled.load(std::memory_order_acquire)) {
            apply_filter(t, cur, ltDig, rtDig);
        }
        {
            _filtered_rings[(size_t)Signal::LeftX].push(t, cur.lx);
            _filtered_rings[(size_t)Signal::LeftY].push(t, cur.ly);
            _filtered_rings[(size_t)Signal::RightX].push(t, cur.rx);
            _filtered_rings[(size_t)Signal::RightY].push(t, cur.ry);
            _filtered_rings[(size_t)Signal::LeftTrigger].push(t, cur.lt);
            _filtered_rings[(size_t)Signal::RightTrigger].push(t, cur.rt);
            auto push_btn = [&](Signal sig, uint16_t mask) {
                _filtered_rings[(size_t)sig].push(t, (cur.buttons & mask) ? 1.0f : 0.0f);
            };
            push_btn(Signal::LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER);
            push_btn(Signal::RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER);
            push_btn(Signal::A, XINPUT_GAMEPAD_A);
            push_btn(Signal::B, XINPUT_GAMEPAD_B);
            push_btn(Signal::X, XINPUT_GAMEPAD_X);
            push_btn(Signal::Y, XINPUT_GAMEPAD_Y);
            push_btn(Signal::StartBtn, XINPUT_GAMEPAD_START);
            push_btn(Signal::BackBtn, XINPUT_GAMEPAD_BACK);
            push_btn(Signal::LeftThumbBtn, XINPUT_GAMEPAD_LEFT_THUMB);
            push_btn(Signal::RightThumbBtn, XINPUT_GAMEPAD_RIGHT_THUMB);
            push_btn(Signal::DPadUp, XINPUT_GAMEPAD_DPAD_UP);
            push_btn(Signal::DPadDown, XINPUT_GAMEPAD_DPAD_DOWN);
            push_btn(Signal::DPadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
            push_btn(Signal::DPadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
            _latest_time_filtered.store(t, std::memory_order_release);
        }
        auto to_short = [](float v){ if (v>1) v=1; if (v<-1) v=-1; return (int16_t)(v>=0? v*32767.0f : v*32768.0f); };
        auto to_trig = [](float v){ if (v<0) v=0; if (v>1) v=1; return (uint8_t)(v*255.0f + 0.5f); };
        XUSB_REPORT rep{};
        rep.wButtons = cur.buttons;
        rep.bLeftTrigger = to_trig(cur.lt);
        rep.bRightTrigger = to_trig(cur.rt);
        rep.sThumbLX = to_short(cur.lx);
        rep.sThumbLY = to_short(-cur.ly);
        rep.sThumbRX = to_short(cur.rx);
        rep.sThumbRY = to_short(-cur.ry);
        auto err = vigem_target_x360_update(_client, _target, rep);
        if (!VIGEM_SUCCESS(err)) {
            _last_update_status = format_error(err);
        } else if(!_last_update_status.empty()) {
            _last_update_status.clear();
        }
    }
private:
    static std::string format_error(VIGEM_ERROR err) {
        if (err == VIGEM_ERROR_NONE) return std::string();
        switch (err) {
            case VIGEM_ERROR_BUS_NOT_FOUND: return "BUS_NOT_FOUND"; 
            case VIGEM_ERROR_NO_FREE_SLOT: return "NO_FREE_SLOT"; 
            case VIGEM_ERROR_INVALID_TARGET: return "INVALID_TARGET"; 
            case VIGEM_ERROR_REMOVAL_FAILED: return "REMOVAL_FAILED"; 
            case VIGEM_ERROR_ALREADY_CONNECTED: return "ALREADY_CONNECTED"; 
            case VIGEM_ERROR_TARGET_UNINITIALIZED: return "TARGET_UNINITIALIZED"; 
            case VIGEM_ERROR_TARGET_NOT_PLUGGED_IN: return "TARGET_NOT_PLUGGED_IN"; 
            case VIGEM_ERROR_BUS_VERSION_MISMATCH: return "BUS_VERSION_MISMATCH"; 
            case VIGEM_ERROR_BUS_ACCESS_FAILED: return "BUS_ACCESS_FAILED"; 
            case VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED: return "CALLBACK_ALREADY_REGISTERED"; 
            case VIGEM_ERROR_CALLBACK_NOT_FOUND: return "CALLBACK_NOT_FOUND"; 
            case VIGEM_ERROR_BUS_ALREADY_CONNECTED: return "BUS_ALREADY_CONNECTED"; 
            case VIGEM_ERROR_BUS_INVALID_HANDLE: return "BUS_INVALID_HANDLE"; 
            case VIGEM_ERROR_XUSB_USERINDEX_OUT_OF_RANGE: return "XUSB_INDEX_OUT_OF_RANGE"; 
            case VIGEM_ERROR_INVALID_PARAMETER: return "INVALID_PARAMETER"; 
            case VIGEM_ERROR_NOT_SUPPORTED: return "NOT_SUPPORTED"; 
            case VIGEM_ERROR_WINAPI: return "WINAPI_ERROR"; 
            case VIGEM_ERROR_TIMED_OUT: return "TIMED_OUT"; 
            case VIGEM_ERROR_IS_DISPOSING: return "IS_DISPOSING"; 
            default: break; }
        char buf[32]; sprintf_s(buf, "ERR_%08X", (unsigned)err); return buf;
    }
    void apply_filter(double t, XInputPoller::ControllerState& cs, bool ltDig, bool rtDig) {
        std::lock_guard<std::mutex> lk(_mtx);
        // Analog spike suppression (skip if signal filtering disabled)
        if (_have_prev) {
            auto clamp_delta = [&](float &cur, float prev, Signal sig){
                if (!_signal_filter[(size_t)sig].load(std::memory_order_acquire)) return;
                float dv = fabsf(cur - prev); if (dv >= _analog_delta) cur = prev;
            };
            clamp_delta(cs.lx, _prev.lx, Signal::LeftX);
            clamp_delta(cs.ly, _prev.ly, Signal::LeftY);
            clamp_delta(cs.rx, _prev.rx, Signal::RightX);
            clamp_delta(cs.ry, _prev.ry, Signal::RightY);
            if (!ltDig) clamp_delta(cs.lt, _prev.lt, Signal::LeftTrigger);
            if (!rtDig) clamp_delta(cs.rt, _prev.rt, Signal::RightTrigger);
        }
        // Build unified button array (include digital triggers mapped to high indices 14/15)
        constexpr int BTN_COUNT = 16;
        bool btn_now[BTN_COUNT] = {false};
        for (int bit=0; bit<BTN_COUNT; ++bit) btn_now[bit] = (cs.buttons & (1u<<bit)) != 0;
        // Use reserved/unused bits 10/11 for digital trigger virtual mapping
        const int LT_INDEX = 10; const int RT_INDEX = 11;
        if (ltDig) btn_now[LT_INDEX] = cs.lt > 0.5f;
        if (rtDig) btn_now[RT_INDEX] = cs.rt > 0.5f;
        // Map indices to Signal (best-effort; unused indices map to COUNT)
        static const Signal INDEX_TO_SIGNAL[BTN_COUNT] = {
            Signal::DPadUp, Signal::DPadDown, Signal::DPadLeft, Signal::DPadRight,
            Signal::StartBtn, Signal::BackBtn, Signal::LeftThumbBtn, Signal::RightThumbBtn,
            Signal::LeftShoulder, Signal::RightShoulder, Signal::COUNT, Signal::COUNT,
            Signal::A, Signal::B, Signal::X, Signal::Y // preserve X/Y at 14/15
        };
        // GATED LOGIC (skip entirely if filtering disabled for that signal)
        for (int i=0;i<BTN_COUNT;++i) {
            bool now = btn_now[i]; bool prev = _btn_prev_raw[i];
            double &rise = _rise_time[i]; bool &active = _btn_active[i];
            Signal sig = INDEX_TO_SIGNAL[i];
            bool filtering_enabled = (sig != Signal::COUNT) ? _signal_filter[(size_t)sig].load(std::memory_order_acquire) : true;
            if (!filtering_enabled) {
                // Bypass gating: immediate visibility
                active = now;
                rise = -1.0;
                _btn_prev_raw[i] = now;
                continue;
            }
            // Original gated behavior
            if (now && !prev) {
                rise = t; active = false;
            } else if (now && prev) {
                if (!active && rise > 0.0) {
                    double dur = t - rise;
                    if (dur >= _digital_max) active = true;
                }
            } else if (!now && prev) {
                // release
                active = false;
                rise = -1.0;
            } else {
                rise = -1.0; if (active) active = false;
            }
            _btn_prev_raw[i] = now;
        }
        // Compose output mask (exclude digital trigger indices 14/15)
        uint16_t outMask = 0;
        for (int i=0;i<BTN_COUNT;++i) {
            if (i==LT_INDEX || i==RT_INDEX) continue;
            if (_btn_active[i]) outMask |= (1u<<i);
        }
        cs.buttons = outMask;
        if (ltDig) cs.lt = _btn_active[LT_INDEX] ? 1.0f : 0.0f;
        if (rtDig) cs.rt = _btn_active[RT_INDEX] ? 1.0f : 0.0f;
        _prev = cs; _have_prev = true;
    }

    std::atomic<bool> _filter_enabled{false};
    std::atomic<bool> _enabled{false};
    bool _ready = false;
    std::string _status = "Not initialized";
    std::string _last_update_status; // empty if OK
    PVIGEM_CLIENT _client = nullptr;
    PVIGEM_TARGET _target = nullptr;
    float _analog_delta = 0.25f;
    double _digital_max = 0.005;
    XInputPoller::ControllerState _prev{}; bool _have_prev=false;
    double _rise_time[16] = { -1.0 }; // per-button pending rise time (buttons + digital triggers)
    bool _btn_prev_raw[16] = {false}; // raw instantaneous highs
    bool _btn_active[16] = {false};   // promoted (visible) highs after gating threshold
    std::mutex _mtx;
    std::atomic<bool> _inject_test{false};
    std::atomic<bool> _lt_digital{false};
    std::atomic<bool> _rt_digital{false};
    std::array<std::atomic<bool>, SignalCount> _signal_filter{};
    std::atomic<double> _window_seconds{30.0};
    std::atomic<double> _latest_time_filtered{0.0};
    std::array<SampleRing, SignalCount> _filtered_rings;
};
