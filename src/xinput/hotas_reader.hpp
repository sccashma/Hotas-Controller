
#pragma once
#include "xinput_poll.hpp"
#include <optional>
#include "core/ring_buffer.hpp"
#include <atomic>
#include <vector>
#include <string>

// HOTAS reader that attempts to find and read the two X56 devices
// reported by Device Manager: "X56 H.O.T.A.S. Stick (Bulk)" and
// "X56 H.O.T.A.S. Throttle (Bulk)". It combines their states into a
// single XInputPoller::ControllerState snapshot for injection.

struct HotasSnapshot {
    bool ok = false;
    XInputPoller::ControllerState state{};
};

class HotasReader {
public:
    HotasReader();
    ~HotasReader();

    // Poll both devices and return a combined snapshot. ok==true when at least
    // one device provided usable data (prefer both combined).
    HotasSnapshot poll_once();
    bool has_stick() const;
    bool has_throttle() const;
    // Snapshot JOY axes into out vectors (timestamps are steady-clock seconds)
    double latest_time() const;
    void snapshot_joys(std::vector<Sample>& out_x, std::vector<Sample>& out_y, double window_seconds) const;

    // Enumerate DirectInput game controller devices and return human-readable lines
    static std::vector<std::string> enumerate_devices();
    // Return debug lines collected during device enumeration (for UI display)
    static std::vector<std::string> debug_lines();

    // Temporary: start/stop a HID live monitor (non-persistent, for mapping VID/PID)
    void start_hid_live();
    void stop_hid_live();
    // Returns pairs of device path and last raw report hex string
    std::vector<std::pair<std::string,std::string>> get_hid_live_snapshot() const;

    // Signal descriptor for a logical HOTAS input
    struct SignalDescriptor {
        std::string id; // unique id
        std::string name; // human label
        int bit_start;
        int bits;
        bool analog;
    };
    // List signals known by the reader (useful for mapping UI)
    std::vector<SignalDescriptor> list_signals() const;

private:
    // Internal state for HotasReader; keep name explicit and non-abbreviated
    struct HotasReaderInternalState;
    HotasReaderInternalState* internal_state = nullptr;
};

