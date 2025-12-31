// Docking enabled globally via CMake compile definitions.
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include <imgui.h>
#include <implot.h>
#include <imgui_internal.h>
#include <mmsystem.h>
#include <wincodec.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Windowscodecs.lib")
#include "xinput/xinput_poll.hpp"
#include "ui/plots_panel.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "xinput/filtered_forwarder.hpp"
#include "xinput/hotas_reader.hpp"
#include "xinput/hotas_mapper.hpp"

struct FilterSettings {
    bool enabled = false;
    float analog_delta = 0.25f;
    float analog_return = 0.15f;
    double digital_max_ms = 5.0; // stored in milliseconds for UI
    bool left_trigger_digital = false; // treat LT as digital for filtering
    bool right_trigger_digital = false; // treat RT as digital for filtering
    // Per-signal filter mode: 0=none, 1=digital, 2=analog
    std::array<int, SignalCount> per_signal_mode;
    FilterSettings() { per_signal_mode.fill(0); } // default: none (no filtering)
};

// Global runtime parameters (window_seconds persisted; target_hz fixed at 1 kHz)
static double g_window_seconds = 30.0;   // plot window length (persisted)
static bool g_virtual_output_enabled = false; // persisted flag

static bool LoadFilterSettings(const char* path, FilterSettings& fs) {
    std::ifstream in(path, std::ios::in); if (!in) return false;
    std::string line; std::unordered_map<std::string,std::string> kv; kv.reserve(8);
    
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('='); if (pos==std::string::npos) continue;
        std::string k = line.substr(0,pos); std::string v = line.substr(pos+1);
        kv[k]=v;
    }
    
    auto getb = [&](const char* k, bool d) -> bool {
        auto it = kv.find(k);
        if (it == kv.end()) return d;
        return it->second == "1" || it->second == "true" || it->second == "TRUE";
    };
    auto getf = [&](const char* k, float d) -> float {
        auto it = kv.find(k);
        if (it == kv.end()) return d;
        try { return std::stof(it->second); }
        catch (...) { return d; }
    };
    auto getd = [&](const char* k, double d) -> double {
        auto it = kv.find(k);
        if (it == kv.end()) return d;
        try { return std::stod(it->second); }
        catch (...) { return d; }
    };
    
    fs.enabled = getb("enabled", fs.enabled);
    fs.analog_delta = getf("analog_delta", fs.analog_delta);
    fs.analog_return = getf("analog_return", fs.analog_return);
    fs.digital_max_ms = getd("digital_max_ms", fs.digital_max_ms);
    g_window_seconds = getd("window_seconds", g_window_seconds);
    g_virtual_output_enabled = getb("virtual_output", g_virtual_output_enabled);
    fs.left_trigger_digital = getb("left_trigger_digital", fs.left_trigger_digital);
    fs.right_trigger_digital = getb("right_trigger_digital", fs.right_trigger_digital);
    
    // Load per-signal filter modes: none|digital|analog
    for (size_t i=0;i<SIGNAL_META.size();++i) {
        std::string key = std::string("filter_") + SIGNAL_META[i].name;
        auto it = kv.find(key);
        if (it != kv.end()) {
            const std::string& v = it->second;
            if (v == "digital") fs.per_signal_mode[i] = 1;
            else if (v == "analog") fs.per_signal_mode[i] = 2;
            else fs.per_signal_mode[i] = 0; // default to none
        }
    }
    return true;
}


static void SaveFilterSettings(const char* path, const FilterSettings& fs) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return;
    out << "# Filter settings\n";
    out << "enabled=" << (fs.enabled?1:0) << "\n";
    out << "analog_delta=" << fs.analog_delta << "\n";
    out << "analog_return=" << fs.analog_return << "\n";
    out << "digital_max_ms=" << fs.digital_max_ms << "\n";
    out << "window_seconds=" << g_window_seconds << "\n";
    out << "virtual_output=" << (g_virtual_output_enabled?1:0) << "\n";
    out << "left_trigger_digital=" << (fs.left_trigger_digital?1:0) << "\n";
    out << "right_trigger_digital=" << (fs.right_trigger_digital?1:0) << "\n";
    
    // Save per-signal filter modes
    for (size_t i=0;i<SIGNAL_META.size();++i) {
        out << "filter_" << SIGNAL_META[i].name << "=";
        switch (fs.per_signal_mode[i]) {
            case 1: out << "digital\n"; break;
            case 2: out << "analog\n"; break;
            default: out << "none\n"; break;
        }
    }
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ImGui Win32 + DX11 backend forward decls
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static ID3D11ShaderResourceView* g_backgroundSRV = nullptr;
static int g_bg_width = 0, g_bg_height = 0;

