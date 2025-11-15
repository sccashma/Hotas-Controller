// Docking enabled via target compile definitions (see CMakeLists.txt)
// Cleaned duplicate implementation; keeping single version with style customization
// Digital Signal Edge Rendering:
// For ABXY and D-Pad groups we use an edge-based representation built from
// snapshot_with_baseline(): we keep the last sample before window start as
// a baseline plus every transition inside the window. We then synthesize
// a step series so that very short pulses (ghost inputs) are always
// visible (assuming at least one poll captured them) without needing to
// plot every polled sample. This prevents stride downsampling from
// accidentally removing narrow pulses while keeping per-frame point
// counts low.
#include "plots_panel.hpp"
#include <implot.h>
#include <algorithm>
#include <cmath>

static void stride_downsample(const std::vector<Sample>& in, int max_points, std::vector<double>& xt, std::vector<double>& yv) {
    xt.clear(); yv.clear();
    if ((int)in.size() <= max_points || max_points <= 0) {
        xt.reserve(in.size()); yv.reserve(in.size());
        for (auto &s : in) { xt.push_back(s.t); yv.push_back(s.v); }
        return;
    }
    double step = (double)in.size() / (double)max_points;
    xt.reserve(max_points+1); yv.reserve(max_points+1);
    double i = 0.0; size_t n = in.size();
    while ((size_t)i < n) {
        size_t idx = (size_t)i;
        const auto &s = in[idx];
        xt.push_back(s.t); yv.push_back(s.v);
        i += step;
    }
    if (xt.back() != in.back().t) { xt.push_back(in.back().t); yv.push_back(in.back().v); }
}

void PlotsPanel::draw_signal(Signal sig, const char* label, bool analog, float y_min, float y_max) {
    _poller.snapshot(sig, _tmp);
    if (_tmp.empty()) return;
    double latest = _poller.latest_time();
    double t0 = latest - _cfg.window_seconds;
    std::vector<double> x; std::vector<double> y;
    stride_downsample(_tmp, _cfg.downsample_max, x, y);
    for (auto &vx : x) vx -= t0; // shift to 0..window
    if (ImPlot::BeginPlot(label, ImVec2(-1,150), ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("Time (s)", "Value", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, _cfg.window_seconds, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);
        ImPlot::PlotLine(label, x.data(), y.data(), (int)x.size());
        if (_cfg.filter_mode && analog) {
            _anomaly_x.clear(); _anomaly_y.clear();
            // Simple spike heuristic: large absolute delta vs previous raw sample (not downsampled)
            for (size_t i = 1; i < _tmp.size(); ++i) {
                float dv = fabsf(_tmp[i].v - _tmp[i-1].v);
                if (dv >= _cfg.analog_spike_delta) {
                    double tx = _tmp[i].t - t0;
                    if (tx >= 0.0 && tx <= _cfg.window_seconds) {
                        _anomaly_x.push_back(tx);
                        _anomaly_y.push_back(_tmp[i].v);
                    }
                }
            }
            if (!_anomaly_x.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0f, ImVec4(1,0,0,1), 1.0f, ImVec4(1,0,0,1));
                ImPlot::PlotScatter("Spikes", _anomaly_x.data(), _anomaly_y.data(), (int)_anomaly_x.size());
            }
        }
        ImPlot::EndPlot();
    }
}

void PlotsPanel::draw_signals_group(const char* plot_label, const std::vector<std::pair<Signal,const char*>>& signals, float y_min, float y_max) {
    // Gather all snapshots first to keep time base consistent (each may have slightly different lengths)
    double latest = _poller.latest_time();
    double t0 = latest - _cfg.window_seconds;
    struct Series { std::vector<double> x; std::vector<double> y; const char* label; };
    std::vector<Series> series; series.reserve(signals.size());
    for (auto &sp : signals) {
        _poller.snapshot(sp.first, _tmp);
        if (_tmp.empty()) continue;
        Series s; s.label = sp.second;
        stride_downsample(_tmp, _cfg.downsample_max, s.x, s.y);
        for (auto &vx : s.x) vx -= t0;
        series.push_back(std::move(s));
    }
    if (series.empty()) return;
    if (ImPlot::BeginPlot(plot_label, ImVec2(-1,150), ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("Time (s)", "Value", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, _cfg.window_seconds, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);
        // Use automatic colors; user can distinguish by legend
        for (auto &s : series) {
            ImPlot::PlotLine(s.label, s.x.data(), s.y.data(), (int)s.x.size());
        }
        if (_cfg.filter_mode) {
            // For grouped analog signals (assume all analog signals in this group)
            _anomaly_x.clear(); _anomaly_y.clear();
            for (auto &sp : signals) {
                _poller.snapshot(sp.first, _tmp);
                for (size_t i = 1; i < _tmp.size(); ++i) {
                    float dv = fabsf(_tmp[i].v - _tmp[i-1].v);
                    if (dv >= _cfg.analog_spike_delta) {
                        double tx = _tmp[i].t - t0;
                        if (tx >= 0.0 && tx <= _cfg.window_seconds) {
                            _anomaly_x.push_back(tx);
                            _anomaly_y.push_back(_tmp[i].v);
                        }
                    }
                }
            }
            if (!_anomaly_x.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Cross, 5.0f, ImVec4(1,0,0,1), 1.0f, ImVec4(1,0,0,1));
                ImPlot::PlotScatter("Spikes", _anomaly_x.data(), _anomaly_y.data(), (int)_anomaly_x.size());
            }
        }
        ImPlot::EndPlot();
    }
}

