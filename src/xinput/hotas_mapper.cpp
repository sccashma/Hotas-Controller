#include "hotas_mapper.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <iostream>
#include <sstream>
#include <Windows.h>
#include <ViGEm/Client.h>
#include <Xinput.h>

HotasMapper::HotasMapper() {}

HotasMapper::~HotasMapper() { stop(); }

// Enable verbose diagnostics for mapper (set true to print mapping and vigem events)
static bool g_verbose_mapper = true; // enable by default during debugging
// Simple file logger to persist diagnostics when running as GUI
static std::mutex g_log_mtx;
static void mapper_log(const std::string &s) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    // append to logfile in executable directory
    std::ofstream out("hotas_mapper.log", std::ios::out | std::ios::app);
    if (!out) return;
    out << s;
    if (s.empty() || s.back() != '\n') out << '\n';
}

// ViGEm client (single global target used by the mapper)
static PVIGEM_CLIENT g_vigem_client = nullptr;
static PVIGEM_TARGET g_vigem_target = nullptr;
static bool g_vigem_ready = false;

static void ensure_vigem_initialized() {
    if (g_vigem_ready) return;
    if (g_verbose_mapper) { std::cerr << "HotasMapper: initializing ViGEm client...\n"; mapper_log("HotasMapper: initializing ViGEm client..."); }
    g_vigem_client = vigem_alloc();
    if (!g_vigem_client) return;
    VIGEM_ERROR err = vigem_connect(g_vigem_client);
    if (!VIGEM_SUCCESS(err)) return;
    g_vigem_target = vigem_target_x360_alloc();
    if (!g_vigem_target) return;
    err = vigem_target_add(g_vigem_client, g_vigem_target);
    if (!VIGEM_SUCCESS(err)) return;
    g_vigem_ready = true;
    if (g_verbose_mapper) { std::cerr << "HotasMapper: ViGEm client initialized\n"; mapper_log("HotasMapper: ViGEm client initialized"); }
}

static void cleanup_vigem() {
    if (g_vigem_ready) {
        if (g_vigem_target && g_vigem_client) {
            vigem_target_remove(g_vigem_client, g_vigem_target);
            vigem_target_free(g_vigem_target);
        }
        if (g_vigem_client) vigem_free(g_vigem_client);
    }
    g_vigem_ready = false;
    g_vigem_client = nullptr; g_vigem_target = nullptr;
}

// Internal storage for mapping entries
// (kept in the cpp to avoid exposing internal containers in header)
static std::vector<MappingEntry> g_mappings;

// Optional injection callback (set by UI/main) so mapper can inject mapped states
static HotasMapper::InjectCallback g_inject_cb = nullptr;

void HotasMapper::set_inject_callback(InjectCallback cb) {
    g_inject_cb = cb;
}

void HotasMapper::start(double target_hz) {
    if (running.exchange(true)) return; // already running
    worker = new std::thread(&HotasMapper::publisher_thread_main, this, target_hz);
    if (g_verbose_mapper) { std::ostringstream ss; ss << "HotasMapper: started publisher thread at " << target_hz << " Hz"; std::cerr << ss.str() << "\n"; mapper_log(ss.str()); }
}

void HotasMapper::stop() {
    if (!running.exchange(false)) return;
    if (worker) {
        worker->join();
        delete worker; worker = nullptr;
    }
    // ensure cleanup of vigem resources when the mapper stops
    cleanup_vigem();
}

void HotasMapper::accept_sample(const std::string& signal_id, double value, double timestamp) {
    std::lock_guard<std::mutex> lk(mtx);
    pending_samples.emplace_back(signal_id, value, timestamp);
    if (g_verbose_mapper) {
        std::ostringstream ss; ss << "HotasMapper: accepted sample " << signal_id << "=" << value << " ts=" << timestamp; std::cerr << ss.str() << "\n"; mapper_log(ss.str());
    }
}

std::vector<HotasMappedOutput> HotasMapper::list_mappings() const {
    // placeholder: no mappings yet
    return {};
}

std::vector<MappingEntry> HotasMapper::list_mapping_entries() const {
    std::lock_guard<std::mutex> lk(mtx);
    return g_mappings;
}

bool HotasMapper::add_mapping(const MappingEntry& e) {
    std::lock_guard<std::mutex> lk(mtx);
    // Ensure unique id
    for (auto &m : g_mappings) if (m.id == e.id) return false;
    g_mappings.push_back(e);
    return true;
}

bool HotasMapper::remove_mapping(const std::string& mapping_id) {
    std::lock_guard<std::mutex> lk(mtx);
    for (size_t i = 0; i < g_mappings.size(); ++i) {
        if (g_mappings[i].id == mapping_id) { g_mappings.erase(g_mappings.begin() + i); return true; }
    }
    return false;
}

bool HotasMapper::save_profile(const std::string& path) const {
    nlohmann::json j;
    {
        std::lock_guard<std::mutex> lk(mtx);
        j["mappings"] = nlohmann::json::array();
        for (auto &m : g_mappings) {
            nlohmann::json jm;
            jm["id"] = m.id;
            jm["signal_id"] = m.signal_id;
            jm["action"] = m.action;
            jm["param"] = m.param;
            j["mappings"].push_back(jm);
        }
    }
    try {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        return true;
    } catch (...) { return false; }
}

