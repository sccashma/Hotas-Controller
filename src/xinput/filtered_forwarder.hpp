#pragma once
#include "xinput_poll.hpp"
#include <ViGEm/Client.h>
#include <Xinput.h>
#include <mutex>
#include <atomic>
#include <array>
#include <string>
#include <cstdlib>
#include <fstream>

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
    }
    ~FilteredForwarder() override {
        if (_target && _client) {
            vigem_target_remove(_client, _target);
            vigem_target_free(_target);
        }
        if (_client) vigem_free(_client);
    }
    void set_params(float analog_delta_pct, double digital_max_sec) {
        std::lock_guard<std::mutex> lk(_mtx);
        _analog_rate_pct = analog_delta_pct;
        _digital_max = digital_max_sec;
    }
    // Set per-signal filter modes: 0=none, 1=digital, 2=analog
    void set_filter_modes(const std::array<int, SignalCount>& modes) {
        for (size_t i=0;i<SignalCount;++i) _signal_mode[i].store(modes[i], std::memory_order_release);
    }
    void set_trigger_modes(bool left_digital, bool right_digital) {
        _lt_digital.store(left_digital, std::memory_order_release);
        _rt_digital.store(right_digital, std::memory_order_release);
    }
    void enable_filter(bool e) { _filter_enabled.store(e, std::memory_order_release); }
    void enable_output(bool e) {
        if (e && !_ready) ensure_ready();
        if (_ready) {
            if (e) {
                // Re-plug target to nudge system enumeration
                VIGEM_ERROR replug_err = VIGEM_ERROR_NONE;
                if (_client && _target) {
                    vigem_target_remove(_client, _target);
                    replug_err = vigem_target_add(_client, _target);
                }
                if (!VIGEM_SUCCESS(replug_err)) {
                    _last_update_status = format_error(replug_err);
                }
            }
            _enabled.store(e, std::memory_order_release);
            if (e) {
                // Perform a one-time neutral update to ensure the virtual device is visible to the system
                XUSB_REPORT rep{};
                rep.wButtons = 0;
                rep.bLeftTrigger = 0;
                rep.bRightTrigger = 0;
                rep.sThumbLX = 0;
                rep.sThumbLY = 0;
                rep.sThumbRX = 0;
                rep.sThumbRY = 0;
                VIGEM_ERROR err = vigem_target_x360_update(_client, _target, rep);
                if (!VIGEM_SUCCESS(err)) {
                    _last_update_status = format_error(err);
                } else {
                    if(!_last_update_status.empty()) _last_update_status.clear();
                }
            }
        } else {
            _enabled.store(false, std::memory_order_release);
        }
    }
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
        XInputPoller::ControllerState cur = s;
        if (_inject_test.exchange(false, std::memory_order_acq_rel)) {
            cur.lx = -1.0f; cur.ly = 1.0f;
            cur.rx = 1.0f; cur.ry = -1.0f;
            cur.lt = 1.0f; cur.rt = 1.0f;
            cur.buttons |= (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
                            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        }

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
            auto push_btn = [&](Signal sig, uint16_t mask) { _filtered_rings[(size_t)sig].push(t, (cur.buttons & mask) ? 1.0f : 0.0f); };
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
        if (_enabled.load(std::memory_order_acquire)) {
            VIGEM_ERROR err = vigem_target_x360_update(_client, _target, rep);
            if (!VIGEM_SUCCESS(err)) { _last_update_status = format_error(err); }
            else if(!_last_update_status.empty()) { _last_update_status.clear(); }
        } else {
            if (!_last_update_status.empty()) _last_update_status.clear();
        }
    }
