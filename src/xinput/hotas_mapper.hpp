#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <tuple>
#include "xinput_poll.hpp"

// Minimal HotasMapper scaffolding: translates logical HOTAS signals into
// output actions (XInput/keyboard/mouse). This is a starting point and will
// be extended with mapping entries and profile persistence.

struct HotasMappedOutput {
    // placeholder: expand to include action type and params
    std::string desc;
};

// A mapping entry connects a logical HOTAS signal id to an action.
// For now actions are represented generically so the UI can list/edit them.
struct MappingEntry {
    std::string id;         // unique mapping id (e.g. "m1")
    std::string signal_id;  // e.g. "joy_x", "A", etc.
    std::string action;     // action descriptor (e.g. "x360:left_x")
    double param = 0.0;     // optional parameter (deadzone, scale, etc.)
};

class HotasMapper {
public:
    HotasMapper();
    ~HotasMapper();

    // Start/stop the mapper's publisher thread (publishes at target_hz)
    void start(double target_hz = 1000.0);
    void stop();

    // Called by the poller when new logical samples are available
    // (signal_id, value, timestamp)
    void accept_sample(const std::string& signal_id, double value, double timestamp);

    // For UI: list current mapped outputs (brief description)
    std::vector<HotasMappedOutput> list_mappings() const;

    // Mapping management
    std::vector<MappingEntry> list_mapping_entries() const;
    bool add_mapping(const MappingEntry& e);
    bool remove_mapping(const std::string& mapping_id);

    // Persist/load mapping profile (JSON)
    bool save_profile(const std::string& path) const;
    bool load_profile(const std::string& path);
    // Optional: register a callback so mapper can inject mapped controller states
    using InjectCallback = std::function<void(double,const XInputPoller::ControllerState&)>;
    void set_inject_callback(InjectCallback cb);

private:
    void publisher_thread_main(double hz);

    std::atomic<bool> running{false};
    std::thread* worker = nullptr;
    // simple sample store (thread-safe minimal); improve later
    mutable std::mutex mtx;
    std::vector<std::tuple<std::string,double,double>> pending_samples; // id,val,ts
};
