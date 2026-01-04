#include "hotas_reader.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <setupapi.h>
#include <hidsdi.h>
#include <initguid.h>
#include <guiddef.h>
#include <hidclass.h>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <Xinput.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#include <sstream>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>
#include <algorithm>
#include <fstream>

struct HotasReader::HotasReaderInternalState {
    // Devices will be opened via HID/raw APIs later; keep storage for axes
    SampleRing joy_x{1u<<18};
    SampleRing joy_y{1u<<18};
    std::atomic<double> latest{0.0};
    std::vector<SignalDescriptor> signals; // loaded from CSV on startup

    // HID device handles (invalid if not opened)
    HANDLE stick_handle = INVALID_HANDLE_VALUE;
    HANDLE throttle_handle = INVALID_HANDLE_VALUE;

    // Temporary live monitor
    std::atomic<bool> live_running{false};
    std::vector<std::thread> live_threads;
    std::vector<HANDLE> live_handles;
    mutable std::mutex live_mutex;
    struct LiveEntry { std::string hex; double ts; };
    std::map<std::string, LiveEntry> live_last; // devicePath -> {hex, timestamp}
};

static std::vector<std::string> s_debug_lines;

// debug_lines returns any collected debug strings (may be empty)
std::vector<std::string> HotasReader::debug_lines() {
    return s_debug_lines;
}

static std::string wcs_to_utf8(const wchar_t* w) {
    if (!w) return {};
    int len = (int)wcslen(w);
    if (len == 0) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, NULL, NULL);
    if (sz <= 0) return {};
    std::string out(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), sz, NULL, NULL);
    return out;
}

