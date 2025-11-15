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
#pragma comment(lib, "winmm.lib")
#include "xinput/xinput_poll.hpp"
#include "ui/plots_panel.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "xinput/filtered_forwarder.hpp"

struct FilterSettings {
    bool enabled = false;
    float analog_delta = 0.25f;
    float analog_return = 0.15f;
    double digital_max_ms = 5.0; // stored in milliseconds for UI
    bool left_trigger_digital = false; // treat LT as digital for filtering
    bool right_trigger_digital = false; // treat RT as digital for filtering
};

// Global persisted runtime parameters
static double g_target_hz = 1000.0;      // polling frequency
static double g_window_seconds = 30.0;   // plot window length
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
    auto getb=[&](const char* k,bool d)->bool{auto it=kv.find(k); if(it==kv.end()) return d; return it->second=="1"||it->second=="true"||it->second=="TRUE";};
    auto getf=[&](const char* k,float d)->float{auto it=kv.find(k); if(it==kv.end()) return d; try{ return std::stof(it->second);}catch(...){return d;}};
    auto getd=[&](const char* k,double d)->double{auto it=kv.find(k); if(it==kv.end()) return d; try{ return std::stod(it->second);}catch(...){return d;}};
    fs.enabled = getb("enabled", fs.enabled);
    fs.analog_delta = getf("analog_delta", fs.analog_delta);
    fs.analog_return = getf("analog_return", fs.analog_return);
    fs.digital_max_ms = getd("digital_max_ms", fs.digital_max_ms);
    g_target_hz = getd("target_hz", g_target_hz);
    g_window_seconds = getd("window_seconds", g_window_seconds);
    g_virtual_output_enabled = getb("virtual_output", g_virtual_output_enabled);
    fs.left_trigger_digital = getb("left_trigger_digital", fs.left_trigger_digital);
    fs.right_trigger_digital = getb("right_trigger_digital", fs.right_trigger_digital);
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
    out << "target_hz=" << g_target_hz << "\n";
    out << "window_seconds=" << g_window_seconds << "\n";
    out << "virtual_output=" << (g_virtual_output_enabled?1:0) << "\n";
    out << "left_trigger_digital=" << (fs.left_trigger_digital?1:0) << "\n";
    out << "right_trigger_digital=" << (fs.right_trigger_digital?1:0) << "\n";
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ImGui Win32 + DX11 backend forward decls
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

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

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Increase timer resolution for better sub-millisecond sleep precision
    timeBeginPeriod(1);
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

    // Load persisted settings before starting poller (overrides defaults if present)
    FilterSettings filter_settings; LoadFilterSettings("filter_settings.cfg", filter_settings);

    // Clamp loaded values to sane ranges
    if (g_target_hz < 10.0) g_target_hz = 10.0; else if (g_target_hz > 8000.0) g_target_hz = 8000.0;
    if (g_window_seconds < 1.0) g_window_seconds = 1.0; else if (g_window_seconds > 60.0) g_window_seconds = 60.0;

    XInputPoller poller; poller.start(0, g_target_hz, g_window_seconds);
    static FilteredForwarder forwarder;
    poller.set_sink(&forwarder);
    bool virtual_enabled = g_virtual_output_enabled && std::string(forwarder.backend_status()) == "Ready";
    forwarder.enable_output(virtual_enabled);
    forwarder.enable_filter(filter_settings.enabled);
    forwarder.set_params(filter_settings.analog_delta, filter_settings.digital_max_ms/1000.0);
    forwarder.set_trigger_modes(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
    PlotsPanel panel(poller, PlotConfig{g_window_seconds, 4000});
    panel.set_filter_mode(filter_settings.enabled);
    panel.set_filter_thresholds(filter_settings.analog_delta, filter_settings.analog_return, filter_settings.digital_max_ms/1000.0);
    panel.set_trigger_digital(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
    FilterSettings working = filter_settings; // editable working copy
    bool filter_dirty = false;
    // Saved snapshots for runtime parameters to participate in dirty tracking
    double saved_target_hz = g_target_hz;
    double saved_window_seconds = g_window_seconds;

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

            static bool layout_built = false;
            if (!layout_built) {
                layout_built = true;
                ImGui::DockBuilderRemoveNode(dock_id);            // clear any previous (in case of hot reload)
                ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dock_id, viewport->Size);
                ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Left, 0.22f, nullptr, &dock_id); // 22% left panel
                ImGuiID dock_main = dock_id; // remainder
                ImGui::DockBuilderDockWindow("Control", dock_left);
                ImGui::DockBuilderDockWindow("Signals", dock_main);
                ImGui::DockBuilderFinish(dock_id);
            }
            ImGui::End();
        }

        // Control window
        ImGui::Begin("Control");
        auto stats = poller.stats();
        ImGui::Text("Effective Hz: %.1f", stats.effective_hz);
        // Virtual controller output toggle
        if (ImGui::Checkbox("Virtual Output", &virtual_enabled)) {
            // Only persist if backend ready; otherwise revert checkbox
            if (std::string(forwarder.backend_status()) == "Ready") {
                g_virtual_output_enabled = virtual_enabled;
                forwarder.enable_output(virtual_enabled);
            } else {
                virtual_enabled = false;
                forwarder.enable_output(false);
            }
        }
        ImGui::SameLine(); ImGui::TextDisabled(forwarder.backend_status());
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
        static double hz_min=50.0; static double hz_max=8000.0;
        if (ImGui::SliderScalar("Target Hz", ImGuiDataType_Double, &g_target_hz, &hz_min, &hz_max, "%.0f")) {
            poller.set_target_hz(g_target_hz); // live apply; saving handled by combined dirty state
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputDouble("Target Hz Exact", &g_target_hz, 10.0, 100.0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (g_target_hz < hz_min) g_target_hz = hz_min; else if (g_target_hz > hz_max) g_target_hz = hz_max;
            poller.set_target_hz(g_target_hz);
        }
        // Window length controls (1 - 60 seconds)
        double win = panel.window_seconds();
        double win_min = 1.0, win_max = 60.0;
        if (ImGui::SliderScalar("Window (s)", ImGuiDataType_Double, &win, &win_min, &win_max, "%.0f")) {
            poller.set_window_seconds(win);
            panel.set_window_seconds(win);
            g_window_seconds = win; // live apply
        }
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputDouble("Window Exact", &win, 0.1, 1.0, "%.1f")) {
            if (win < 1.0) win = 1.0; else if (win > 60.0) win = 60.0;
            poller.set_window_seconds(win);
            panel.set_window_seconds(win);
            g_window_seconds = win;
        }
        if (ImGui::Button("Clear Plots")) {
            poller.clear();
        }
        // Filter / Anomaly detection controls
        // Filter settings (persistent across runs)
        bool filter_mode = working.enabled;
        float analog_delta = working.analog_delta;
        float analog_return = working.analog_return;
        double digital_max = working.digital_max_ms;
        if (ImGui::CollapsingHeader("Filter Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Checkbox("Enable Filter Mode", &filter_mode)) {
                panel.set_filter_mode(filter_mode);
                working.enabled = filter_mode; filter_dirty = true;
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
                if (ImGui::Checkbox("Left Trigger Digital", &lt_dig)) { working.left_trigger_digital = lt_dig; panel.set_trigger_digital(lt_dig, working.right_trigger_digital); forwarder.set_trigger_modes(lt_dig, working.right_trigger_digital); filter_dirty = true; }
                if (ImGui::Checkbox("Right Trigger Digital", &rt_dig)) { working.right_trigger_digital = rt_dig; panel.set_trigger_digital(working.left_trigger_digital, rt_dig); forwarder.set_trigger_modes(working.left_trigger_digital, rt_dig); filter_dirty = true; }
                if (updated) {
                    panel.set_filter_thresholds(analog_delta, analog_return, digital_max/1000.0); // apply live but not yet saved
                    working.analog_delta = analog_delta;
                    working.analog_return = analog_return;
                    working.digital_max_ms = digital_max;
                    filter_dirty = true;
                    forwarder.set_params(analog_delta, digital_max/1000.0);
                }
                ImGui::TextDisabled("Spikes highlighted in red; short digital pulses flagged.");
                bool runtime_dirty = (g_target_hz != saved_target_hz) || (g_window_seconds != saved_window_seconds);
                bool any_dirty = filter_dirty || runtime_dirty;
                if (any_dirty) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.6f,0,1), "*modified"); }
                // Always show buttons; disable when no pending changes (filter or runtime params)
                ImGui::BeginDisabled(!any_dirty);
                if (ImGui::Button("Save Settings")) {
                    if (any_dirty) {
                        if (filter_dirty) {
                            filter_settings = working;
                            forwarder.set_trigger_modes(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
                            panel.set_trigger_digital(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
                        }
                        // Persist current runtime + filter settings
                        SaveFilterSettings("filter_settings.cfg", filter_settings);
                        // Update snapshots
                        saved_target_hz = g_target_hz;
                        saved_window_seconds = g_window_seconds;
                        filter_dirty = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Revert")) {
                    if (any_dirty) {
                        if (filter_dirty) {
                            working = filter_settings;
                            panel.set_filter_mode(working.enabled);
                            panel.set_filter_thresholds(working.analog_delta, working.analog_return, working.digital_max_ms/1000.0);
                            forwarder.enable_filter(working.enabled);
                            forwarder.set_params(working.analog_delta, working.digital_max_ms/1000.0);
                        }
                        if (runtime_dirty) {
                            g_target_hz = saved_target_hz;
                            poller.set_target_hz(g_target_hz);
                            g_window_seconds = saved_window_seconds;
                            poller.set_window_seconds(g_window_seconds);
                            panel.set_window_seconds(g_window_seconds);
                        }
                        filter_dirty = false;
                    }
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::End();

        // Panels
        ImGui::Begin("Signals");
        panel.draw();
        ImGui::End();

        // Render plots window

        ImGui::Render();
        const float clear_color_with_alpha[4] = {0.05f,0.05f,0.07f,1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // vsync 1 (will block when visible)
    }

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
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}
