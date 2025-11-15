#include "xinput_poll.hpp"
#include <windows.h>
#include <Xinput.h>
#include <chrono>
#include <cmath>
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUG__)
#include <immintrin.h>
#endif

#pragma comment(lib, "xinput9_1_0.lib")

XInputPoller::XInputPoller()
    : _rings{ SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
              SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
              SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19),
              SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19), SampleRing(1u<<19) } {
    _stats.store(PollStats{}, std::memory_order_relaxed);
}

XInputPoller::~XInputPoller() { stop(); }

void XInputPoller::start(int controller_index, double target_hz, double window_seconds) {
    if (_running.load()) return;
    _running.store(true);
    _target_hz.store(target_hz);
    _window_seconds.store(window_seconds);
    _thread = std::thread(&XInputPoller::run, this, controller_index);
}

void XInputPoller::stop() {
    if (!_running.exchange(false)) return;
    if (_thread.joinable()) _thread.join();
}

// set_target_hz implemented inline in header with clamping

void XInputPoller::snapshot(Signal sig, std::vector<Sample>& out) const {
    double lt = _latest_time.load(std::memory_order_acquire);
    double window = _window_seconds.load(std::memory_order_acquire);
    _rings[static_cast<size_t>(sig)].snapshot(lt, window, out);
}

void XInputPoller::snapshot_with_baseline(Signal sig, std::vector<Sample>& out) const {
    double lt = _latest_time.load(std::memory_order_acquire);
    double window = _window_seconds.load(std::memory_order_acquire);
    _rings[static_cast<size_t>(sig)].snapshot_with_baseline(lt, window, out);
}

void XInputPoller::clear() {
    for (auto &r : _rings) {
        r.clear();
    }
    _latest_time.store(0.0, std::memory_order_release);
}