HotasReader::HotasReader() {
    internal_state = new HotasReaderInternalState();

    // Attempt to enumerate HID devices and open Saitek stick/throttle handles.
    auto lines = HotasReader::enumerate_devices();
    // enumerate_devices also populates s_debug_lines; we now try to open handles
    // by re-scanning and opening the matching device paths.

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        s_debug_lines.push_back("SetupDiGetClassDevsW failed");
        return;
    }

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, idx, &ifData); ++idx) {
        // Get required buffer size
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &required, NULL);
        std::vector<uint8_t> buf(required);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfoData;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required, NULL, &devInfoData)) {
            std::ostringstream oss; oss << "SetupDiGetDeviceInterfaceDetailW failed at index " << idx;
            s_debug_lines.push_back(oss.str());
            continue;
        }
        const wchar_t* devicePath = detail->DevicePath;
        // Open handle to query product string
        HANDLE h = CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            std::ostringstream oss; oss << "CreateFileW failed for path: " << wcs_to_utf8(devicePath);
            s_debug_lines.push_back(oss.str());
            continue;
        }
        // Only open handles for the specific device paths requested (connection-only)
        std::string p = wcs_to_utf8(devicePath);
        if (p.find("vid_0738&pid_2221") != std::string::npos && p.find("mi_00") != std::string::npos) {
            if (internal_state->stick_handle == INVALID_HANDLE_VALUE) {
                internal_state->stick_handle = h;
                s_debug_lines.push_back(std::string("Opened stick HID handle: ") + p);
                continue; // keep handle
            }
        }
        if (p.find("vid_0738&pid_a221") != std::string::npos && p.find("mi_00") != std::string::npos) {
            if (internal_state->throttle_handle == INVALID_HANDLE_VALUE) {
                internal_state->throttle_handle = h;
                s_debug_lines.push_back(std::string("Opened throttle HID handle: ") + p);
                continue; // keep handle
            }
        }
        // Not one of the requested devices; close handle.
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    // Load signal descriptors from CSV (single source of truth)
    auto load_csv_signals = [&](const wchar_t* path)->bool {
        std::string sp = wcs_to_utf8(path);
        std::ifstream in(sp.c_str());
        if (!in) return false;
        std::string line;
        // Skip header
        if (!std::getline(in, line)) return false;
        std::vector<SignalDescriptor> sigs;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            // Basic CSV split (no quoted field parsing beyond notes at end)
            std::vector<std::string> cols; cols.reserve(8);
            size_t pos = 0;
            while (pos < line.size()) {
                size_t comma = line.find(',', pos);
                if (comma == std::string::npos) comma = line.size();
                cols.emplace_back(line.substr(pos, comma - pos));
                pos = (comma == line.size()) ? comma : (comma + 1);
            }
            if (cols.size() < 7) continue;
            std::string device = cols[0];
            std::string inputType = cols[3];
            std::string inputId = cols[4];
            std::string bitRange = cols[5];
            std::string bitsStr = cols[6];
            // Parse bit_start from range "A-B" or single number
            int bit_start = 0; int bits = 0;
            try {
                size_t dash = bitRange.find('-');
                if (dash == std::string::npos) bit_start = std::stoi(bitRange);
                else bit_start = std::stoi(bitRange.substr(0, dash));
                bits = std::stoi(bitsStr);
            } catch (...) { continue; }
            bool analog = false;
            {
                std::string t = inputType; 
                for (auto &c : t) c = (char)tolower((unsigned char)c);
                analog = (t.find("analog") != std::string::npos);
            }
            // Build display name: uppercase with '-' -> '_' and id-specific casing
            auto to_upper_underscore = [](std::string s){
                for (auto &c : s) {
                    if (c == '-') c = '_';
                    else c = (char)toupper((unsigned char)c);
                }
                return s;
            };
            std::string name = to_upper_underscore(inputId);
            // Normalize known variants
            if (name == "G-WHEEL") name = "G_WHEEL";
            HotasReader::SignalDescriptor::DeviceKind dk = HotasReader::SignalDescriptor::DeviceKind::Stick;
            {
                std::string d = device; for (auto &c : d) c = (char)tolower((unsigned char)c);
                if (d.find("throttle") != std::string::npos) dk = HotasReader::SignalDescriptor::DeviceKind::Throttle;
                else dk = HotasReader::SignalDescriptor::DeviceKind::Stick;
            }
            SignalDescriptor sd{ inputId, name, bit_start, bits, analog, dk };
            sigs.push_back(sd);
        }
        internal_state->signals = std::move(sigs);
        return !internal_state->signals.empty();
    };
    // Try common relative locations from build directory
    if (!load_csv_signals(L"config\\X56_Hotas_hid_bit_map.csv")) {
        if (!load_csv_signals(L"..\\X56_Hotas_hid_bit_map.csv")) {
            load_csv_signals(L"..\\..\\X56_Hotas_hid_bit_map.csv");
        }
    }
    // Fallback to built-in minimal set if CSV missing
    if (internal_state->signals.empty()) {
        internal_state->signals = std::vector<SignalDescriptor>{
            {"joy_x","JOY_X",8,16,true, SignalDescriptor::DeviceKind::Stick},
            {"joy_y","JOY_Y",24,16,true, SignalDescriptor::DeviceKind::Stick},
            {"joy_z","JOY_Z",40,12,true, SignalDescriptor::DeviceKind::Stick},
            {"c_joy_x","C_JOY_X",80,8,true, SignalDescriptor::DeviceKind::Stick},
            {"c_joy_y","C_JOY_Y",88,8,true, SignalDescriptor::DeviceKind::Stick},
            {"C","C",59,1,false, SignalDescriptor::DeviceKind::Stick},
            {"trigger","TRIGGER",56,1,false, SignalDescriptor::DeviceKind::Stick},
            {"A","BTN_A",57,1,false, SignalDescriptor::DeviceKind::Stick},
            {"B","BTN_B",58,1,false, SignalDescriptor::DeviceKind::Stick},
            {"D","BTN_D",60,1,false, SignalDescriptor::DeviceKind::Stick},
            {"E","BTN_E",61,1,false, SignalDescriptor::DeviceKind::Stick},
            {"POV","POV",52,4,false, SignalDescriptor::DeviceKind::Stick},
            {"H1","H1",62,4,false, SignalDescriptor::DeviceKind::Stick},
            {"H2","H2",66,4,false, SignalDescriptor::DeviceKind::Stick},
            {"left_throttle","LEFT_THROTTLE",8,10,true, SignalDescriptor::DeviceKind::Throttle},
            {"right_throttle","RIGHT_THROTTLE",18,10,true, SignalDescriptor::DeviceKind::Throttle},
            {"F_wheel","F_WHEEL",64,8,true, SignalDescriptor::DeviceKind::Throttle},
            {"G_wheel","G_WHEEL",80,8,true, SignalDescriptor::DeviceKind::Throttle},
            {"RTY3","RTY3",104,8,true, SignalDescriptor::DeviceKind::Throttle},
            {"RTY4","RTY4",96,8,true, SignalDescriptor::DeviceKind::Throttle},
            {"thumb_joy_x","THUMB_JOY_X",72,8,true, SignalDescriptor::DeviceKind::Throttle},
            {"thumb_joy_y","THUMB_JOY_Y",88,8,true, SignalDescriptor::DeviceKind::Throttle},
            {"pinky_encoder","PINKY_ENCODER",57,2,false, SignalDescriptor::DeviceKind::Throttle},
            {"thumb_joy_press","THUMB_JOY_PRESS",59,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"E_th","E",28,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"F_th","F",29,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"G_th","G",30,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"H_th","H",32,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"I_th","I",31,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"K1_up","K1_UP",55,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"K1_down","K1_DOWN",56,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"slide","SLIDE",60,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"SW1","SW1",33,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"SW2","SW2",34,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"SW3","SW3",35,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"SW4","SW4",36,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"SW5","SW5",37,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"SW6","SW6",38,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL1_up","TGL1_UP",39,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL1_down","TGL1_DOWN",40,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL2_up","TGL2_UP",41,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL2_down","TGL2_DOWN",42,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL3_up","TGL3_UP",43,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL3_down","TGL3_DOWN",44,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL4_up","TGL4_UP",45,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"TGL4_down","TGL4_DOWN",46,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"M1","M1",61,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"M2","M2",62,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"S1","S1",63,1,false, SignalDescriptor::DeviceKind::Throttle},
            {"H3","H3",47,4,false, SignalDescriptor::DeviceKind::Throttle},
            {"H4","H4",51,4,false, SignalDescriptor::DeviceKind::Throttle}
        };
    }
}

