#pragma once
#include <vector>
#include "xinput/xinput_poll.hpp"

struct PlotConfig {
    double window_seconds = 60.0;   // rolling window length in seconds
    int downsample_max = 8000;       // max points per plot after stride downsampling
    bool filter_mode = false;        // enable anomaly highlighting
    // Analog spike detection: absolute delta threshold (value jump) and z-score style optional later
    float analog_spike_delta = 0.25f; // jump > this between consecutive samples counts as spike
    float analog_spike_return = 0.15f; // optional hysteresis when returning (future use)
    // Digital noise: very short pulses (press then release) shorter than this (seconds) considered ghost
    double digital_pulse_max = 0.005; // 5 ms default
};

class PlotsPanel {
public:
    PlotsPanel(XInputPoller& poller, PlotConfig cfg) : _poller(poller), _cfg(cfg) {}
    void draw();
    void set_window_seconds(double w) { _cfg.window_seconds = w; }
    double window_seconds() const { return _cfg.window_seconds; }
    void set_filter_mode(bool enabled) { _cfg.filter_mode = enabled; }
    void set_filter_thresholds(float analog_delta, float analog_return, double digital_pulse_max) {
        _cfg.analog_spike_delta = analog_delta;
        _cfg.analog_spike_return = analog_return;
        _cfg.digital_pulse_max = digital_pulse_max;
    }
    void set_trigger_digital(bool left, bool right) { _left_trigger_digital = left; _right_trigger_digital = right; }
    bool left_trigger_digital() const { return _left_trigger_digital; }
    bool right_trigger_digital() const { return _right_trigger_digital; }
private:
    void draw_signal(Signal sig, const char* label, bool analog, float y_min, float y_max);
    void draw_signals_group(const char* plot_label, const std::vector<std::pair<Signal,const char*>>& signals, float y_min, float y_max);
    void draw_signals_group_edges(const char* plot_label, const std::vector<std::pair<Signal,const char*>>& signals, float y_min, float y_max);
    void build_step_series(const std::vector<Sample>& in, double t0, double window_end, std::vector<double>& x, std::vector<double>& y);
    XInputPoller& _poller;
    PlotConfig _cfg;
    std::vector<Sample> _tmp; // reused buffer to avoid reallocations
    // Working buffers for anomaly markers
    std::vector<double> _anomaly_x; 
    std::vector<double> _anomaly_y; 
    bool _left_trigger_digital = false;
    bool _right_trigger_digital = false;
};