void XInputPoller::run(int controller_index) {
    using clock = std::chrono::steady_clock;
    auto to_double = [](clock::time_point tp){ return std::chrono::duration<double>(tp.time_since_epoch()).count(); };

    auto now_tp = clock::now();
    double ema_loop_us = 0.0;
    double window_start_time = to_double(now_tp);
    uint64_t window_polls = 0;
    double last_sample_time = window_start_time;
    double target_hz_cached = _target_hz.load(std::memory_order_relaxed);
    if (target_hz_cached < 10.0) target_hz_cached = 10.0;
    if (target_hz_cached > 8000.0) target_hz_cached = 8000.0;
    double interval = 1.0 / target_hz_cached;
    using duration_d = std::chrono::duration<double>;
    auto interval_ticks = std::chrono::duration_cast<clock::duration>(duration_d(interval));
    auto wake_time = now_tp + interval_ticks; // first wake

    // Polling loop with simple sleep+spin scheduling.

    // Simplified scheduling: basic deadline, per-loop stats update, minimal logic.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // keep slight priority bump

    while (_running.load(std::memory_order_relaxed)) {
        controller_index = _controller_index.load(std::memory_order_relaxed);
        // Refresh target interval only if changed (avoid per-loop division/cast when stable)
        double thz = _target_hz.load(std::memory_order_relaxed);
        if (thz < 10.0) thz = 10.0; else if (thz > 8000.0) thz = 8000.0;
        if (thz != target_hz_cached) {
            target_hz_cached = thz;
            interval = 1.0 / target_hz_cached;
            interval_ticks = std::chrono::duration_cast<clock::duration>(duration_d(interval));
        }

        auto loop_start = clock::now(); // one clock read per loop start

        XINPUT_STATE state{}; DWORD res = XInputGetState(controller_index, &state);
        double t = to_double(loop_start); // reuse loop_start time as timestamp
        if (res != ERROR_SUCCESS) {
            _connected.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            last_sample_time = t;
            wake_time = clock::now() + interval_ticks;
            continue;
        }
        _connected.store(true, std::memory_order_release);

        const XINPUT_GAMEPAD &gp = state.Gamepad;
        // Inline axis normalization to avoid lambda overhead (minor but keeps hot loop lean)
        auto norm_axis_func = [](SHORT v){ return (v >= 0) ? (double)v / 32767.0 : (double)v / 32768.0; };

        // Capture work start to measure just polling + storage
        auto work_start = clock::now();
        _rings[(size_t)Signal::LeftX].push(t, (float)norm_axis_func(gp.sThumbLX));
        _rings[(size_t)Signal::LeftY].push(t, (float)-norm_axis_func(gp.sThumbLY));
        _rings[(size_t)Signal::RightX].push(t, (float)norm_axis_func(gp.sThumbRX));
        _rings[(size_t)Signal::RightY].push(t, (float)-norm_axis_func(gp.sThumbRY));
        _rings[(size_t)Signal::LeftTrigger].push(t, gp.bLeftTrigger / 255.0f);
        _rings[(size_t)Signal::RightTrigger].push(t, gp.bRightTrigger / 255.0f);
        WORD buttons = gp.wButtons;
        auto push_btn = [&](Signal s, WORD mask){ _rings[(size_t)s].push(t, (buttons & mask) ? 1.0f : 0.0f); };
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
        auto work_end = clock::now();

        // Forward raw state to optional sink (filtering applied externally)
        if (auto* sink = _sink.load(std::memory_order_acquire)) {
            ControllerState cs;
            cs.lx = (float)norm_axis_func(gp.sThumbLX);
            cs.ly = (float)-norm_axis_func(gp.sThumbLY);
            cs.rx = (float)norm_axis_func(gp.sThumbRX);
            cs.ry = (float)-norm_axis_func(gp.sThumbRY);
            cs.lt = gp.bLeftTrigger / 255.0f;
            cs.rt = gp.bRightTrigger / 255.0f;
            cs.buttons = gp.wButtons;
            sink->process(t, cs);
        }

        // Stats accumulation (per poll)
        double dt = t - last_sample_time; if (dt > 0) {}
        last_sample_time = t;
        _latest_time.store(t, std::memory_order_release);
        window_polls++;

        auto loop_us_ll = std::chrono::duration_cast<std::chrono::microseconds>(work_end - loop_start).count();
        double loop_us = static_cast<double>(loop_us_ll);
        const double loop_alpha = 0.05;
        ema_loop_us = (ema_loop_us == 0.0) ? loop_us : (1 - loop_alpha) * ema_loop_us + loop_alpha * loop_us;

        // Simple sleep+spin: sleep for remaining, then spin to deadline.
        // Sleep until scheduled wake time (coarse) then spin if a little early
        auto before_wait = clock::now();
        auto sleep_time = wake_time - clock::duration( std::chrono::microseconds(800) ); 
        if (before_wait < sleep_time) {
            std::this_thread::sleep_until(sleep_time);
        }
        while (clock::now() < wake_time) {
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP)
            _mm_pause();
#endif
        }

        auto after_wait = clock::now();
        // Compute overshoot (lateness)
        // (Overshoot no longer tracked for display)

        // Advance deadline in fixed steps to avoid drift accumulation from overshoot.
        wake_time += interval_ticks;
        auto now_after = clock::now();
        if (now_after > wake_time + interval_ticks) {
            wake_time = now_after + interval_ticks; // drift correction
        }

        // Publish stats every loop (simple approach)
        // Publish every ~100ms
        double now_sec_d = to_double(now_after);
        if (now_sec_d - window_start_time >= 0.1) {
            double elapsed = now_sec_d - window_start_time;
            double eff = (elapsed > 0.0) ? (double)window_polls / elapsed : 0.0;
            PollStats ps{};
            ps.effective_hz = eff;
            ps.avg_loop_us = ema_loop_us;
            _stats.store(ps, std::memory_order_release);
            window_start_time = now_sec_d;
            window_polls = 0;
        }
    }
}
