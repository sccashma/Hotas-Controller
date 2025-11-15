#pragma once
#include <atomic>
#include <cstdint>
#include "xinput/xinput_poll.hpp"

// Placeholder virtual controller sink. Real implementation would use ViGEmBus or similar.
// For now it stores last filtered state; hook actual backend later.
class VirtualControllerOutput : public XInputPoller::IControllerSink {
public:
    void set_enabled(bool e) { _enabled.store(e, std::memory_order_release); }
    bool enabled() const { return _enabled.load(std::memory_order_acquire); }

    void process(double t, const XInputPoller::ControllerState& state) override {
        if (!enabled()) return;
        _last_time.store(t, std::memory_order_release);
        _last = state; // plain store; data race acceptable for diagnostic viewer (reads are best-effort)
        // TODO: send to virtual device backend
    }

    bool last_state(XInputPoller::ControllerState& out, double& t) const {
        if (!enabled()) return false;
        out = _last; // non-atomic snapshot
        t = _last_time.load(std::memory_order_acquire);
        return true;
    }
private:
    std::atomic<bool> _enabled{false};
    std::atomic<double> _last_time{0.0};
    XInputPoller::ControllerState _last{}; // non-atomic; single-writer multi-reader benign tearing risk
};