// Build step series from baseline+edges sample array. Assumes 'in' is time-ordered.
void PlotsPanel::build_step_series(const std::vector<Sample>& in, double t0, double window_end, std::vector<double>& x, std::vector<double>& y) {
    x.clear(); y.clear();
    if (in.empty()) return;
    // Start with first sample (baseline)
    float current = in.front().v;
    double prev_t = in.front().t;
    // Anchor at window start if baseline precedes it
    if (prev_t < t0) {
        prev_t = t0;
    }
    x.push_back(prev_t - t0); y.push_back(current);
    for (size_t i = 1; i < in.size(); ++i) {
        const auto &s = in[i];
        double t = s.t;
        if (t < t0) continue; // still before window
        if (s.v == current) continue; // no change (shouldn't happen if edges only, but safe)
        // Vertical step: duplicate time with old then new value
        double rel_t = t - t0;
        x.push_back(rel_t); y.push_back(current); // hold previous until change
        current = s.v;
        x.push_back(rel_t); y.push_back(current); // new state
    }
    // Extend to window end
    if (!x.empty() && x.back() < window_end) {
        x.push_back(window_end); y.push_back(y.back());
    }
}

void PlotsPanel::draw_signals_group_edges(const char* plot_label, const std::vector<std::pair<Signal,const char*>>& signals, float y_min, float y_max) {
    double latest = _poller.latest_time();
    double t0 = latest - _cfg.window_seconds;
    double window_end = _cfg.window_seconds;
    struct Series { std::vector<double> x; std::vector<double> y; const char* label; };
    std::vector<Series> series; series.reserve(signals.size());
    std::vector<Sample> local;
    for (auto &sp : signals) {
        _poller.snapshot_with_baseline(sp.first, local);
        if (local.empty()) continue;
        Series s; s.label = sp.second;
        build_step_series(local, t0, window_end, s.x, s.y);
        if (!s.x.empty()) series.push_back(std::move(s));
    }
    if (series.empty()) return;
    if (ImPlot::BeginPlot(plot_label, ImVec2(-1,150), ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("Time (s)", "Value", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, _cfg.window_seconds, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);
        for (auto &s : series) {
            ImPlot::PlotLine(s.label, s.x.data(), s.y.data(), (int)s.x.size());
        }
        if (_cfg.filter_mode) {
            // New logic: treat edges as alternating states; measure HIGH intervals directly.
            _anomaly_x.clear(); _anomaly_y.clear();
            for (auto &sp : signals) {
                _poller.snapshot_with_baseline(sp.first, local);
                if (local.size() < 2) continue;
                // Determine baseline state
                float current = local[0].v; // baseline
                double high_start = -1.0;
                for (size_t i = 1; i < local.size(); ++i) {
                    float next = local[i].v;
                    if (next == current) continue; // no state change
                    double t_edge = local[i].t;
                    // Transition detected: current -> next
                    if (current < 0.5f && next > 0.5f) { // rising edge
                        high_start = t_edge; // start HIGH interval
                    } else if (current > 0.5f && next < 0.5f) { // falling edge
                        if (high_start >= 0.0) {
                            double dur = t_edge - high_start;
                            if (dur > 0 && dur <= _cfg.digital_pulse_max) {
                                double tx = (high_start + t_edge) * 0.5 - t0;
                                if (tx >= 0.0 && tx <= _cfg.window_seconds) {
                                    _anomaly_x.push_back(tx);
                                    _anomaly_y.push_back(1.0);
                                }
                            }
                            high_start = -1.0;
                        }
                    }
                    current = next;
                }
            }
            if (!_anomaly_x.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 6.0f, ImVec4(1,0.5f,0,1), 1.0f, ImVec4(1,0.5f,0,1));
                ImPlot::PlotScatter("Short Pulses", _anomaly_x.data(), _anomaly_y.data(), (int)_anomaly_x.size());
            }
        }
        ImPlot::EndPlot();
    }
}

