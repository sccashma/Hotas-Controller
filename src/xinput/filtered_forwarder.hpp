#pragma once
#include "xinput_poll.hpp"
#include <ViGEm/Client.h>
#include <Xinput.h>
#include <mutex>
#include <atomic>
#include <string>

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
    FilteredForwarder() {
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
    void set_params(float analog_delta, double digital_max_sec) {
        std::lock_guard<std::mutex> lk(_mtx);
        _analog_delta = analog_delta;
        _digital_max = digital_max_sec;
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
        // Analog spike suppression (only for analog paths; digital triggers handled as buttons)
        if (_have_prev) {
            auto clamp_delta = [&](float &cur, float prev){ float dv = fabsf(cur - prev); if (dv >= _analog_delta) cur = prev; };
            clamp_delta(cs.lx, _prev.lx); clamp_delta(cs.ly, _prev.ly); clamp_delta(cs.rx, _prev.rx); clamp_delta(cs.ry, _prev.ry);
            if (!ltDig) clamp_delta(cs.lt, _prev.lt); if (!rtDig) clamp_delta(cs.rt, _prev.rt);
        }
        // Build unified button array (include digital triggers mapped to high indices 14/15)
        constexpr int BTN_COUNT = 16;
        bool btn_now[BTN_COUNT] = {false};
        for (int bit=0; bit<BTN_COUNT; ++bit) btn_now[bit] = (cs.buttons & (1u<<bit)) != 0;
        const int LT_INDEX = 14; const int RT_INDEX = 15;
        if (ltDig) btn_now[LT_INDEX] = cs.lt > 0.5f; // treat thresholded value as button
        if (rtDig) btn_now[RT_INDEX] = cs.rt > 0.5f;
        // GATED LOGIC: A button press is not exposed (promoted) until held for >= _digital_max seconds.
        for (int i=0;i<BTN_COUNT;++i) {
            bool now = btn_now[i]; bool prev = _btn_prev_raw[i];
            double &rise = _rise_time[i]; bool &active = _btn_active[i];
            if (now && !prev) {
                // start pending window
                rise = t; active = false; // not yet visible
            } else if (now && prev) {
                if (!active && rise > 0.0) {
                    double dur = t - rise;
                    if (dur >= _digital_max) {
                        active = true; // promote
                    }
                }
            } else if (!now && prev) {
                // falling edge
                if (!active) {
                    // short pulse suppressed (never promoted)
                } else {
                    // legitimate press ends
                    active = false;
                }
                rise = -1.0;
            } else {
                // idle low
                rise = -1.0; if (active) { active = false; }
            }
            _btn_prev_raw[i] = now;
        }
        // Compose output mask from promoted (active) buttons only (exclude indices 14/15 reserved for digital triggers).
        uint16_t outMask = 0; 
        for (int i=0;i<BTN_COUNT; ++i) {
            if (i >= 14) continue; // strip synthetic trigger indices
            if (_btn_active[i]) outMask |= (1u<<i);
        }
        cs.buttons = outMask;
        // Digital triggers analog fields reflect gated active state (1 after promotion, else 0). Analog triggers stay analog.
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
};