bool HotasMapper::load_profile(const std::string& path) {
    try {
        std::ifstream in(path);
        if (!in) return false;
        nlohmann::json j; in >> j;
        if (!j.contains("mappings") || !j["mappings"].is_array()) return false;
        std::vector<MappingEntry> loaded;
        for (auto &jm : j["mappings"]) {
            MappingEntry me;
            if (jm.contains("id")) me.id = jm["id"].get<std::string>();
            if (jm.contains("signal_id")) me.signal_id = jm["signal_id"].get<std::string>();
            if (jm.contains("action")) me.action = jm["action"].get<std::string>();
            if (jm.contains("param")) me.param = jm["param"].get<double>();
            loaded.push_back(me);
        }
        std::lock_guard<std::mutex> lk(mtx);
        g_mappings = std::move(loaded);
        return true;
    } catch (...) { return false; }
}

void HotasMapper::publisher_thread_main(double hz) {
    using clock = std::chrono::high_resolution_clock;
    auto period = std::chrono::duration<double>(1.0 / hz);
    ensure_vigem_initialized();
    std::unordered_map<std::string,double> curvals;
    while (running) {
        auto t0 = clock::now();
        // simple publish: just print and clear pending samples
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!pending_samples.empty()) {
                for (auto &s : pending_samples) {
                    const auto &id = std::get<0>(s);
                    double v = std::get<1>(s);
                    curvals[id] = v; // update latest value for the logical signal
                }
                pending_samples.clear();
            }
        }
        // Build and send x360 report if any mappings target x360
        if (g_vigem_ready && !g_mappings.empty()) {
            XUSB_REPORT rep{};
            // helper conversions
            auto to_short = [](double v){ double vv = v; if (vv>1) vv=1; if (vv<-1) vv=-1; return (int16_t)(vv>=0? vv*32767.0 : vv*32768.0); };
            auto to_trig = [](double v){ double vv = v; if (vv<0) vv=0; if (vv>1) vv=1; return (uint8_t)(vv*255.0 + 0.5); };
            uint16_t button_mask = 0;
            for (auto &m : g_mappings) {
                if (m.action.rfind("x360:",0) != 0) continue;
                std::string act = m.action.substr(5);
                // axes
                if (act == "left_x") {
                    double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                    rep.sThumbLX = to_short(v);
                } else if (act == "left_y") {
                    double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                    rep.sThumbLY = to_short(-v);
                } else if (act == "right_x") {
                    double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                    rep.sThumbRX = to_short(v);
                } else if (act == "right_y") {
                    double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                    rep.sThumbRY = to_short(-v);
                } else if (act == "left_trigger") {
                    double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                    rep.bLeftTrigger = to_trig(v);
                } else if (act == "right_trigger") {
                    double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                    rep.bRightTrigger = to_trig(v);
                } else if (act == "button_a") { button_mask |= XINPUT_GAMEPAD_A; }
                else if (act == "button_b") { button_mask |= XINPUT_GAMEPAD_B; }
                else if (act == "button_x") { button_mask |= XINPUT_GAMEPAD_X; }
                else if (act == "button_y") { button_mask |= XINPUT_GAMEPAD_Y; }
                else if (act == "left_shoulder") { button_mask |= XINPUT_GAMEPAD_LEFT_SHOULDER; }
                else if (act == "right_shoulder") { button_mask |= XINPUT_GAMEPAD_RIGHT_SHOULDER; }
                else if (act == "back") { button_mask |= XINPUT_GAMEPAD_BACK; }
                else if (act == "start") { button_mask |= XINPUT_GAMEPAD_START; }
                else if (act == "left_thumb") { button_mask |= XINPUT_GAMEPAD_LEFT_THUMB; }
                else if (act == "right_thumb") { button_mask |= XINPUT_GAMEPAD_RIGHT_THUMB; }
                else if (act == "dpad_up") { button_mask |= XINPUT_GAMEPAD_DPAD_UP; }
                else if (act == "dpad_down") { button_mask |= XINPUT_GAMEPAD_DPAD_DOWN; }
                else if (act == "dpad_left") { button_mask |= XINPUT_GAMEPAD_DPAD_LEFT; }
                else if (act == "dpad_right") { button_mask |= XINPUT_GAMEPAD_DPAD_RIGHT; }
            }
            rep.wButtons = button_mask;
            // Before sending the report, optionally call the inject callback with a mapped ControllerState
            if (g_inject_cb) {
                XInputPoller::ControllerState cs{};
                auto to_float = [](int16_t s)->float { return (s >= 0) ? (double)s / 32767.0 : (double)s / 32768.0; };
                cs.lx = to_float(rep.sThumbLX);
                cs.ly = -to_float(rep.sThumbLY);
                cs.rx = to_float(rep.sThumbRX);
                cs.ry = -to_float(rep.sThumbRY);
                cs.lt = rep.bLeftTrigger / 255.0f;
                cs.rt = rep.bRightTrigger / 255.0f;
                cs.buttons = rep.wButtons;
                // timestamp unknown here; use steady_clock
                using clock = std::chrono::steady_clock;
                double t = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
                try { g_inject_cb(t, cs); } catch(...) {}
            }
            // send report
            if (g_verbose_mapper) {
                std::ostringstream ss;
                ss << "HotasMapper: sending X360 report: LX=" << rep.sThumbLX << " LY=" << rep.sThumbLY
                   << " RX=" << rep.sThumbRX << " RY=" << rep.sThumbRY << " LT=" << (int)rep.bLeftTrigger
                   << " RT=" << (int)rep.bRightTrigger << " buttons=0x" << std::hex << rep.wButtons << std::dec;
                std::cerr << ss.str() << "\n";
                mapper_log(ss.str());
            }
            VIGEM_ERROR err = vigem_target_x360_update(g_vigem_client, g_vigem_target, rep);
            if (!VIGEM_SUCCESS(err)) {
                std::ostringstream ss; ss << "HotasMapper: vigem update failed: " << err; std::cerr << ss.str() << "\n"; mapper_log(ss.str());
            }
        }
        auto t1 = clock::now();
        std::this_thread::sleep_for(period - (t1 - t0));
    }
}