static bool LoadTextureWIC(const wchar_t* filename, ID3D11Device* device, ID3D11DeviceContext* context,
                           ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
    if (!device || !context || !filename || !out_srv) return false;
    *out_srv = nullptr;
    HRESULT hr;
    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return false;
    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(filename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) { factory->Release(); return false; }
    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) { decoder->Release(); factory->Release(); return false; }
    UINT w=0,h=0; frame->GetSize(&w,&h);
    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) { frame->Release(); decoder->Release(); factory->Release(); return false; }
    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { converter->Release(); frame->Release(); decoder->Release(); factory->Release(); return false; }
    std::vector<uint8_t> pixels; pixels.resize((size_t)w * (size_t)h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) { converter->Release(); frame->Release(); decoder->Release(); factory->Release(); return false; }
    converter->Release(); frame->Release(); decoder->Release(); factory->Release();

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = w; texDesc.Height = h; texDesc.MipLevels = 1; texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE; texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initData{}; initData.pSysMem = pixels.data(); initData.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&texDesc, &initData, &tex);
    if (FAILED(hr) || !tex) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = texDesc.Format; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(tex, &srvDesc, out_srv);
    tex->Release();
    if (FAILED(hr) || !*out_srv) return false;
    if (out_width) *out_width = (int)w; if (out_height) *out_height = (int)h;
    return true;
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// Win32 message handler
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Increase timer resolution for better sub-millisecond sleep precision
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Register class
    WNDCLASSEX wc{ sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, _T("XInputPlotter"), nullptr };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("Virtual 360 Controller Filter"), WS_OVERLAPPEDWINDOW, 100, 100, 1600, 900, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Setup style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    extern bool ImGui_ImplWin32_Init(void* hwnd);
    extern void ImGui_ImplWin32_Shutdown();
    extern void ImGui_ImplWin32_NewFrame();
    extern bool ImGui_ImplDX11_Init(ID3D11Device* device, ID3D11DeviceContext* device_context);
    extern void ImGui_ImplDX11_Shutdown();
    extern void ImGui_ImplDX11_NewFrame();
    extern void ImGui_ImplDX11_RenderDrawData(ImDrawData* draw_data);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load background image (try common relative paths)
    const wchar_t* candidates[] = {
        L"res\\graphics\\HOTAS_Controller.png",
        L"..\\res\\graphics\\HOTAS_Controller.png",
        L"..\\..\\res\\graphics\\HOTAS_Controller.png"
    };
    for (auto* path : candidates) {
        if (LoadTextureWIC(path, g_pd3dDevice, g_pd3dDeviceContext, &g_backgroundSRV, &g_bg_width, &g_bg_height)) break;
    }

    // Load persisted settings before starting poller (overrides defaults if present)
    FilterSettings filter_settings; LoadFilterSettings("filter_settings.cfg", filter_settings);

    // Clamp loaded values to sane ranges
    if (g_window_seconds < 1.0) g_window_seconds = 1.0; else if (g_window_seconds > 60.0) g_window_seconds = 60.0;

    // Note: Polling rate is fixed at 1000 Hz (not configurable per spec)
    double fixed_polling_hz = 1000.0;
    XInputPoller poller; poller.start(0, fixed_polling_hz, g_window_seconds);
    HotasReader hotas;
    HotasMapper hotas_mapper;
    // Load persisted HOTAS mappings at startup
    hotas_mapper.load_profile("mappings.json");
    // Do not inject mapped states back into the poller; forwarder handles output.
    static bool show_hotas_detect_window = false;
    static std::vector<std::string> hotas_detect_lines;
    static bool show_developer_view = false;
    // HOTAS is always enabled; no UI toggle
    static bool show_mappings_window = false;
    static FilteredForwarder forwarder;
    poller.set_sink(&forwarder);
    bool virtual_enabled = false; // start disabled; will auto-enable after HOTAS detection
    forwarder.enable_output(false);
    forwarder.enable_filter(filter_settings.enabled);
    forwarder.set_params(filter_settings.analog_delta, filter_settings.digital_max_ms/1000.0);
    forwarder.set_trigger_modes(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
    forwarder.set_filter_modes(filter_settings.per_signal_mode);
    forwarder.set_window_seconds(g_window_seconds);
    PlotsPanel raw_panel(poller, PlotConfig{g_window_seconds, 4000});
    raw_panel.set_filter_mode(false);
    raw_panel.set_filter_thresholds(filter_settings.analog_delta, filter_settings.analog_return, filter_settings.digital_max_ms/1000.0);
    raw_panel.set_trigger_digital(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
    FilterSettings working = filter_settings; // editable working copy
    bool filter_dirty = false;
    // Saved snapshot for window_seconds to participate in dirty tracking
    double saved_window_seconds = g_window_seconds;

    // Background thread to manage HOTAS input continuously, independent of UI focus/rendering.
    // This ensures HOTAS input is read and processed even when the window is minimized or unfocused.
    std::atomic<bool> hotas_bg_thread_running{true};
    std::atomic<bool> hotas_bg_enabled{true};
    std::atomic<bool> hotas_detected{false};
    std::atomic<bool> output_auto_started{false};
    std::thread hotas_background_thread([&]() {
        using clock = std::chrono::steady_clock;
        auto last_ok_tp = clock::now();
        auto next_refresh_tp = clock::now();
        while (hotas_bg_thread_running.load()) {
            // HOTAS input always enabled
            if (hotas_bg_enabled.load()) {
                auto snap = hotas.poll_once();
                auto now_tp = clock::now();
                if (snap.ok) {
                    last_ok_tp = now_tp;
                    if (!hotas_detected.exchange(true)) {
                        // First detection; auto-start virtual output
                        if (!forwarder.output_enabled()) {
                            forwarder.enable_output(true);
                        }
                        output_auto_started.store(true, std::memory_order_release);
                    }
                    double now = std::chrono::duration<double>(now_tp.time_since_epoch()).count();
                    // Inject the raw HOTAS state into the poller pipeline (poller -> filter -> mapper -> output)
                    poller.inject_state(now, snap.state);
                } else {
                    // If we've had no valid HOTAS data for >1s, refresh HID live to re-enumerate devices
                    if (now_tp - last_ok_tp > std::chrono::seconds(1) && now_tp >= next_refresh_tp) {
                        hotas.stop_hid_live();
                        hotas.start_hid_live();
                        next_refresh_tp = now_tp + std::chrono::seconds(2); // cooldown to avoid busy re-enumeration
                        hotas_detected.store(false, std::memory_order_release);
                    }
                }
            }
            // Poll HOTAS at ~250Hz even when disabled (fast wakeup when enabled)
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    });

    // Always start HID live and use external input path
    hotas.start_hid_live();
    poller.set_external_input(true);

    // Controller polling and plotting panel initialization.

    // Main loop
    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Throttle rendering if window minimized or not active (poller continues normally)
        bool minimized = IsIconic(hwnd) != 0;
        bool foreground = (GetForegroundWindow() == hwnd);
        if (minimized || !foreground) {
            // Light sleep to yield CPU; keep polling thread unaffected
            Sleep(80); // ~12.5 FPS update pace while unfocused
            continue; // skip frame build/render
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Draw background image to viewport
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            if (g_backgroundSRV && vp) {
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                ImVec2 pos = vp->Pos;
                ImVec2 size = vp->Size;
                dl->AddImage((void*)g_backgroundSRV, pos, ImVec2(pos.x + size.x, pos.y + size.y));
            }
        }

        // Make panes/docks transparent
        {
            ImGuiStyle& style = ImGui::GetStyle();
            style.Colors[ImGuiCol_WindowBg].w = 0.0f;
            style.Colors[ImGuiCol_ChildBg].w = 0.0f;
            style.Colors[ImGuiCol_MenuBarBg].w = 0.2f;
            style.Colors[ImGuiCol_TitleBg].w = 0.3f;
            style.Colors[ImGuiCol_TitleBgActive].w = 0.4f;
        }

        // Dockspace window + initial layout (Control left, Signals main) on first frame
        {
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::Begin("DockSpace", nullptr, flags);
            ImGui::PopStyleVar(2);
            ImGuiID dock_id = ImGui::GetID("MyDockSpace");
            ImGui::DockSpace(dock_id, ImVec2(0,0));

            // Menu bar: add Edit -> Mappings and Help -> Detect Inputs
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("Edit")) {
                    if (ImGui::MenuItem("Mappings...")) {
                        show_mappings_window = true;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Help")) {
                        if (ImGui::MenuItem("Detect Inputs...")) {
                            hotas_detect_lines = HotasReader::enumerate_devices();
                            // also pull debug lines
                            auto dbg = HotasReader::debug_lines();
                            if (!dbg.empty()) {
                                hotas_detect_lines.insert(hotas_detect_lines.end(), dbg.begin(), dbg.end());
                            }
                            show_hotas_detect_window = true;
                        }
                        static bool show_developer_view_menu = false;
                        if (ImGui::MenuItem("Developer View", nullptr, &show_developer_view_menu)) {
                            // toggle developer view; actual docking handled after layout build
                            show_developer_view = show_developer_view_menu;
                        }
                        ImGui::EndMenu();
                    }
                ImGui::EndMenuBar();
            }

            static bool layout_built = false;
            if (!layout_built) {
                layout_built = true;
                ImGui::DockBuilderRemoveNode(dock_id);            // clear any previous (in case of hot reload)
                ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dock_id, viewport->Size);
                // Create a three-column layout: left (Control), middle (Raw Signals), right (Filtered Signals)
                ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Left, 0.22f, nullptr, &dock_id); // 22% left panel
                ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Right, 0.33f, nullptr, &dock_id); // 33% right panel
                ImGuiID dock_main = dock_id; // middle area (remaining)
                ImGui::DockBuilderDockWindow("Control", dock_left);
                ImGui::DockBuilderDockWindow("Raw Signals", dock_main);
                ImGui::DockBuilderDockWindow("HOTAS Raw", dock_main);
                ImGui::DockBuilderDockWindow("Filtered Signals", dock_right);
                ImGui::DockBuilderDockWindow("Mappings", dock_right);
                ImGui::DockBuilderFinish(dock_id);
            }
            // Developer view docking: create bottom dock for HID Live when requested
            static bool dev_dock_created = false;
            // When developer view requested, create a bottom dock and place "HID Live" there
            if (show_developer_view && !dev_dock_created) {
                ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Down, 0.25f, nullptr, &dock_id);
                ImGui::DockBuilderDockWindow("HID Live", dock_bottom);
                ImGui::DockBuilderFinish(dock_id);
                dev_dock_created = true;
            } else if (!show_developer_view && dev_dock_created) {
                // move HID Live back to main dock area
                ImGui::DockBuilderDockWindow("HID Live", dock_id);
                ImGui::DockBuilderFinish(dock_id);
                dev_dock_created = false;
            }
            ImGui::End();
        }

        // Control window
        ImGui::Begin("Control", nullptr, ImGuiWindowFlags_NoBackground);
        auto stats = poller.stats();
        ImGui::Text("Effective Hz: %.1f", stats.effective_hz);
        // Sync UI checkbox with backend state
        virtual_enabled = forwarder.output_enabled();
        // Virtual controller output toggle
        if (ImGui::Checkbox("Virtual Output", &virtual_enabled)) {
            // Try to enable/disable; backend will init on demand
            forwarder.enable_output(virtual_enabled);
            if (forwarder.output_enabled()) {
                g_virtual_output_enabled = true;
                virtual_enabled = true;
            } else {
                g_virtual_output_enabled = false;
                virtual_enabled = false;
            }
        }
        ImGui::SameLine(); ImGui::TextDisabled(forwarder.backend_status());
        ImGui::Text("Backend: %s | Output: %s", forwarder.backend_status(), forwarder.output_enabled() ? "On" : "Off");
        // Controller selection (0-3)
        static int controller_idx = 0;
        controller_idx = poller.controller_index();
        ImGui::SetNextItemWidth(80);
        if (ImGui::SliderInt("Controller Index", &controller_idx, 0, 3)) {
            poller.set_controller_index(controller_idx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto Detect")) {
            // Probe all 4 once and pick the first reporting connected
            int found = -1;
            for (int i=0;i<4;i++) {
                XINPUT_STATE st{}; if (XInputGetState(i,&st)==ERROR_SUCCESS) { found = i; break; }
            }
            if (found>=0) { poller.set_controller_index(found); }
        }
        ImGui::SetItemTooltip("Pick the physical controller. Auto Detect chooses the first connected (could be the emulated one if enabled).");
        // HOTAS input is always enabled; no toggle displayed.

        if (virtual_enabled) {
            const char* upd = forwarder.last_update_status();
            if (upd && *upd) { ImGui::TextColored(ImVec4(1,0.2f,0.2f,1), "Last Update: %s", upd); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Test Pulse")) {
                forwarder.trigger_test_pulse();
            }
            ImGui::SetItemTooltip("Injects a synthetic full-press state (ABXY+Shoulders, full sticks/triggers) once");
        }
        if (stats.avg_loop_us > 0.0) {
            ImGui::Text("Avg loop: %.2f us", stats.avg_loop_us);
        }
        ImGui::TextDisabled("Polling rate: 1000 Hz (fixed)");
        // Window length controls (1 - 60 seconds)
        double win = raw_panel.window_seconds();
        double win_min = 1.0, win_max = 60.0;
        if (ImGui::SliderScalar("Window (s)", ImGuiDataType_Double, &win, &win_min, &win_max, "%.0f")) {
            poller.set_window_seconds(win);
            raw_panel.set_window_seconds(win);
            forwarder.set_window_seconds(win);
            g_window_seconds = win;
        }
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputDouble("Window Exact", &win, 0.1, 1.0, "%.1f")) {
            if (win < 1.0) win = 1.0; else if (win > 60.0) win = 60.0;
            poller.set_window_seconds(win);
            raw_panel.set_window_seconds(win);
            forwarder.set_window_seconds(win);
            g_window_seconds = win;
        }
        if (ImGui::Button("Clear Plots")) { poller.clear(); forwarder.clear_filtered(); }
        // Filter / Anomaly detection controls
        // Filter settings (persistent across runs)
        bool filter_mode = working.enabled;
        float analog_delta = working.analog_delta;
        float analog_return = working.analog_return;
        double digital_max = working.digital_max_ms;
        if (ImGui::CollapsingHeader("Filter Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Checkbox("Enable Filter Mode", &filter_mode)) {
                raw_panel.set_filter_mode(false);
                working.enabled = filter_mode;
                filter_dirty = true;
                forwarder.enable_filter(filter_mode);
            }
            if (filter_mode) {
                bool updated = false;
                updated |= ImGui::SliderFloat("Analog Spike Delta", &analog_delta, 0.05f, 1.0f, "%.2f");
                updated |= ImGui::SliderFloat("Analog Spike Return", &analog_return, 0.05f, 1.0f, "%.2f");
                double dp_min = 0.1; double dp_max = 500.0; // 0.1ms .. 500ms
                updated |= ImGui::SliderScalar("Digital Pulse Max (ms)", ImGuiDataType_Double, &digital_max, &dp_min, &dp_max, "%.2f");
                bool lt_dig = working.left_trigger_digital;
                bool rt_dig = working.right_trigger_digital;
                if (ImGui::Checkbox("Left Trigger Digital", &lt_dig)) {
                    working.left_trigger_digital = lt_dig;
                    raw_panel.set_trigger_digital(lt_dig, working.right_trigger_digital);
                    forwarder.set_trigger_modes(lt_dig, working.right_trigger_digital);
                    filter_dirty = true;
                }
                if (ImGui::Checkbox("Right Trigger Digital", &rt_dig)) {
                    working.right_trigger_digital = rt_dig;
                    raw_panel.set_trigger_digital(working.left_trigger_digital, rt_dig);
                    forwarder.set_trigger_modes(working.left_trigger_digital, rt_dig);
                    filter_dirty = true;
                }
                if (updated) {
                    raw_panel.set_filter_thresholds(analog_delta, analog_return, digital_max/1000.0);
                    working.analog_delta = analog_delta;
                    working.analog_return = analog_return;
                    working.digital_max_ms = digital_max;
                    filter_dirty = true;
                    forwarder.set_params(analog_delta, digital_max/1000.0);
                }
                
                ImGui::SeparatorText("Per-Input Filter Modes");
                ImGui::TextDisabled("Select filter mode for each input: None (raw), Digital (debounce), or Analog (spike suppress).");
                // Color table (repeatable palette)
                static ImVec4 cols[] = {
                    {0.86f,0.58f,0.24f,1},{0.30f,0.75f,0.93f,1},{0.50f,0.70f,0.30f,1},{0.90f,0.30f,0.30f,1},
                    {0.70f,0.50f,0.90f,1},{0.95f,0.80f,0.30f,1},{0.40f,0.85f,0.60f,1},{0.80f,0.40f,0.80f,1}
                };
                auto draw_mode_selector = [&](Signal sig, const char* label){
                    size_t idx = (size_t)sig;
                    ImGui::PushStyleColor(ImGuiCol_Text, cols[idx % IM_ARRAYSIZE(cols)]);
                    int mode = working.per_signal_mode[idx];
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    const char* items[] = { "None", "Digital", "Analog" };
                    if (ImGui::Combo(("##mode_" + std::to_string(idx)).c_str(), &mode, items, IM_ARRAYSIZE(items))) {
                        working.per_signal_mode[idx] = mode; 
                        filter_dirty = true;
                        forwarder.set_filter_modes(working.per_signal_mode);
                    }
                    ImGui::PopStyleColor();
                };
                // Grouped layout - explicitly define columns to avoid collapsing one column when narrow
                if (ImGui::BeginTable("filter_modes", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Signals1");
                    ImGui::TableSetupColumn("Signals2");
                    // First row: sticks
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::LeftX, "Left Stick X");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::RightX, "Right Stick X");
                    // Second row: more sticks
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::LeftY, "Left Stick Y");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::RightY, "Right Stick Y");
                    // Third row: triggers & shoulders
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::LeftTrigger, "Left Trigger");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::RightTrigger, "Right Trigger");
                    // Fourth row: shoulders
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::LeftShoulder, "Left Shoulder");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::RightShoulder, "Right Shoulder");
                    // Fifth row: face buttons
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::A, "Face Button A");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::B, "Face Button B");
                    // Sixth row: more face
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::X, "Face Button X");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::Y, "Face Button Y");
                    // Seventh row: system
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::StartBtn, "Start / Menu");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::BackBtn, "Back / View");
                    // Eighth row: thumb buttons
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::LeftThumbBtn, "Left Thumb (L3)");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::RightThumbBtn, "Right Thumb (R3)");
                    // Ninth row: d-pad
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::DPadUp, "D-Pad Up");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::DPadDown, "D-Pad Down");
                    // Tenth row: more d-pad
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); draw_mode_selector(Signal::DPadLeft, "D-Pad Left");
                    ImGui::TableSetColumnIndex(1); draw_mode_selector(Signal::DPadRight, "D-Pad Right");
                    ImGui::EndTable();
                }

                ImGui::TextDisabled("Digital mode detects rising edges and requires sustained press; Analog mode suppresses jitter spikes.");
                bool runtime_dirty = (g_window_seconds != saved_window_seconds);
                bool any_dirty = filter_dirty || runtime_dirty;

                if (any_dirty) { 
                    ImGui::SameLine();
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.6f,0,1), "*modified");
                }

                // Always show buttons; disable when no pending changes (filter or runtime params)
                ImGui::BeginDisabled(!any_dirty);
                if (ImGui::Button("Save Settings")) {
                    if (any_dirty) {
                        if (filter_dirty) {
                            filter_settings = working;
                            forwarder.set_filter_modes(filter_settings.per_signal_mode);
                            forwarder.set_trigger_modes(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
                            raw_panel.set_trigger_digital(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
                        }
                        // Persist current runtime + filter settings
                        SaveFilterSettings("filter_settings.cfg", filter_settings);
                        // Update snapshots
                        saved_window_seconds = g_window_seconds;
                        filter_dirty = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Revert")) {
                    if (any_dirty) {
                        if (filter_dirty) {
                            working = filter_settings;
                            raw_panel.set_filter_mode(false);
                            raw_panel.set_filter_thresholds(
                                working.analog_delta,
                                working.analog_return,
                                working.digital_max_ms/1000.0);
                            forwarder.enable_filter(working.enabled);
                            forwarder.set_params(
                                working.analog_delta,
                                working.digital_max_ms/1000.0);
                        }
                        if (runtime_dirty) {
                            g_window_seconds = saved_window_seconds;
                            poller.set_window_seconds(g_window_seconds);
                            raw_panel.set_window_seconds(g_window_seconds);
                            forwarder.set_window_seconds(g_window_seconds);
                        }
                        filter_dirty = false;
                    }
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::End();

        // HOTAS detection window (Help -> Detect Inputs...)
        if (show_hotas_detect_window) {
            ImGui::Begin("Detect HOTAS Devices", &show_hotas_detect_window, ImGuiWindowFlags_NoBackground);
            ImGui::TextWrapped("This lists DirectInput game controller devices. Use Refresh to rescan. Click Save to write results to hotas_devices.txt.");
            if (ImGui::Button("Refresh")) {
                hotas_detect_lines = HotasReader::enumerate_devices();
                auto dbg = HotasReader::debug_lines();
                if (!dbg.empty()) {
                    hotas_detect_lines.insert(hotas_detect_lines.end(), dbg.begin(), dbg.end());
                } else {
                    hotas_detect_lines.push_back("Detected ProductName: not-found");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                std::ofstream out("hotas_devices.txt", std::ios::out | std::ios::trunc);
                for (auto &l : hotas_detect_lines) out << l << "\n";
                out.close();
            }
            ImGui::Separator();
            if (hotas_detect_lines.empty()) {
                ImGui::TextDisabled("No devices found. Press Refresh to rescan.");
            } else {
                for (size_t i = 0; i < hotas_detect_lines.size(); ++i) {
                    ImGui::TextUnformatted(hotas_detect_lines[i].c_str());
                }
            }
            ImGui::End();
        }

        // Mappings window (Edit -> Mappings...)
        if (show_mappings_window) {
            ImGui::Begin("Mappings", &show_mappings_window);
            // List existing mappings
            if (ImGui::Button("Refresh")) {
                // noop: will be refreshed on draw
            }
            ImGui::SetItemTooltip("Refresh the mapping list from the current session.");
            ImGui::SameLine();
            if (ImGui::Button("Save...")) {
                hotas_mapper.save_profile("mappings.json");
            }
            ImGui::SetItemTooltip("Save all mappings to 'mappings.json' in the application directory for persistence across runs.");
            ImGui::SameLine();
            if (ImGui::Button("Load...")) {
                hotas_mapper.load_profile("mappings.json");
            }
            ImGui::SetItemTooltip("Load mappings from 'mappings.json', replacing the current mapping list.");
            ImGui::Separator();

            // Add mapping form
            static char new_id[64] = "m1";
            static int device_sel = 0; // 0=All,1=Stick,2=Throttle
            const char* device_names[] = { "All", "Stick", "Throttle" };
            ImGui::Combo("Device", &device_sel, device_names, IM_ARRAYSIZE(device_names));
            ImGui::SetItemTooltip("Filter HOTAS signals by device: All (all signals), Stick (joystick inputs), or Throttle (throttle/quadrant inputs)");

            // Build signal list from HotasReader signals and apply simple device filter
            auto sigs = hotas.list_signals();
            std::vector<std::string> sig_ids; sig_ids.reserve(sigs.size());
            for (auto &sd : sigs) {
                bool include = false;
                if (device_sel == 0) include = true;
                else if (device_sel == 1) { // Stick heuristics: name contains JOY or common stick buttons
                    std::string nm = sd.name;
                    for (auto &c : nm) c = (char)toupper((unsigned char)c);
                    if (nm.find("JOY") != std::string::npos) include = true;
                    if (nm == "TRIGGER" || nm == "BTN_A" || nm == "BTN_B" || nm == "C" ) include = true;
                } else if (device_sel == 2) { // Throttle heuristics: name contains THR or TRIGGER
                    std::string nm = sd.name;
                    for (auto &c : nm) c = (char)toupper((unsigned char)c);
                    if (nm.find("THR") != std::string::npos || nm.find("THROTTLE") != std::string::npos) include = true;
                    if (nm.find("TRIGGER") != std::string::npos) include = true; // triggers may live on throttle
                }
                if (include) sig_ids.push_back(sd.id);
            }
            if (sig_ids.empty()) {
                // fallback to all known signals
                for (auto &sd : sigs) sig_ids.push_back(sd.id);
            }

            static int sig_sel = 0;
            std::vector<const char*> sig_items; sig_items.reserve(sig_ids.size());
            for (auto &s : sig_ids) sig_items.push_back(s.c_str());

            ImGui::InputText("Mapping ID", new_id, sizeof(new_id));
            ImGui::SetItemTooltip("Unique identifier for this mapping (e.g., 'm1', 'stick_x_axis'). Used to manage and remove mappings.");
            if (!sig_items.empty()) {
                ImGui::Combo("Signal ID", &sig_sel, sig_items.data(), (int)sig_items.size());
                ImGui::SetItemTooltip("Select the HOTAS signal to map (e.g., 'joy_x', 'throttle', 'trigger_left'). Filters are applied based on the Device selection above.");
            } else {
                ImGui::TextDisabled("No signals available for selected device");
            }
            // Action selector: prefer structured choices instead of free text
            static const char* action_types[] = { "x360", "keyboard", "mouse" };
            static int action_type_sel = 0;
            ImGui::Combo("Action Type", &action_type_sel, action_types, IM_ARRAYSIZE(action_types));
            ImGui::SetItemTooltip("Type of action to trigger: x360 (virtual Xbox controller), keyboard (key press), or mouse (cursor/click)");

            // Xbox 360 inputs list
            static const char* x360_labels[] = {
                "Left X (axis)", "Left Y (axis)", "Right X (axis)", "Right Y (axis)",
                "Left Trigger", "Right Trigger",
                "A", "B", "X", "Y",
                "Left Shoulder", "Right Shoulder",
                "Back", "Start", "Left Thumb Press", "Right Thumb Press",
                "DPad Up", "DPad Down", "DPad Left", "DPad Right"
            };
            static const char* x360_actions[] = {
                "x360:left_x","x360:left_y","x360:right_x","x360:right_y",
                "x360:left_trigger","x360:right_trigger",
                "x360:button_a","x360:button_b","x360:button_x","x360:button_y",
                "x360:left_shoulder","x360:right_shoulder",
                "x360:back","x360:start","x360:left_thumb","x360:right_thumb",
                "x360:dpad_up","x360:dpad_down","x360:dpad_left","x360:dpad_right"
            };
            static int x360_sel = 0;
            static char keyboard_action[64] = "";
            static char mouse_action[64] = "";
            static double new_param = 1.0;
            if (action_type_sel == 0) {
                ImGui::Combo("X360 Input", &x360_sel, x360_labels, IM_ARRAYSIZE(x360_labels));
                ImGui::SetItemTooltip("Select the target Xbox 360 input: axes (sticks, triggers) or buttons (A/B/X/Y, shoulders, DPad, thumb presses, etc.)");
            } else if (action_type_sel == 1) {
                ImGui::InputText("Keyboard (e.g. VK_SPACE or 'A')", keyboard_action, sizeof(keyboard_action));
                ImGui::SetItemTooltip("Enter a keyboard key code (VK_* constant) or a single character (e.g., 'VK_SPACE', 'A', 'Enter'). Check Windows virtual key codes for reference.");
            } else {
                ImGui::InputText("Mouse Action (e.g. left_click)", mouse_action, sizeof(mouse_action));
                ImGui::SetItemTooltip("Enter a mouse action: left_click, right_click, move_up, move_down, scroll_up, scroll_down, etc.");
            }
            ImGui::InputDouble("Param", &new_param, 0.1, 1.0, "%.3f");
            ImGui::SetItemTooltip("Optional parameter for the action (scale, deadzone, etc.). Default is 1.0. Interpretations depend on the action type.");
            if (ImGui::Button("Add Mapping")) {
                MappingEntry e;
                e.id = std::string(new_id);
                // selected signal id
                if (!sig_ids.empty()) e.signal_id = sig_ids[sig_sel]; else e.signal_id = std::string("");
                if (action_type_sel == 0) {
                    e.action = std::string(x360_actions[x360_sel]);
                } else if (action_type_sel == 1) {
                    e.action = std::string("keyboard:") + keyboard_action;
                } else {
                    e.action = std::string("mouse:") + mouse_action;
                }
                e.param = new_param;
                if (!hotas_mapper.add_mapping(e)) {
                    ImGui::TextColored(ImVec4(1,0.2f,0.2f,1), "Add failed: id exists");
                }
            }
            ImGui::SetItemTooltip("Create a new mapping from the HOTAS signal to the selected action. The Mapping ID must be unique.");
            ImGui::Separator();

            // Show table of mappings
            auto entries = hotas_mapper.list_mapping_entries();
            if (ImGui::BeginTable("mappings_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("Signal");
                ImGui::TableSetupColumn("Action");
                ImGui::TableSetupColumn("Param");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < entries.size(); ++i) {
                    const auto &me = entries[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(me.id.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(me.signal_id.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(me.action.c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f", me.param);
                    // Remove button per row
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::SmallButton((std::string("Remove##") + me.id).c_str())) {
                        hotas_mapper.remove_mapping(me.id);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }

        ImGui::Begin("Raw Signals", nullptr, ImGuiWindowFlags_NoBackground);
        raw_panel.draw();
        ImGui::End();

        // HOTAS Raw window: parse HID live hex and show raw integer graphs for mapped inputs
        ImGui::Begin("HOTAS Raw", nullptr, ImGuiWindowFlags_NoBackground);
        // Mapping from CSV: device stick (Saitek X56)
        struct HidInputMap { const char* id; const char* name; int bit_start; int bits; bool analog; };
        static const std::vector<HidInputMap> stick_map = {
            {"joy_x",   "JOY_X",   8, 16, true},
            {"joy_y",   "JOY_Y",  24, 16, true},
            {"joy_z",   "JOY_Z",  40, 12, true},
            {"c_joy_x", "C_JOY_X",80,  8, true},
            {"c_joy_y", "C_JOY_Y",88,  8, true},
            {"C",       "C",      59,  1, false},
            {"trigger", "TRIGGER",56,  1, false},
            {"A",       "BTN_A",  57,  1, false},
            {"B",       "BTN_B",  58,  1, false},
            {"D",       "BTN_D",  60,  1, false},
            {"E",       "BTN_E",  61,  1, false},
            {"POV",     "POV",    52,  4, false},
            {"H1",      "H1",     62,  4, false},
            {"H2",      "H2",     66,  4, false},
        };

        // Per-signal sample buffers
        struct Buf { std::vector<double> t, v; };
        static std::unordered_map<std::string, Buf> g_hid_buffers;

        // Helper: parse hex string to bytes
        auto hex_to_bytes = [&](const std::string &hex, std::vector<uint8_t> &out) {
            out.clear();
            auto hexval = [](char c)->int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return 0;
            };
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                int hi = hexval(hex[i]), lo = hexval(hex[i+1]);
                out.push_back((uint8_t)((hi << 4) | lo));
            }
        };

        // Acquire latest HID live snapshot and pick first non-empty report
        auto hid_snap = hotas.get_hid_live_snapshot();
        std::vector<uint8_t> hid_bytes;
        bool have_stick_report = false;
        for (auto &p : hid_snap) {
            if (p.second.empty() || p.second == "(no data yet)") continue;
            hex_to_bytes(p.second, hid_bytes);
            if (!hid_bytes.empty()) { have_stick_report = true; break; }
        }

        double now_ts = hotas.latest_time();
        if (!have_stick_report) {
            ImGui::TextDisabled("No HID stick report available yet.");
        } else {
            double window = raw_panel.window_seconds();
            double t0 = now_ts - window;

            std::unordered_map<std::string,double> logical_vals;
            for (auto &m : stick_map) {
                uint64_t val = 0;
                int last_bit = m.bit_start + m.bits - 1;
                size_t needed_bytes = (last_bit / 8) + 1;
                if (hid_bytes.size() < needed_bytes) continue;

                // Extract bits LSB-first into val
                for (int i = 0; i < m.bits; ++i) {
                    int bit_global = m.bit_start + i;
                    size_t byte_idx = bit_global / 8;
                    int bit_in_byte = bit_global % 8; // LSB-first
                    int bitv = (hid_bytes[byte_idx] >> bit_in_byte) & 1;
                    val |= (uint64_t(bitv) << i);
                }

                double plotted = 0.0;
                double y_min = 0.0, y_max = 1.0;
                // By default plot raw integer ranges; for signed wide analogs treat as signed.
                bool normalize_to_unit = false;
                std::string mid(m.id);
                if (m.analog && m.bits >= 12) {
                    // Special-case: joystick axes (joy_x, joy_y, joy_z) are unsigned in your device
                    uint64_t maxv = (1ULL << m.bits) - 1;
                    if (mid == "joy_x" || mid == "joy_y" || mid == "joy_z") {
                        // treat as unsigned 0..2^n-1 and normalize to -1..1
                        plotted = (double)val;
                        if (maxv != 0) {
                            plotted = (plotted / (double)maxv) * 2.0 - 1.0;
                        }
                        y_min = -1.0; y_max = 1.0;
                        normalize_to_unit = true;
                    } else {
                        // treat as signed two's complement for other wide analogs
                        uint64_t sign_bit = (1ULL << (m.bits - 1));
                        int64_t sval = (val & sign_bit) ? (int64_t)val - (1LL << m.bits) : (int64_t)val;
                        // default axis range is raw signed integer
                        y_min = -(double)(1ULL << (m.bits - 1));
                        y_max =  (double)((1ULL << (m.bits - 1)) - 1);
                        plotted = (double)sval;
                    }
                } else if (m.analog) {
                    // smaller analog fields: plot unsigned 0..2^n-1
                    double maxv = (double)((1ULL << m.bits) - 1);
                    y_min = 0.0;
                    y_max = maxv;
                    plotted = (double)val;
                    // normalize center-stick small-axis? only normalize full sticks above
                } else {
                    // digital / enumeration
                    y_min = 0.0;
                    y_max = (double)((1ULL << m.bits) - 1);
                    plotted = (double)val;
                }

                // Append to buffer and trim by time window
                Buf &b = g_hid_buffers[m.name];
                b.t.push_back(now_ts);
                b.v.push_back(plotted);
                // store logical sample for constructing ControllerState
                logical_vals[m.id] = plotted;
                size_t first_keep = 0;
                while (first_keep < b.t.size() && b.t[first_keep] < t0) ++first_keep;
                if (first_keep > 0) {
                    b.t.erase(b.t.begin(), b.t.begin() + first_keep);
                    b.v.erase(b.v.begin(), b.v.begin() + first_keep);
                }

            }

                // Build a ControllerState from logical values (for visualization only)
                XInputPoller::ControllerState cs{};
                auto getv = [&](const char* k, double def=0.0)->double {
                    auto it = logical_vals.find(k);
                    if (it == logical_vals.end()) return def; return it->second;
                };
                // Map HOTAS signals to controller axes (best-effort)
                cs.lx = (float)getv("joy_x", 0.0);
                cs.ly = (float)getv("joy_y", 0.0);
                cs.rx = (float)getv("c_joy_x", 0.0);
                cs.ry = (float)getv("c_joy_y", 0.0);
                cs.lt = (float)fmax(0.0, getv("trigger", 0.0));
                cs.rt = 0.0f;
                // Buttons: set bits when logical value > 0.5
                WORD btns = 0;
                if (getv("A", 0.0) > 0.5) btns |= XINPUT_GAMEPAD_A;
                if (getv("B", 0.0) > 0.5) btns |= XINPUT_GAMEPAD_B;
                if (getv("D", 0.0) > 0.5) btns |= XINPUT_GAMEPAD_X;
                if (getv("E", 0.0) > 0.5) btns |= XINPUT_GAMEPAD_Y;
                cs.buttons = btns;
                // Do not inject here; background thread handles injection independent of UI focus.
                
                // Grouped plots per request
                auto plot_group = [&](const char* title, const std::vector<std::pair<const char*, const char*>>& series, float y_min, float y_max) {
                    // Build each series from buffered samples
                    struct S { std::vector<double> x; std::vector<double> y; const char* name; };
                    std::vector<S> all;
                    for (auto &p : series) {
                        auto it = g_hid_buffers.find(p.first);
                        if (it == g_hid_buffers.end()) continue;
                        const Buf &buf = it->second;
                        S s; s.name = p.second;
                        s.x.reserve(buf.t.size());
                        s.y.reserve(buf.v.size());
                        for (size_t i = 0; i < buf.t.size(); ++i) {
                            double rel = buf.t[i] - t0;
                            if (rel < 0) continue;
                            s.x.push_back(rel);
                            s.y.push_back(buf.v[i]);
                        }
                        if (!s.x.empty()) all.push_back(std::move(s));
                    }
                    if (all.empty()) return;
                    if (ImPlot::BeginPlot(title, ImVec2(-1, 130), ImPlotFlags_NoTitle)) {
                        ImPlot::SetupAxes("Time (s)", "Value", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
                        ImPlot::SetupAxisLimits(ImAxis_X1, 0, window, ImGuiCond_Always);
                        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);
                        for (auto &s : all) {
                            ImPlot::PlotLine(s.name, s.x.data(), s.y.data(), (int)s.x.size());
                        }
                        ImPlot::EndPlot();
                    }
                };

                // Joy Stick: JOY_X, JOY_Y, JOY_Z (normalized -1..1)
                plot_group("Joy Stick", { {"JOY_X","x"}, {"JOY_Y","y"}, {"JOY_Z","z"} }, -1.0f, 1.0f);
                // C-Joy: C_JOY_X, C_JOY_Y (0..255)
                plot_group("C-Joy", { {"C_JOY_X","x"}, {"C_JOY_Y","y"} }, 0.0f, 255.0f);
                // Triggers: TRIGGER, BTN_E (0..1)
                plot_group("Triggers", { {"TRIGGER","Trigger"}, {"BTN_E","pinky trigger"} }, 0.0f, 1.0f);
                // Buttons: BTN_A, BTN_B, C, BTN_D (0..1)
                plot_group("Buttons", { {"BTN_A","A"}, {"BTN_B","B"}, {"C","C"}, {"BTN_D","D"} }, 0.0f, 1.0f);
                // POV: 0..15
                plot_group("POV", { {"POV","POV"} }, 0.0f, 15.0f);
                // H1: 0..15
                plot_group("H1", { {"H1","H1"} }, 0.0f, 15.0f);
                // H2: 0..15
                plot_group("H2", { {"H2","H2"} }, 0.0f, 15.0f);
            }
        ImGui::End();

        // HID Live monitor (temporary) - only visible in Developer View
        static bool hid_live_running = false;
        if (show_developer_view) {
            ImGui::Begin("HID Live", nullptr, ImGuiWindowFlags_NoBackground);
            if (!hid_live_running) {
                if (ImGui::Button("Start HID Live")) { hotas.start_hid_live(); hid_live_running = true; }
            } else {
                if (ImGui::Button("Stop HID Live")) { hotas.stop_hid_live(); hid_live_running = false; }
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh Devices")) {
                hotas.stop_hid_live(); hid_live_running = false;
                hotas.enumerate_devices();
            }
        ImGui::Separator();
        // table: device path | last hex
        // Allow resizing of columns by enabling resizable flag and sizing stretch
        if (ImGui::BeginTable("hid_live_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendX)) {
            ImGui::TableSetupColumn("Device Path", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Last Report (hex)", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableHeadersRow();
            auto live_snap = hotas.get_hid_live_snapshot();
            for (auto &p : live_snap) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(p.first.c_str());
                ImGui::TableSetColumnIndex(1);
                // Right cell: show raw hex and below render tables of 8-bit groups
                if (p.second.empty() || p.second == "(no data yet)") {
                    ImGui::TextUnformatted(p.second.c_str());
                } else {
                    const std::string &hex = p.second;
                    // Show grouped hex bytes on one line
                    std::string grouped;
                    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                        if (!grouped.empty()) grouped += ' ';
                        grouped += hex.substr(i, 2);
                    }
                    ImGui::TextUnformatted(grouped.c_str());

                    // Convert hex string to bytes
                    auto hexval = [&](char c)->int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                        return 0;
                    };
                    std::vector<uint8_t> live_bytes;
                    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                        int v = (hexval(hex[i]) << 4) | hexval(hex[i+1]);
                        live_bytes.push_back((uint8_t)v);
                    }

                    size_t total_bits = live_bytes.size() * 8;
                    if (total_bits == 0) continue;
                    size_t tables = (total_bits + 7) / 8; // number of 8-bit tables

                    // Render tables vertically (stacked), each table is 2 rows x 8 cols
                    for (size_t t = 0; t < tables; ++t) {
                        // Each child gets a unique id using both device path and table index
                        std::string child_id = std::string("hidlive_tbl_") + p.first + "_" + std::to_string(t);
                        std::string table_id = std::string("tbl_") + p.first + "_" + std::to_string(t);
                        ImGui::BeginChild(child_id.c_str(), ImVec2(0, 60), true);
                        if (ImGui::BeginTable(table_id.c_str(), 8, ImGuiTableFlags_SizingFixedFit)) {
                            // First row: global bit indices
                            ImGui::TableNextRow();
                            for (int c = 0; c < 8; ++c) {
                                size_t bit_global = t * 8 + c;
                                ImGui::TableSetColumnIndex(c);
                                if (bit_global < total_bits) {
                                    ImGui::TextUnformatted(std::to_string(bit_global).c_str());
                                } else {
                                    ImGui::TextUnformatted("");
                                }
                            }
                            // Second row: bit values MSB-first (bit 7 .. 0 within a byte)
                            ImGui::TableNextRow();
                            for (int c = 0; c < 8; ++c) {
                                size_t bit_global = t * 8 + c;
                                ImGui::TableSetColumnIndex(c);
                                if (bit_global < total_bits) {
                                    size_t byte_idx = bit_global / 8;
                                    // HID reports and many device encodings are little-endian with
                                    // bitfields defined LSB-first within bytes. Extract the bit
                                    // using LSB-first ordering (bit 0 is least-significant).
                                    int bit_in_byte = (int)(bit_global % 8);
                                    uint8_t bv = live_bytes[byte_idx];
                                    int bitval = (bv >> bit_in_byte) & 1;
                                    ImGui::TextUnformatted(bitval ? "1" : "0");
                                } else {
                                    ImGui::TextUnformatted("");
                                }
                            }
                            ImGui::EndTable();
                        }
                        ImGui::EndChild();
                    }
                }
            }
            ImGui::EndTable();
        }
            ImGui::End();
        } else {
            // Developer view hidden: ensure HID live monitor stopped and window not shown
            if (hid_live_running) {
                hotas.stop_hid_live(); hid_live_running = false;
            }
        }

        ImGui::Begin("Filtered Signals", nullptr, ImGuiWindowFlags_NoBackground);
        {
            auto build_step_series = [](const std::vector<Sample>& in,
                                        double t0,
                                        double window_end,
                                        std::vector<double>& x,
                                        std::vector<double>& y) {
                x.clear();
                y.clear();
                if (in.empty()) return;
                float current = in.front().v;
                double prev_t = in.front().t;
                if (prev_t < t0) prev_t = t0;
                x.push_back(prev_t - t0);
                y.push_back(current);
                for (size_t i = 1; i < in.size(); ++i) {
                    const auto &s = in[i];
                    double tt = s.t;
                    if (tt < t0) continue;
                    if (s.v == current) continue;
                    double rel = tt - t0;
                    x.push_back(rel);
                    y.push_back(current);
                    current = s.v;
                    x.push_back(rel);
                    y.push_back(current);
                }
                if (!x.empty() && x.back() < window_end) {
                    x.push_back(window_end);
                    y.push_back(y.back());
                }
            };
            auto plot_analog_group = [&](const char* label,
                                         std::initializer_list<std::pair<Signal,const char*>> sigs,
                                         float y_min,
                                         float y_max) {
                double latest = forwarder.latest_filtered_time();
                double w = forwarder.window_seconds();
                double t0 = latest - w;
                struct S { std::vector<double> x; std::vector<double> y; const char* name; };
                std::vector<S> series;
                std::vector<Sample> tmp;
                for (auto &p : sigs) {
                    forwarder.snapshot_filtered(p.first, tmp);
                    if (tmp.empty()) continue;
                    S s; s.name = p.second;
                    s.x.reserve(tmp.size());
                    s.y.reserve(tmp.size());
                    for (auto &sm : tmp) {
                        if (sm.t >= t0) {
                            s.x.push_back(sm.t - t0);
                            s.y.push_back(sm.v);
                        }
                    }
                    if (!s.x.empty()) series.push_back(std::move(s));
                }
                if (series.empty()) return;
                if (ImPlot::BeginPlot(label, ImVec2(-1,150), ImPlotFlags_NoTitle)) {
                    ImPlot::SetupAxes("Time (s)", "Value",
                                       ImPlotAxisFlags_NoTickLabels,
                                       ImPlotAxisFlags_AutoFit);
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, w, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);
                    for (auto &s : series) {
                        ImPlot::PlotLine(s.name, s.x.data(), s.y.data(), (int)s.x.size());
                    }
                    ImPlot::EndPlot();
                }
            };
            auto plot_digital_group = [&](const char* label,
                                          std::initializer_list<std::pair<Signal,const char*>> sigs) {
                double latest = forwarder.latest_filtered_time();
                double w = forwarder.window_seconds();
                double t0 = latest - w;
                struct S { std::vector<double> x; std::vector<double> y; const char* name; };
                std::vector<S> series;
                std::vector<Sample> tmp;
                for (auto &p : sigs) {
                    forwarder.snapshot_filtered_with_baseline(p.first, tmp);
                    if (tmp.empty()) continue;
                    S s; s.name = p.second;
                    build_step_series(tmp, t0, w, s.x, s.y);
                    if (!s.x.empty()) series.push_back(std::move(s));
                }
                if (series.empty()) return;
                if (ImPlot::BeginPlot(label, ImVec2(-1,150), ImPlotFlags_NoTitle)) {
                    ImPlot::SetupAxes("Time (s)", "Value",
                                       ImPlotAxisFlags_NoTickLabels,
                                       ImPlotAxisFlags_AutoFit);
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, w, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, -0.05f, 1.05f, ImGuiCond_Always);
                    for (auto &s : series) {
                        ImPlot::PlotLine(s.name, s.x.data(), s.y.data(), (int)s.x.size());
                    }
                    ImPlot::EndPlot();
                }
            };
            plot_analog_group(
                "Left Stick",
                { {Signal::LeftX,"Left Stick X Axis"}, {Signal::LeftY,"Left Stick Y Axis"} },
                -1.05f, 1.05f);
            plot_analog_group(
                "Right Stick",
                { {Signal::RightX,"Right Stick X Axis"}, {Signal::RightY,"Right Stick Y Axis"} },
                -1.05f, 1.05f);
            plot_analog_group(
                "Triggers",
                { {Signal::LeftTrigger,"Left Trigger (LT)"}, {Signal::RightTrigger,"Right Trigger (RT)"} },
                -0.05f, 1.05f);
            plot_digital_group(
                "Shoulder Buttons",
                { {Signal::LeftShoulder,"Left Shoulder (LB)"}, {Signal::RightShoulder,"Right Shoulder (RB)"} });
            plot_digital_group(
                "Face Buttons",
                { {Signal::A,"Face Button A"}, {Signal::B,"Face Button B"}, {Signal::X,"Face Button X"}, {Signal::Y,"Face Button Y"} });
            plot_digital_group(
                "Start/Back",
                { {Signal::StartBtn,"Start / Menu"}, {Signal::BackBtn,"Back / View"} });
            plot_digital_group(
                "Thumbstick Buttons",
                { {Signal::LeftThumbBtn,"Left Thumbstick (L3)"}, {Signal::RightThumbBtn,"Right Thumbstick (R3)"} });
            plot_digital_group(
                "D-Pad",
                { {Signal::DPadUp,"D-Pad Up"}, {Signal::DPadDown,"D-Pad Down"}, {Signal::DPadLeft,"D-Pad Left"}, {Signal::DPadRight,"D-Pad Right"} });
        }
        ImGui::End();

        // Render plots window

        ImGui::Render();
        const float clear_color_with_alpha[4] = {0.05f,0.05f,0.07f,1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // vsync 1 (will block when visible)
    }

    // Shutdown background HOTAS thread and resources
    hotas_bg_enabled.store(false, std::memory_order_release);
    hotas_bg_thread_running.store(false, std::memory_order_release);
    if (hotas_background_thread.joinable()) hotas_background_thread.join();
    hotas.stop_hid_live();
    hotas_mapper.stop();
    
    poller.stop();
    // No auto-save on exit; user must press Save.
    timeEndPeriod(1);

    // Cleanup
    extern void ImGui_ImplWin32_Shutdown();
    extern void ImGui_ImplDX11_Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    if (g_backgroundSRV) { g_backgroundSRV->Release(); g_backgroundSRV = nullptr; }
    CoUninitialize();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}