// (Removed custom trigger drawing; unified logic uses existing helpers below)

void PlotsPanel::draw() {
    if (ImGui::BeginTabBar("signals_tab")) {
        if (ImGui::BeginTabItem("Sticks")) {
            draw_signals_group("Left Stick", {
                {Signal::LeftX, "Left X"}, {Signal::LeftY, "Left Y"}
            }, -1.05f, 1.05f);
            draw_signals_group("Right Stick", {
                {Signal::RightX, "Right X"}, {Signal::RightY, "Right Y"}
            }, -1.05f, 1.05f);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Triggers/Bumpers")) {
            // Digital mode toggles
            ImGui::BeginGroup();
            bool ltDig = _left_trigger_digital; bool rtDig = _right_trigger_digital;
            ImGui::Checkbox("LT Digital", &ltDig); ImGui::SameLine(); ImGui::Checkbox("RT Digital", &rtDig);
            if (ltDig != _left_trigger_digital || rtDig != _right_trigger_digital) { _left_trigger_digital = ltDig; _right_trigger_digital = rtDig; }
            ImGui::EndGroup();
            if (_left_trigger_digital || _right_trigger_digital) {
                // Treat any digital triggers as edge-based digital signals (using baseline+edges) combined with any remaining analog one
                std::vector<std::pair<Signal,const char*>> digitalSeries; digitalSeries.reserve(2);
                if (_left_trigger_digital) digitalSeries.push_back({Signal::LeftTrigger, "Left (D)"});
                if (_right_trigger_digital) digitalSeries.push_back({Signal::RightTrigger, "Right (D)"});
                // Plot digital triggers using the same edge-group function (re-using digital short pulse highlighting logic) by borrowing draw_signals_group_edges mechanics.
                draw_signals_group_edges("Triggers (Digital)", digitalSeries, -0.05f, 1.05f);
                // If one trigger remains analog, plot it separately (regular analog group call with just that one)
                std::vector<std::pair<Signal,const char*>> analogRem;
                if (!_left_trigger_digital) analogRem.push_back({Signal::LeftTrigger, "Left"});
                if (!_right_trigger_digital) analogRem.push_back({Signal::RightTrigger, "Right"});
                if (!analogRem.empty()) {
                    draw_signals_group("Triggers (Analog)", analogRem, -0.05f, 1.05f);
                }
            } else {
                draw_signals_group("Triggers", { {Signal::LeftTrigger, "Left"}, {Signal::RightTrigger, "Right"} }, -0.05f, 1.05f);
            }
            // Group digital bumpers (shoulders). Edge-based to catch very short presses.
            draw_signals_group_edges("Bumpers", {
                {Signal::LeftShoulder, "Left"}, {Signal::RightShoulder, "Right"}
            }, -0.05f, 1.05f);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Buttons/D-Pad")) {
            // Group ABXY
            // Edge-based plotting for digital groups (ABXY, D-Pad)
            draw_signals_group_edges("ABXY", {
                {Signal::A, "A"}, {Signal::B, "B"}, {Signal::X, "X"}, {Signal::Y, "Y"}
            }, -0.05f, 1.05f);
            // Group Start + Back
            draw_signals_group_edges("Start/Back", {
                {Signal::StartBtn, "Start"}, {Signal::BackBtn, "Back"}
            }, -0.05f, 1.05f);
            // Group Thumb buttons
            draw_signals_group_edges("Thumb Buttons", {
                {Signal::LeftThumbBtn, "Left Thumb"}, {Signal::RightThumbBtn, "Right Thumb"}
            }, -0.05f, 1.05f);
            // Group D-Pad
            draw_signals_group_edges("D-Pad", {
                {Signal::DPadUp, "Up"}, {Signal::DPadDown, "Down"}, {Signal::DPadLeft, "Left"}, {Signal::DPadRight, "Right"}
            }, -0.05f, 1.05f);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