static std::string to_hex(const uint8_t* d, size_t n) {
    std::ostringstream oss;
    oss << std::hex;
    for (size_t i=0;i<n;++i) {
        oss.width(2); oss.fill('0'); oss << (int)d[i];
    }
    return oss.str();
}

void HotasReader::start_hid_live() {
    if (!internal_state) return;
    if (internal_state->live_running.exchange(true)) return; // already running
    // enumerate device interfaces and collect matching device paths first
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> paths;
    SP_DEVICE_INTERFACE_DATA ifData; ifData.cbSize = sizeof(ifData);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, idx, &ifData); ++idx) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &required, NULL);
        std::vector<uint8_t> buf(required);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfoData; devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required, NULL, &devInfoData)) continue;
        std::wstring wp(detail->DevicePath);
        std::string path = wcs_to_utf8(wp.c_str());
        // Only register paths matching the exact stick/throttle identifiers requested by user
        if ((path.find("vid_0738&pid_2221") != std::string::npos && path.find("mi_00") != std::string::npos) ||
            (path.find("vid_0738&pid_a221") != std::string::npos && path.find("mi_00") != std::string::npos)) {
            paths.push_back(wp);
            std::lock_guard<std::mutex> g(internal_state->live_mutex);
            internal_state->live_last[path] = HotasReader::HotasReaderInternalState::LiveEntry{ std::string("(no data yet)"), 0.0 };
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    // For each matching path open handle and spawn a thread to read it
    for (auto &wp : paths) {
        HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string path = wcs_to_utf8(wp.c_str());
        {
            std::lock_guard<std::mutex> g(internal_state->live_mutex);
            internal_state->live_handles.push_back(h);
            // Register the path so UI shows it even before any reports arrive
            internal_state->live_last[path] = HotasReader::HotasReaderInternalState::LiveEntry{ std::string("(no data yet)"), 0.0 };
        }
        internal_state->live_threads.emplace_back([this, h, path]() {
            const size_t buf_sz = 64;
            std::vector<uint8_t> rbuf(buf_sz);
            OVERLAPPED ov{}; ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            while (internal_state->live_running.load()) {
                ResetEvent(ov.hEvent);
                DWORD read = 0;
                BOOL ok = ReadFile(h, rbuf.data(), (DWORD)buf_sz, &read, &ov);
                if (!ok) {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        DWORD w = WaitForSingleObject(ov.hEvent, 200);
                        if (w == WAIT_OBJECT_0) {
                            GetOverlappedResult(h, &ov, &read, FALSE);
                        } else {
                            continue;
                        }
                    } else {
                        break; // error
                    }
                }
                if (read > 0) {
                    std::string hex = to_hex(rbuf.data(), read);
                    double ts = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    std::lock_guard<std::mutex> g(internal_state->live_mutex);
                    internal_state->live_last[path] = HotasReader::HotasReaderInternalState::LiveEntry{ hex, ts };
                } else {
                    // mark as no data yet
                    std::lock_guard<std::mutex> g(internal_state->live_mutex);
                    auto it = internal_state->live_last.find(path);
                    if (it != internal_state->live_last.end()) it->second.hex = std::string("(no data yet)");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CloseHandle(ov.hEvent);
            // Do not close h here; stop_hid_live will close all handles.
        });
    }
}

void HotasReader::stop_hid_live() {
    if (!internal_state) return;
    if (!internal_state->live_running.exchange(false)) return;
    // join threads
    for (auto &t : internal_state->live_threads) if (t.joinable()) t.join();
    internal_state->live_threads.clear();
    // close handles
    {
        std::lock_guard<std::mutex> g(internal_state->live_mutex);
        for (auto h : internal_state->live_handles) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        internal_state->live_handles.clear();
        internal_state->live_last.clear();
    }
}

std::vector<std::pair<std::string,std::string>> HotasReader::get_hid_live_snapshot() const {
    std::vector<std::pair<std::string,std::string>> out;
    if (!internal_state) return out;
    std::lock_guard<std::mutex> g(internal_state->live_mutex);
    for (auto &p : internal_state->live_last) out.emplace_back(p.first, p.second.hex);
    return out;
}

std::vector<std::string> HotasReader::enumerate_devices() {
    std::vector<std::string> lines;
    s_debug_lines.clear();

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        s_debug_lines.push_back("SetupDiGetClassDevsW failed");
        return {};
    }

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, idx, &ifData); ++idx) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &required, NULL);
        std::vector<uint8_t> buf(required);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfoData;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required, NULL, &devInfoData)) {
            std::ostringstream oss; oss << "SetupDiGetDeviceInterfaceDetailW failed at index " << idx;
            s_debug_lines.push_back(oss.str());
            continue;
        }
        const wchar_t* devicePath = detail->DevicePath;

        HANDLE h = CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            std::ostringstream oss; oss << "CreateFileW failed for path: " << wcs_to_utf8(devicePath);
            s_debug_lines.push_back(oss.str());
            continue;
        }
        // Do not query product strings here; only report the device path.
        lines.push_back(std::string("DevicePath: ") + wcs_to_utf8(devicePath));
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return lines;
}

