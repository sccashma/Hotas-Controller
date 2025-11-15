#pragma once
#include <vector>
#include <atomic>
#include <cstdint>
#include <span>
#include <mutex>

// Lock-light single-writer multi-reader ring buffer for samples (time,value)
// Writer pushes sequential indices; readers snapshot head and copy out.

struct Sample {
    double t;   // seconds (wall or relative)
    float  v;   // value
};

class SampleRing {
public:
    explicit SampleRing(size_t capacity_pow2)
        : _capacity(capacity_pow2), _mask(capacity_pow2 - 1), _data(capacity_pow2) {}

    void push(double t, float v) {
        const uint64_t idx = _write_index.fetch_add(1, std::memory_order_relaxed);
        _data[idx & _mask] = Sample{t, v};
    }

    // Copy last up to max_seconds of data into out vector; assumes times are monotonic increasing.
    // We pass latest_time to compute cutoff externally for speed.
    void snapshot(double latest_time, double window_seconds, std::vector<Sample>& out) const {
        out.clear();
        uint64_t end = _write_index.load(std::memory_order_acquire);
        if (end == 0) return;
        const uint64_t start = (end > _capacity) ? end - _capacity : 0;
        const double cutoff = latest_time - window_seconds;
        for (uint64_t i = start; i < end; ++i) {
            const Sample &s = _data[i & _mask];
            if (s.t >= cutoff) {
                out.push_back(s);
            }
        }
    }

    // Variant that also includes the last sample immediately prior to the cutoff (baseline)
    void snapshot_with_baseline(double latest_time, double window_seconds, std::vector<Sample>& out) const {
        out.clear();
        uint64_t end = _write_index.load(std::memory_order_acquire);
        if (end == 0) return;
        const uint64_t start = (end > _capacity) ? end - _capacity : 0;
        const double cutoff = latest_time - window_seconds;
        const Sample* baseline = nullptr;
        for (uint64_t i = start; i < end; ++i) {
            const Sample &s = _data[i & _mask];
            if (s.t < cutoff) {
                baseline = &s; // keep most recent before cutoff
                continue;
            }
            if (baseline && (out.empty())) {
                out.push_back(*baseline);
                baseline = nullptr; // inserted once
            }
            out.push_back(s);
        }
        // If nothing in window but we had a baseline, keep it so caller can know stable state
        if (out.empty() && baseline) {
            out.push_back(*baseline);
        }
    }

    uint64_t size() const { return _write_index.load(std::memory_order_relaxed); }
    size_t capacity() const { return _capacity; }
    void clear() { _write_index.store(0, std::memory_order_relaxed); }
private:
    size_t _capacity;
    size_t _mask;
    std::vector<Sample> _data;
    std::atomic<uint64_t> _write_index{0};
};