private:
    void ensure_ready() {
        if (_ready) return;
        _client = vigem_alloc();
        if (!_client) { _status = "alloc failed"; return; }
        VIGEM_ERROR err = vigem_connect(_client);
        if (!VIGEM_SUCCESS(err)) { _status = "connect failed"; return; }
        _target = vigem_target_x360_alloc();
        if (!_target) { _status = "target alloc failed"; return; }
        err = vigem_target_add(_client, _target);
        if (!VIGEM_SUCCESS(err)) { _status = "target add failed"; return; }
        _ready = true; _status = "Ready";
    }
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
        if (!_have_prev) {
            _prev = cs;
            _have_prev = true;
            return;
        }
        
        // Apply per-signal analog or digital filtering based on mode
        // Analog: rate limiter — cap per-sample change to percent of full range
        auto apply_analog_filter = [&](float &cur, float prev) {
            float dv = cur - prev;
            float range = ((prev >= 0.0f && prev <= 1.0f) && (cur >= 0.0f && cur <= 1.0f)) ? 1.0f : 2.0f;
            float max_step = (_analog_rate_pct / 100.0f) * range;
            if (dv > max_step) cur = prev + max_step;
            else if (dv < -max_step) cur = prev - max_step;
        };
        
        auto apply_digital_filter = [&](bool &now, bool prev, int btn_idx) {
            double &rise = _rise_time[btn_idx];
            bool &active = _btn_active[btn_idx];
            if (now && !prev) {
                rise = t; active = false; // rising edge detected, not yet promoted
            } else if (now && prev) {
                if (!active && rise >= 0.0) {
                    double dur = t - rise;
                    if (dur >= _digital_max) active = true; // promoted after duration
                }
            } else if (!now && prev) {
                active = false; rise = -1.0; // release
            } else {
                rise = -1.0; active = false; // stable low
            }
            _btn_prev_raw[btn_idx] = now;
        };
        
        // Process analog signals (stick axes, triggers in analog mode)
        {
            int mode_lx = _signal_mode[(size_t)Signal::LeftX].load(std::memory_order_acquire);
            int mode_ly = _signal_mode[(size_t)Signal::LeftY].load(std::memory_order_acquire);
            int mode_rx = _signal_mode[(size_t)Signal::RightX].load(std::memory_order_acquire);
            int mode_ry = _signal_mode[(size_t)Signal::RightY].load(std::memory_order_acquire);
            if (mode_lx == 2) apply_analog_filter(cs.lx, _prev.lx);
            if (mode_ly == 2) apply_analog_filter(cs.ly, _prev.ly);
            if (mode_rx == 2) apply_analog_filter(cs.rx, _prev.rx);
            if (mode_ry == 2) apply_analog_filter(cs.ry, _prev.ry);
            
            // Triggers: apply analog filter only if not in digital mode
            if (!ltDig) {
                int mode_lt = _signal_mode[(size_t)Signal::LeftTrigger].load(std::memory_order_acquire);
                if (mode_lt == 2) apply_analog_filter(cs.lt, _prev.lt);
            }
            if (!rtDig) {
                int mode_rt = _signal_mode[(size_t)Signal::RightTrigger].load(std::memory_order_acquire);
                if (mode_rt == 2) apply_analog_filter(cs.rt, _prev.rt);
            }
        }
        
        // Process digital signals (buttons, digital triggers)
        constexpr int BTN_COUNT = 16;
        bool btn_now[BTN_COUNT] = {false};
        for (int bit=0; bit<BTN_COUNT; ++bit) btn_now[bit] = (cs.buttons & (1u<<bit)) != 0;
        
        // Map digital triggers to virtual indices
        const int LT_INDEX = 10; const int RT_INDEX = 11;
        if (ltDig) btn_now[LT_INDEX] = cs.lt > 0.5f;
        if (rtDig) btn_now[RT_INDEX] = cs.rt > 0.5f;
        
        // Index to Signal mapping
        static const Signal INDEX_TO_SIGNAL[BTN_COUNT] = {
            Signal::DPadUp, Signal::DPadDown, Signal::DPadLeft, Signal::DPadRight,
            Signal::StartBtn, Signal::BackBtn, Signal::LeftThumbBtn, Signal::RightThumbBtn,
            Signal::LeftShoulder, Signal::RightShoulder, Signal::COUNT, Signal::COUNT,
            Signal::A, Signal::B, Signal::X, Signal::Y
        };
        
        // Process each button with its filter mode
        for (int i=0; i<BTN_COUNT; ++i) {
            Signal sig = INDEX_TO_SIGNAL[i];
            int mode = (sig != Signal::COUNT) ? _signal_mode[(size_t)sig].load(std::memory_order_acquire) : 0;
            bool now = btn_now[i];
            bool prev = _btn_prev_raw[i];
            
            if (mode == 0) {
                // None: pass raw (immediate)
                _btn_active[i] = now;
                _btn_prev_raw[i] = now;
            } else if (mode == 1) {
                // Digital: gated debounce
                apply_digital_filter(now, prev, i);
            }
            // Mode 2 (analog) not applicable to digital signals; ignore
        }
        
        // Compose output button mask
        uint16_t outMask = 0;
        for (int i=0; i<BTN_COUNT; ++i) {
            if (i == LT_INDEX || i == RT_INDEX) continue; // skip virtual indices
            if (_btn_active[i]) outMask |= (1u<<i);
        }
        cs.buttons = outMask;
        if (ltDig) cs.lt = _btn_active[LT_INDEX] ? 1.0f : 0.0f;
        if (rtDig) cs.rt = _btn_active[RT_INDEX] ? 1.0f : 0.0f;
        
        _prev = cs;
    }

    std::atomic<bool> _filter_enabled{false};
    std::atomic<bool> _enabled{false};
    bool _ready = false;
    std::string _status = "Not initialized";
    std::string _last_update_status; // empty if OK
    PVIGEM_CLIENT _client = nullptr;
    PVIGEM_TARGET _target = nullptr;
    float _analog_rate_pct = 5.0f;
    double _digital_max = 0.005;
    XInputPoller::ControllerState _prev{}; bool _have_prev=false;
    double _rise_time[16] = { -1.0 }; // per-button pending rise time (buttons + digital triggers)
    bool _btn_prev_raw[16] = {false}; // raw instantaneous highs
    bool _btn_active[16] = {false};   // promoted (visible) highs after gating threshold
    std::mutex _mtx;
    std::atomic<bool> _inject_test{false};
    std::atomic<bool> _lt_digital{false};
    std::atomic<bool> _rt_digital{false};
    // Per-signal filter mode: 0=none, 1=digital, 2=analog
    std::array<std::atomic<int>, SignalCount> _signal_mode{};
    std::atomic<double> _window_seconds{30.0};
    std::atomic<double> _latest_time_filtered{0.0};
    std::array<SampleRing, SignalCount> _filtered_rings;
};