std::vector<HotasReader::SignalDescriptor> HotasReader::list_signals() const {
    if (!internal_state) return {};
    return internal_state->signals;
}

HotasReader::~HotasReader() {
    if (internal_state) {
        if (internal_state->stick_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(internal_state->stick_handle);
            internal_state->stick_handle = INVALID_HANDLE_VALUE;
        }
        if (internal_state->throttle_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(internal_state->throttle_handle);
            internal_state->throttle_handle = INVALID_HANDLE_VALUE;
        }
        delete internal_state;
        internal_state = nullptr;
    }
}

// Helper: convert hex string to bytes
static bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out) {
    out.clear();
    if (hex.size() < 2) return false;
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hexv(hex[i]);
        int lo = hexv(hex[i+1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return !out.empty();
}

 

// Poll devices and synthesize a ControllerState from latest HID reports.
// Uses live_last_hex captured by start_hid_live(); runs independent of UI focus.
HotasSnapshot HotasReader::poll_once() {
    HotasSnapshot snap;
    if (!internal_state) return snap;

    // Grab latest HID hex for known device paths (stick/throttle), requiring freshness
    std::string stick_hex;
    std::string throttle_hex;
    double now_sec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // Advance UI timebase on every poll to avoid apparent freezes when inputs idle
    internal_state->latest.store(now_sec, std::memory_order_release);
    const double fresh_thresh = 0.5; // seconds
    {
        std::lock_guard<std::mutex> g(internal_state->live_mutex);
        for (const auto& kv : internal_state->live_last) {
            const std::string& path = kv.first;
            const auto& entry  = kv.second;
            const std::string& hex  = entry.hex;
            if (hex.empty() || hex == "(no data yet)") continue;
            // Require recent updates to avoid using stale data after disconnect
            if (entry.ts <= 0.0 || (now_sec - entry.ts) > fresh_thresh) continue;
            if (path.find("vid_0738&pid_2221") != std::string::npos && path.find("mi_00") != std::string::npos) {
                stick_hex = hex;
            } else if (path.find("vid_0738&pid_a221") != std::string::npos && path.find("mi_00") != std::string::npos) {
                throttle_hex = hex;
            }
        }
    }

    std::vector<uint8_t> stick_bytes;
    std::vector<uint8_t> throttle_bytes;
    bool have_stick = !stick_hex.empty() && hex_to_bytes(stick_hex, stick_bytes);
    bool have_throttle = !throttle_hex.empty() && hex_to_bytes(throttle_hex, throttle_bytes);

    // Hard-coded stick/throttle → ControllerState mapping removed.
    // This reader only advances time and reports availability; actual mapping is file-driven via HotasMapper.
    // Report ok if any fresh HID data is present (for liveness), but do not synthesize ControllerState here.
    bool any_ok = have_stick || have_throttle;
    snap.ok = any_ok;

    return snap;
}

bool HotasReader::has_stick() const {
    if (!internal_state) return false;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double fresh_thresh = 0.5; // seconds; consider connected only with recent HID activity
    std::lock_guard<std::mutex> g(internal_state->live_mutex);
    for (const auto& kv : internal_state->live_last) {
        const std::string& path = kv.first;
        const auto& entry = kv.second;
        if (path.find("vid_0738&pid_2221") != std::string::npos && path.find("mi_00") != std::string::npos) {
            if (!entry.hex.empty() && entry.hex != "(no data yet)" && entry.ts > 0.0 && (now - entry.ts) <= fresh_thresh) {
                return true;
            }
        }
    }
    return false;
}

bool HotasReader::has_throttle() const {
    if (!internal_state) return false;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double fresh_thresh = 0.5; // seconds; consider connected only with recent HID activity
    std::lock_guard<std::mutex> g(internal_state->live_mutex);
    for (const auto& kv : internal_state->live_last) {
        const std::string& path = kv.first;
        const auto& entry = kv.second;
        if (path.find("vid_0738&pid_a221") != std::string::npos && path.find("mi_00") != std::string::npos) {
            if (!entry.hex.empty() && entry.hex != "(no data yet)" && entry.ts > 0.0 && (now - entry.ts) <= fresh_thresh) {
                return true;
            }
        }
    }
    return false;
}

double HotasReader::latest_time() const { return internal_state ? internal_state->latest.load(std::memory_order_acquire) : 0.0; }

void HotasReader::snapshot_joys(std::vector<Sample>& out_x, std::vector<Sample>& out_y, double window_seconds) const {
    if (!internal_state) { out_x.clear(); out_y.clear(); return; }
    double lt = internal_state->latest.load(std::memory_order_acquire);
    internal_state->joy_x.snapshot(lt, window_seconds, out_x);
    internal_state->joy_y.snapshot(lt, window_seconds, out_y);
}
