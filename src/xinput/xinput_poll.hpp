#pragma once
#include <cstdint>
#include <array>
#include <string_view>
#include <thread>
#include <atomic>
#include <vector>
#include "core/ring_buffer.hpp"

// Signals enumeration similar to Python version
enum class Signal : uint8_t {
    LeftX, LeftY, RightX, RightY,
    LeftTrigger, RightTrigger,
    LeftShoulder, RightShoulder,
    A, B, X, Y, StartBtn, BackBtn, LeftThumbBtn, RightThumbBtn,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    COUNT
};

constexpr size_t SignalCount = static_cast<size_t>(Signal::COUNT);

struct SignalMeta { const char* name; bool analog; };
inline constexpr std::array<SignalMeta, SignalCount> SIGNAL_META = {{
    {"left_x", true}, {"left_y", true}, {"right_x", true}, {"right_y", true},
    {"left_trigger", true}, {"right_trigger", true},
    {"left_shoulder", false}, {"right_shoulder", false},
    {"a", false}, {"b", false}, {"x", false}, {"y", false}, {"start", false}, {"back", false}, {"left_thumb", false}, {"right_thumb", false},
    {"dpad_up", false}, {"dpad_down", false}, {"dpad_left", false}, {"dpad_right", false}
}};

struct PollStats {
    double effective_hz = 0.0;    // Rolling ~100ms window or EMA hybrid
    double avg_loop_us = 0.0;     // EMA of total loop cost
};

class XInputPoller {
public:
    struct ControllerState {
        float lx, ly, rx, ry;      // -1..1
        float lt, rt;              // 0..1
        uint16_t buttons;          // bitmask using XINPUT_GAMEPAD_* bits
    };
    struct IControllerSink {
        virtual ~IControllerSink() = default;
        virtual void process(double t, const ControllerState& state) = 0; // called from poller thread
    };

    XInputPoller();
    ~XInputPoller();

    void start(int controller_index, double target_hz, double window_seconds);
    void stop();
    void set_controller_index(int idx) { if (idx < 0) idx = 0; if (idx > 3) idx = 3; _controller_index.store(idx, std::memory_order_release); }
    int controller_index() const { return _controller_index.load(std::memory_order_acquire); }

    bool connected() const { return _connected.load(std::memory_order_acquire); }
    PollStats stats() const { return _stats.load(std::memory_order_acquire); }
    double latest_time() const { return _latest_time.load(std::memory_order_acquire); }

    void snapshot(Signal sig, std::vector<Sample>& out) const;
    void snapshot_with_baseline(Signal sig, std::vector<Sample>& out) const;
    // Inject an externally-sourced controller state (e.g. HOTAS reader) into the poller.
    // This will push samples to the internal rings and notify any sink exactly as if
    // the poller had read them itself.
    void inject_state(double t, const ControllerState& state);
    void set_target_hz(double hz) {
        if (hz < 10.0) hz = 10.0; // sensible floor
        if (hz > 8000.0) hz = 8000.0; // clamp ceiling
        _target_hz.store(hz, std::memory_order_release);
    }
    void set_window_seconds(double seconds) { _window_seconds.store(seconds, std::memory_order_release); }
    double window_seconds() const { return _window_seconds.load(std::memory_order_acquire); }
    void clear();
    void set_sink(IControllerSink* sink) { _sink.store(sink, std::memory_order_release); }
    void set_external_input(bool v) { _external_only.store(v, std::memory_order_release); }
    uint64_t samples_captured() const { return _samples_captured.load(std::memory_order_acquire); }

private:
    void run(int controller_index);
    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{false};
    std::atomic<double> _latest_time{0.0};
    std::atomic<double> _target_hz{1000.0};
    std::atomic<double> _window_seconds{30.0};
    std::atomic<PollStats> _stats; // atomic trivially copyable
    std::thread _thread;

    std::array<SampleRing, SignalCount> _rings; // sized by capacity
    std::atomic<IControllerSink*> _sink{nullptr};
    std::atomic<int> _controller_index{0};
    std::atomic<bool> _external_only{false};
    std::atomic<uint64_t> _samples_captured{0}; // total samples processed by polling thread
};
