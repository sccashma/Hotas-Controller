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
#include <unordered_map>
#include <cmath>
#include <chrono>
#include <vector>
#include <unordered_map>

HotasMapper::HotasMapper() {}

HotasMapper::~HotasMapper() { stop(); }

// Enable verbose diagnostics for mapper (set true to print mapping and vigem events)
static bool g_verbose_mapper = false; // disable verbose mapper logging

// ViGEm client (single global target used by the mapper)
static PVIGEM_CLIENT g_vigem_client = nullptr;
static PVIGEM_TARGET g_vigem_target = nullptr;
static bool g_vigem_ready = false;

static void ensure_vigem_initialized() {
    if (g_vigem_ready) return;
    if (g_verbose_mapper) { std::cerr << "HotasMapper: initializing ViGEm client...\n"; }
    g_vigem_client = vigem_alloc();
    if (!g_vigem_client) return;
    VIGEM_ERROR err = vigem_connect(g_vigem_client);
    if (!VIGEM_SUCCESS(err)) return;
    g_vigem_target = vigem_target_x360_alloc();
    if (!g_vigem_target) return;
    err = vigem_target_add(g_vigem_client, g_vigem_target);
    if (!VIGEM_SUCCESS(err)) return;
    g_vigem_ready = true;
    if (g_verbose_mapper) { std::cerr << "HotasMapper: ViGEm client initialized\n"; }
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

// Keyboard auto-repeat parameters (from system) and per-key repeat state
struct KbdRepeatParams { int delay_ms = 250; int interval_ms = 33; bool inited = false; };
static KbdRepeatParams g_kbd_params;

static void init_kbd_params_once() {
    if (g_kbd_params.inited) return;
    UINT delay = 1, speed = 31; // sensible defaults
    SystemParametersInfoA(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    SystemParametersInfoA(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    int delay_ms = static_cast<int>((delay + 1) * 250); // 0..3 => 250..1000ms
    // Map 0..31 to approx 2.5..30 cps
    double cps = 2.5 + (27.5 * (static_cast<double>(speed) / 31.0));
    int interval_ms = cps > 0.1 ? static_cast<int>(1000.0 / cps) : 40;
    g_kbd_params.delay_ms = delay_ms;
    g_kbd_params.interval_ms = (interval_ms < 10) ? 10 : interval_ms;
    g_kbd_params.inited = true;
}

struct KeyRepeatState {
    bool pressed = false;
    std::string name;
    std::chrono::steady_clock::time_point press_time;
    std::chrono::steady_clock::time_point next_repeat;
};
static std::unordered_map<UINT, KeyRepeatState> g_key_repeat;

static UINT parse_vk(const std::string& name) {
    std::string s = name; for (auto &c : s) c = (char)toupper((unsigned char)c);
    if (s.rfind("VK_",0) == 0) {
        s = s.substr(3);
    }
    // Common VK names
    if (s == "SPACE") return VK_SPACE;
    if (s == "SHIFT") return VK_SHIFT;
    if (s == "LSHIFT") return VK_LSHIFT;
    if (s == "RSHIFT") return VK_RSHIFT;
    if (s == "CONTROL") return VK_CONTROL;
    if (s == "LCONTROL") return VK_LCONTROL;
    if (s == "RCONTROL") return VK_RCONTROL;
    if (s == "ALT" || s == "MENU") return VK_MENU;
    if (s == "LALT" || s == "LMENU") return VK_LMENU;
    if (s == "RALT" || s == "RMENU") return VK_RMENU;
    if (s == "RETURN" || s == "ENTER") return VK_RETURN;
    if (s == "TAB") return VK_TAB;
    if (s == "ESC" || s == "ESCAPE") return VK_ESCAPE;
    if (s == "UP") return VK_UP;
    if (s == "DOWN") return VK_DOWN;
    if (s == "LEFT") return VK_LEFT;
    if (s == "RIGHT") return VK_RIGHT;
    if (s == "BACK" || s == "BACKSPACE") return VK_BACK;
    if (s == "DELETE" || s == "DEL") return VK_DELETE;
    if (s == "HOME") return VK_HOME;
    if (s == "END") return VK_END;
    if (s == "PAGEUP") return VK_PRIOR;
    if (s == "PAGEDOWN") return VK_NEXT;
    if (s == "CAPS") return VK_CAPITAL;
    if (s == "NUMLOCK") return VK_NUMLOCK;
    if (s == "SCROLL") return VK_SCROLL;
    // Single ASCII letter/digit
    if (s.size() == 1) {
        char c = s[0];
        if (c >= 'A' && c <= 'Z') return (UINT)c;
        if (c >= '0' && c <= '9') return (UINT)c;
    }
    // F-keys
    if (s.size() >= 2 && s[0]=='F') {
        int fn = 0; try { fn = std::stoi(s.substr(1)); } catch(...) { fn = 0; }
        if (fn >= 1 && fn <= 24) return (UINT)(VK_F1 + (fn - 1));
    }
    return 0; // unknown
}

static bool is_extended_vk(UINT vk) {
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL:
        case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: // PageUp/PageDown
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_DIVIDE: case VK_NUMLOCK:
            return true;
        default: return false;
    }
}

static void send_key(UINT vk, bool down) {
    if (vk == 0) return;
    HKL layout = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC, layout);
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0; // use scan code to ensure proper 'code' and game handling
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    if (is_extended_vk(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(INPUT));
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
    if (g_verbose_mapper) { std::ostringstream ss; ss << "HotasMapper: started publisher thread at " << target_hz << " Hz"; std::cerr << ss.str() << "\n"; }
}

void HotasMapper::stop() {
    if (!running.exchange(false)) return;
    if (worker) {
        worker->join();
        delete worker; worker = nullptr;
    }
    // Release any pressed keys on stop
    for (auto &kv : g_key_repeat) {
        if (kv.second.pressed) {
            send_key(kv.first, false);
        }
    }
    g_key_repeat.clear();
    // ensure cleanup of vigem resources when the mapper stops
    cleanup_vigem();
}

void HotasMapper::accept_sample(const std::string& signal_id, double value, double timestamp) {
    std::lock_guard<std::mutex> lk(mtx);
    pending_samples.emplace_back(signal_id, value, timestamp);
    if (g_verbose_mapper) {
        std::ostringstream ss; ss << "HotasMapper: accepted sample " << signal_id << "=" << value << " ts=" << timestamp; std::cerr << ss.str() << "\n";
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
    // Overwrite if id exists; else append
    for (auto &m : g_mappings) {
        if (m.id == e.id) { m = e; return true; }
    }
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
            jm["priority"] = m.priority;
            jm["deadband"] = m.deadband;
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
            if (jm.contains("priority")) me.priority = jm["priority"].get<int>();
            // For analog actions, deadband must be present; if missing, apply a sensible default
            if (jm.contains("deadband")) me.deadband = jm["deadband"].get<double>();
            else {
                // default only for analog actions
                std::string act = me.action;
                if (act.rfind("x360:",0) == 0) {
                    std::string a = act.substr(5);
                    if (a == "left_x" || a == "left_y" || a == "right_x" || a == "right_y" || a == "left_trigger" || a == "right_trigger") {
                        me.deadband = 0.05; // default deadband
                    }
                }
            }
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
        if (!g_mappings.empty()) {
            XUSB_REPORT rep{};
            auto to_short = [](double v){ double vv = v; if (vv>1) vv=1; if (vv<-1) vv=-1; return (int16_t)(vv>=0? vv*32767.0 : vv*32768.0); };
            auto to_trig = [](double v){ double vv = v; if (vv<0) vv=0; if (vv>1) vv=1; return (uint8_t)(vv*255.0 + 0.5); };
            // Group mappings by action
            std::unordered_map<std::string, std::vector<MappingEntry>> groups;
            {
                std::lock_guard<std::mutex> lk(mtx);
                for (const auto &m : g_mappings) {
                    if (m.action.rfind("x360:",0) != 0) continue;
                    groups[m.action].push_back(m);
                }
            }
            auto read_val = [&](const std::string &sid)->double {
                auto it = curvals.find(sid);
                return (it != curvals.end()) ? it->second : 0.0;
            };
            auto resolve_axis = [&](const std::vector<MappingEntry>& vec)->double {
                if (vec.empty()) return 0.0;
                // sort by priority desc
                std::vector<MappingEntry> tmp = vec;
                std::sort(tmp.begin(), tmp.end(), [](const MappingEntry& a, const MappingEntry& b){ return a.priority > b.priority; });
                double fallback_max = 0.0; double fallback_val = 0.0;
                for (const auto &m : tmp) {
                    double v = read_val(m.signal_id);
                    double mag = std::fabs(v);
                    if (mag > m.deadband) {
                        return v; // first above deadband wins by priority
                    }
                    if (mag > fallback_max) { fallback_max = mag; fallback_val = v; }
                }
                return fallback_val; // none above deadband: use largest magnitude
            };
            auto resolve_button = [&](const std::vector<MappingEntry>& vec)->bool {
                if (vec.empty()) return false;
                std::vector<MappingEntry> tmp = vec;
                std::sort(tmp.begin(), tmp.end(), [](const MappingEntry& a, const MappingEntry& b){ return a.priority > b.priority; });
                for (const auto &m : tmp) {
                    double v = read_val(m.signal_id);
                    if (v > 0.5) return true; // first active wins
                }
                return false;
            };

            // Axes
            if (groups.count("x360:left_x")) rep.sThumbLX = to_short(resolve_axis(groups["x360:left_x"]));
            if (groups.count("x360:left_y")) rep.sThumbLY = to_short(-resolve_axis(groups["x360:left_y"]));
            if (groups.count("x360:right_x")) rep.sThumbRX = to_short(resolve_axis(groups["x360:right_x"]));
            if (groups.count("x360:right_y")) rep.sThumbRY = to_short(-resolve_axis(groups["x360:right_y"]));
            if (groups.count("x360:left_trigger")) rep.bLeftTrigger = to_trig(resolve_axis(groups["x360:left_trigger"]));
            if (groups.count("x360:right_trigger")) rep.bRightTrigger = to_trig(resolve_axis(groups["x360:right_trigger"]));

            // Buttons/DPad
            uint16_t button_mask = 0;
            if (groups.count("x360:button_a") && resolve_button(groups["x360:button_a"])) button_mask |= XINPUT_GAMEPAD_A;
            if (groups.count("x360:button_b") && resolve_button(groups["x360:button_b"])) button_mask |= XINPUT_GAMEPAD_B;
            if (groups.count("x360:button_x") && resolve_button(groups["x360:button_x"])) button_mask |= XINPUT_GAMEPAD_X;
            if (groups.count("x360:button_y") && resolve_button(groups["x360:button_y"])) button_mask |= XINPUT_GAMEPAD_Y;
            if (groups.count("x360:left_shoulder") && resolve_button(groups["x360:left_shoulder"])) button_mask |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (groups.count("x360:right_shoulder") && resolve_button(groups["x360:right_shoulder"])) button_mask |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (groups.count("x360:back") && resolve_button(groups["x360:back"])) button_mask |= XINPUT_GAMEPAD_BACK;
            if (groups.count("x360:start") && resolve_button(groups["x360:start"])) button_mask |= XINPUT_GAMEPAD_START;
            if (groups.count("x360:left_thumb") && resolve_button(groups["x360:left_thumb"])) button_mask |= XINPUT_GAMEPAD_LEFT_THUMB;
            if (groups.count("x360:right_thumb") && resolve_button(groups["x360:right_thumb"])) button_mask |= XINPUT_GAMEPAD_RIGHT_THUMB;
            if (groups.count("x360:dpad_up") && resolve_button(groups["x360:dpad_up"])) button_mask |= XINPUT_GAMEPAD_DPAD_UP;
            if (groups.count("x360:dpad_down") && resolve_button(groups["x360:dpad_down"])) button_mask |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (groups.count("x360:dpad_left") && resolve_button(groups["x360:dpad_left"])) button_mask |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (groups.count("x360:dpad_right") && resolve_button(groups["x360:dpad_right"])) button_mask |= XINPUT_GAMEPAD_DPAD_RIGHT;
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
            // send report (only if ViGEm ready)
            if (g_vigem_ready) {
                if (g_verbose_mapper) {
                    std::ostringstream ss;
                    ss << "HotasMapper: sending X360 report: LX=" << rep.sThumbLX << " LY=" << rep.sThumbLY
                       << " RX=" << rep.sThumbRX << " RY=" << rep.sThumbRY << " LT=" << (int)rep.bLeftTrigger
                       << " RT=" << (int)rep.bRightTrigger << " buttons=0x" << std::hex << rep.wButtons << std::dec;
                    std::cerr << ss.str() << "\n";
                }
                VIGEM_ERROR err = vigem_target_x360_update(g_vigem_client, g_vigem_target, rep);
                if (!VIGEM_SUCCESS(err)) {
                    std::ostringstream ss; ss << "HotasMapper: vigem update failed: " << err; std::cerr << ss.str() << "\n";
                }
            }
        }
        // Handle keyboard mappings with aggregation + auto-repeat while held
        if (!g_mappings.empty()) {
            init_kbd_params_once();
            std::unordered_map<UINT, bool> desired_active; // vk -> active
            std::unordered_map<UINT, std::string> vk_names;
            for (auto &m : g_mappings) {
                if (m.action.rfind("keyboard:",0) != 0) continue;
                std::string keyStr = m.action.substr(9);
                UINT vk = parse_vk(keyStr);
                if (vk == 0) continue;
                double v = curvals.count(m.signal_id) ? curvals[m.signal_id] : 0.0;
                bool active = std::fabs(v) > 0.01; // axes use -1..1; buttons 0/1
                auto it = desired_active.find(vk);
                if (it == desired_active.end()) desired_active[vk] = active;
                else it->second = it->second || active;
                vk_names[vk] = keyStr;
            }

            const auto now = std::chrono::steady_clock::now();
            // Press, repeat, or release as needed
            for (auto &kv : desired_active) {
                UINT vk = kv.first; bool want = kv.second;
                auto &st = g_key_repeat[vk];
                if (want && !st.pressed) {
                    send_key(vk, true);
                    st.pressed = true;
                    st.name = vk_names[vk];
                    st.press_time = now;
                    st.next_repeat = now + std::chrono::milliseconds(g_kbd_params.delay_ms);
                        if (g_verbose_mapper) {
                            std::ostringstream ss; ss << "HotasMapper: keydown " << st.name;
                            std::cerr << ss.str() << "\n";
                        }
                } else if (want && st.pressed) {
                    if (now >= st.next_repeat) {
                        send_key(vk, true); // generate auto-repeat keydown
                        st.next_repeat = now + std::chrono::milliseconds(g_kbd_params.interval_ms);
                        if (g_verbose_mapper) {
                            std::ostringstream ss; ss << "HotasMapper: keyrepeat " << (st.name.empty()?std::to_string(vk):st.name);
                            std::cerr << ss.str() << "\n";
                        }
                    }
                } else if (!want && st.pressed) {
                    send_key(vk, false);
                    st.pressed = false;
                        if (g_verbose_mapper) {
                            std::ostringstream ss; ss << "HotasMapper: keyup " << (st.name.empty()?std::to_string(vk):st.name);
                            std::cerr << ss.str() << "\n";
                        }
                }
            }
            // Release any keys no longer desired (not in desired_active)
            std::vector<UINT> to_release;
            for (auto &kv : g_key_repeat) {
                UINT vk = kv.first; auto &st = kv.second;
                bool want = desired_active.count(vk) ? desired_active[vk] : false;
                if (st.pressed && !want) to_release.push_back(vk);
            }
            for (UINT vk : to_release) {
                auto it = g_key_repeat.find(vk);
                if (it != g_key_repeat.end() && it->second.pressed) {
                    send_key(vk, false);
                    if (g_verbose_mapper) {
                        std::ostringstream ss; ss << "HotasMapper: keyup " << (it->second.name.empty()?std::to_string(vk):it->second.name);
                        std::cerr << ss.str() << "\n";
                    }
                    it->second.pressed = false;
                }
            }
        }
        auto t1 = clock::now();
        std::this_thread::sleep_for(period - (t1 - t0));
    }
}
