// Docking enabled globally via CMake compile definitions.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <implot.h>
#include <imgui_internal.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <lunasvg.h>
#include <dxgi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1svg.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Windowscodecs.lib")
#include "xinput/xinput_poll.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "xinput/filtered_forwarder.hpp"
#include "xinput/hotas_reader.hpp"
#include "xinput/hotas_mapper.hpp"
// Plots for XInput signals (sticks, triggers, buttons)
#include "ui/plots_panel.hpp"

// Shared HID buffers for raw Stick/Throttle plotting
struct HidBuf { std::vector<double> t; std::vector<double> v; };
static std::unordered_map<std::string, HidBuf> g_hid_buffers;
// Filtered HID buffers (post per-signal filtering)
static std::unordered_map<std::string, HidBuf> g_hid_filtered_buffers;

// Helper to build canonical device-prefixed keys to avoid stick/throttle collisions
static inline const char* device_prefix(HotasReader::SignalDescriptor::DeviceKind dk) {
    return (dk == HotasReader::SignalDescriptor::DeviceKind::Stick) ? "stick" : "throttle";
}

// Common raw HID plotter with slight Y padding and fixed ticks for standard ranges
static void PlotHidGroup(const char* title,
                         const std::unordered_map<std::string, HidBuf>& buffers,
                         const std::vector<std::pair<const char*, const char*>>& series,
                         double window,
                         double t0,
                         float y_min,
                         float y_max) {
    struct S { std::vector<double> x; std::vector<double> y; const char* name; };
    std::vector<S> all;
    for (auto &p : series) {
        auto it = buffers.find(p.first);
        if (it == buffers.end()) continue;
        const HidBuf &buf = it->second;
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
        double y_lo = (double)y_min, y_hi = (double)y_max;
        bool zero_one = (y_min == 0.0f && y_max == 1.0f);
        bool neg1_pos1 = (y_min == -1.0f && y_max == 1.0f);
        if (zero_one) { y_lo = -0.05; y_hi = 1.05; }
        else if (neg1_pos1) { y_lo = -1.05; y_hi = 1.05; }
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_lo, y_hi, ImGuiCond_Always);
        if (zero_one) {
            static double ticks[] = {0.0, 0.5, 1.0};
            static const char* labels[] = {"0", "0.5", "1.0"};
            ImPlot::SetupAxisTicks(ImAxis_Y1, ticks, 3, labels);
        } else if (neg1_pos1) {
            static double ticks[] = {-1.0, 0.0, 1.0};
            static const char* labels[] = {"-1", "0", "1"};
            ImPlot::SetupAxisTicks(ImAxis_Y1, ticks, 3, labels);
        }
        for (auto &s : all) {
            ImPlot::PlotLine(s.name, s.x.data(), s.y.data(), (int)s.x.size());
        }
        ImPlot::EndPlot();
    }
}

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

// Virtual Output monitor globals
static bool g_show_virtual_output_window = false;
static XInputPoller g_output_poller; // polls XInput state of the (virtual) controller
static PlotConfig g_output_plot_cfg{}; // default config; window_seconds set at runtime
static PlotsPanel g_output_plots(g_output_poller, g_output_plot_cfg);
static bool g_output_started = false;
static int g_output_controller_idx = 0; // index to poll (choose the ViGEm virtual pad)

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

static void SaveHotasFilterModes(const char* path,
                                 const std::vector<HotasReader::SignalDescriptor>& sigs,
                                 const std::unordered_map<std::string,int>& hotas_modes) {
    // Append/update per-signal modes for HOTAS signals at the end of the cfg.
    // Simple approach: append lines; loader uses last occurrence effectively.
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out) return;
    for (const auto& sd : sigs) {
        int mode = 0;
        const char* devp = (sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) ? "stick" : "throttle";
        std::string map_key = std::string(devp) + ":" + sd.id;
        auto it = hotas_modes.find(map_key);
        if (it != hotas_modes.end()) mode = it->second;
        // Write device-prefixed key to disambiguate duplicates (legacy reader falls back if this is absent)
        out << "filter_" << devp << "_" << sd.name << "=";
        switch (mode) {
            case 1: out << "digital\n"; break;
            case 2: out << "analog\n"; break;
            default: out << "none\n"; break;
        }
    }
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")

// ImGui Win32 + DX11 backend forward decls
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static ID3D11ShaderResourceView* g_backgroundSRV = nullptr;
static int g_bg_width = 0, g_bg_height = 0;
static ID3D11ShaderResourceView* g_keyboardSRV = nullptr;
static int g_kb_width = 0, g_kb_height = 0;

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

    // Create texture with auto-generated mipmaps for crisp downscaling
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = w; texDesc.Height = h; texDesc.MipLevels = 0; texDesc.ArraySize = 1; // 0 = full mip chain
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&texDesc, nullptr, &tex);
    if (FAILED(hr) || !tex) return false;
    // Upload top mip
    context->UpdateSubresource(tex, 0, nullptr, pixels.data(), w * 4, 0);
    // Create SRV covering all mips
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = texDesc.Format; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0; srvDesc.Texture2D.MipLevels = (UINT)-1; // all mips
    hr = device->CreateShaderResourceView(tex, &srvDesc, out_srv);
    if (FAILED(hr) || !*out_srv) { tex->Release(); return false; }
    // Generate mipmaps
    context->GenerateMips(*out_srv);
    tex->Release();
    if (out_width) *out_width = (int)w; if (out_height) *out_height = (int)h;
    return true;
}

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out; out.resize((size_t)len - 1);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Load an SVG via Lunasvg and create a mipmapped D3D11 texture (BGRA format)
static bool LoadTextureSVG_Luna(const wchar_t* filename, ID3D11Device* device, ID3D11DeviceContext* context,
                                ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height,
                                int target_px_width, int target_px_height) {
    if (!device || !context || !filename || !out_srv) return false;
    *out_srv = nullptr;
    std::string path = WideToUtf8(filename);
    auto doc = lunasvg::Document::loadFromFile(path);
    if (!doc) return false;
    // Compute output size: prefer explicit target width/height if provided
    int out_w = target_px_width;
    int out_h = target_px_height;
    if (out_w <= 0 || out_h <= 0) {
        // Fallback: derive aspect ratio from intrinsic size or bounding box
        float dw = doc->width();
        float dh = doc->height();
        if (dw <= 0.f || dh <= 0.f) {
            auto box = doc->boundingBox();
            dw = box.w;
            dh = box.h;
        }
        double aspect = (dh > 0.0f) ? (static_cast<double>(dw) / static_cast<double>(dh)) : 1.0;
        out_h = (out_h > 0) ? out_h : 64;
        out_w = (out_w > 0) ? out_w : std::max(1, (int)std::floor(out_h * aspect + 0.5));
    }
    auto bmp = doc->renderToBitmap(out_w, out_h);
    if (bmp.isNull()) return false;
    // Convert to RGBA plain for DXGI_FORMAT_R8G8B8A8_UNORM
    bmp.convertToRGBA();
    const unsigned char* data = bmp.data();

    // Create texture with auto-generated mipmaps for crisp downscaling (BGRA)
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = out_w; texDesc.Height = out_h; texDesc.MipLevels = 0; texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &tex);
    if (FAILED(hr) || !tex) return false;
    context->UpdateSubresource(tex, 0, nullptr, data, bmp.stride(), 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = texDesc.Format; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0; srvDesc.Texture2D.MipLevels = (UINT)-1;
    hr = device->CreateShaderResourceView(tex, &srvDesc, out_srv);
    if (FAILED(hr) || !*out_srv) { tex->Release(); return false; }
    context->GenerateMips(*out_srv);
    tex->Release();
    if (out_width) *out_width = out_w; if (out_height) *out_height = out_h;
    return true;
}
// Rasterize an SVG into a texture using Direct2D + WIC, for crisp scaling
// Removed unused legacy SVG rasterizer stub (replaced by LunaSVG path)

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

    // Load keyboard icon strictly from SVG (no PNG/text fallbacks)
    const wchar_t* kb_svg_candidates[] = {
        L"graphics\\keyboard.svg",
        L"..\\graphics\\keyboard.svg",
        L"..\\..\\graphics\\keyboard.svg",
        L"res\\graphics\\keyboard.svg",
        L"..\\res\\graphics\\keyboard.svg",
        L"..\\..\\res\\graphics\\keyboard.svg"
    };
    for (auto* path : kb_svg_candidates) {
        // Render SVG at fixed 64x36 pixels for crisp display
        if (LoadTextureSVG_Luna(path, g_pd3dDevice, g_pd3dDeviceContext, &g_keyboardSRV, &g_kb_width, &g_kb_height, 64, 36)) break;
    }

    // Load persisted settings before starting poller (overrides defaults if present)
    FilterSettings filter_settings; LoadFilterSettings("config/filter_settings.cfg", filter_settings);

    // Clamp loaded values to sane ranges
    if (g_window_seconds < 1.0) g_window_seconds = 1.0; else if (g_window_seconds > 60.0) g_window_seconds = 60.0;

    // Note: Polling rate is fixed at 1000 Hz (not configurable per spec)
    double fixed_polling_hz = 1000.0;
    XInputPoller poller; poller.start(0, fixed_polling_hz, g_window_seconds);
    HotasReader hotas;
    HotasMapper hotas_mapper;
    // Build HOTAS per-signal filter mode map from config (device-scoped keys)
    // Key format: "stick:<id>" or "throttle:<id>" to disambiguate duplicates (e.g., E/F/G)
    std::unordered_map<std::string,int> hotas_filter_modes; // 0=none,1=digital,2=analog
    {
        std::ifstream in("config/filter_settings.cfg", std::ios::in);
        if (in) {
            std::unordered_map<std::string,std::string> kv;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                auto pos = line.find('='); if (pos==std::string::npos) continue;
                kv[line.substr(0,pos)] = line.substr(pos+1);
            }
            auto sigs = hotas.list_signals();
            for (const auto &sd : sigs) {
                const char* devp = (sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) ? "stick" : "throttle";
                std::string dev_key = std::string("filter_") + devp + "_" + sd.name; // new device-scoped key
                std::string legacy_key = std::string("filter_") + sd.name;             // legacy key without device prefix
                // Prefer device-scoped key; fall back to legacy
                std::string key = kv.count(dev_key) ? dev_key : legacy_key;
                auto it = kv.find(key);
                int mode = 0;
                if (it != kv.end()) {
                    const std::string &v = it->second;
                    if (v == "digital") mode = 1; else if (v == "analog") mode = 2; else mode = 0;
                }
                std::string map_key = std::string(devp) + ":" + sd.id;
                hotas_filter_modes[map_key] = mode; // track by device+id
            }
        }
    }
    // Load persisted HOTAS mappings at startup
    hotas_mapper.load_profile("config/mappings.json");
    // Migrate legacy mappings (no device prefix) to device-prefixed IDs
    {
        auto entries = hotas_mapper.list_mapping_entries();
        if (!entries.empty()) {
            // Build id -> device map; mark ambiguous ids
            struct DevInfo { bool seen = false; HotasReader::SignalDescriptor::DeviceKind dk{}; bool ambiguous = false; };
            std::unordered_map<std::string, DevInfo> id_map;
            for (const auto &sd : hotas.list_signals()) {
                auto &di = id_map[sd.id];
                if (!di.seen) { di.seen = true; di.dk = sd.device; }
                else { di.ambiguous = true; }
            }
            bool changed = false;
            for (const auto &me : entries) {
                if (me.signal_id.find(':') == std::string::npos) {
                    auto it = id_map.find(me.signal_id);
                    if (it != id_map.end() && it->second.seen && !it->second.ambiguous) {
                        const char* devp = (it->second.dk == HotasReader::SignalDescriptor::DeviceKind::Stick) ? "stick" : "throttle";
                        std::string new_sig = std::string(devp) + ":" + me.signal_id;
                        // Replace mapping entry with updated signal_id
                        hotas_mapper.remove_mapping(me.id);
                        MappingEntry updated = me; updated.signal_id = new_sig;
                        hotas_mapper.add_mapping(updated);
                        changed = true;
                    }
                }
            }
            if (changed) {
                // Persist normalized mappings
                hotas_mapper.save_profile("config/mappings.json");
            }
        }
    }
    // Inject mapped controller states back into the poller for plotting/filtering
    hotas_mapper.set_inject_callback([&](double t, const XInputPoller::ControllerState& cs){
        poller.inject_state(t, cs);
    });
    static bool show_hotas_detect_window = false;
    static std::vector<std::string> hotas_detect_lines;
    static bool show_developer_view = false;
    // HOTAS is always enabled; no UI toggle
    static bool show_mappings_window = false;
    static FilteredForwarder forwarder;
    poller.set_sink(&forwarder);
    bool virtual_enabled = g_virtual_output_enabled; // start from persisted setting
    // Keep forwarder output disabled; HotasMapper will drive ViGEm output based on mappings
    forwarder.enable_output(false);
    // Start mapper if virtual output persisted enabled
    if (virtual_enabled) {
        hotas_mapper.start(1000.0);
    }
    forwarder.enable_filter(filter_settings.enabled);
    forwarder.set_params(filter_settings.analog_delta, filter_settings.digital_max_ms/1000.0);
    forwarder.set_trigger_modes(filter_settings.left_trigger_digital, filter_settings.right_trigger_digital);
    forwarder.set_filter_modes(filter_settings.per_signal_mode);
    forwarder.set_window_seconds(g_window_seconds);
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
        // Per-signal filter state (id -> prev/current)
        std::unordered_map<std::string,double> prev_vals;
        // Track previous RAW values separately for digital gating state machine
        std::unordered_map<std::string,double> prev_raw_vals;
        std::unordered_map<std::string,double> rise_times;
        // For multi-bit digital signals (e.g., hats), track pending target value
        std::unordered_map<std::string,double> pending_vals;
        std::unordered_map<std::string,bool> active_flags;
        while (hotas_bg_thread_running.load()) {
            // HOTAS input always enabled
            if (hotas_bg_enabled.load()) {
                // Advance HOTAS timebase to keep raw HID plots rolling
                (void)hotas.poll_once();
                auto now_tp = clock::now();
                // Connection-based liveness: prefer handle visibility over report freshness.
                bool connected = hotas.has_stick() || hotas.has_throttle();
                // Pull HID live snapshot and parse per-signal values
                auto live = hotas.get_hid_live_snapshot();
                // Convert hex to bytes per device
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
                std::vector<uint8_t> stick_bytes;
                std::vector<uint8_t> throttle_bytes;
                bool have_stick_report = false;
                bool have_throttle_report = false;
                for (auto &p : live) {
                    const std::string &path = p.first;
                    const std::string &hex = p.second;
                    if (hex.empty() || hex == "(no data yet)") continue;
                    if (path.find("vid_0738&pid_2221") != std::string::npos && path.find("mi_00") != std::string::npos) {
                        hex_to_bytes(hex, stick_bytes);
                        have_stick_report = !stick_bytes.empty();
                    } else if (path.find("vid_0738&pid_a221") != std::string::npos && path.find("mi_00") != std::string::npos) {
                        hex_to_bytes(hex, throttle_bytes);
                        have_throttle_report = !throttle_bytes.empty();
                    }
                }
                if (have_stick_report || have_throttle_report) {
                    last_ok_tp = now_tp;
                    hotas_detected.store(true, std::memory_order_release);
                    // Auto-start mapper on first detection if not already running
                    static bool mapper_started_auto = false;
                    if (virtual_enabled && !mapper_started_auto) {
                        hotas_mapper.start(1000.0);
                        mapper_started_auto = true;
                    }
                    double now = std::chrono::duration<double>(now_tp.time_since_epoch()).count();
                    // Build per-signal values using CSV descriptors
                    auto sigs = hotas.list_signals();
                    auto extract_bits = [&](const std::vector<uint8_t>& bytes, int bit_start, int bits)->uint64_t {
                        if (bits <= 0) return 0;
                        uint64_t val = 0;
                        for (int i = 0; i < bits; ++i) {
                            int bit_global = bit_start + i;
                            size_t byte_idx = (size_t)bit_global / 8;
                            int bit_in_byte = bit_global % 8; // LSB-first
                            int bitv = 0;
                            if (byte_idx < bytes.size()) bitv = (bytes[byte_idx] >> bit_in_byte) & 1;
                            val |= (uint64_t)bitv << i;
                        }
                        return val;
                    };
                    for (const auto &sd : sigs) {
                        const std::vector<uint8_t>& bytes = (sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) ? stick_bytes : throttle_bytes;
                        if (bytes.empty()) continue;
                        uint64_t raw = extract_bits(bytes, sd.bit_start, sd.bits);
                        double v = 0.0;
                        // Normalize common analog types
                        if (sd.id == "joy_x" || sd.id == "joy_y" || sd.id == "joy_z") {
                            double maxv = (double)((1ULL << sd.bits) - 1);
                            v = (maxv > 0.0) ? (double)raw / maxv * 2.0 - 1.0 : 0.0;
                        } else if (sd.id == "c_joy_x" || sd.id == "c_joy_y" || sd.id == "thumb_joy_x" || sd.id == "thumb_joy_y") {
                            v = ((double)raw / 255.0) * 2.0 - 1.0;
                        } else if (sd.id == "left_throttle" || sd.id == "right_throttle") {
                            v = (double)raw / (double)((1ULL << sd.bits) - 1);
                        } else if (sd.analog) {
                            v = (double)raw; // other analogs raw 0..(2^bits-1)
                        } else {
                            v = (double)raw; // digital/multi-bit raw value
                        }
                        // Apply per-signal filtering prior to mapping
                        int mode = 0;
                        std::string map_key = std::string(device_prefix(sd.device)) + ":" + sd.id;
                        auto fm_it = hotas_filter_modes.find(map_key);
                        if (fm_it != hotas_filter_modes.end()) mode = fm_it->second;
                        double out_v = v;
                        if (mode == 2) {
                            // Analog spike suppression
                            double prev = prev_vals.count(map_key) ? prev_vals[map_key] : v;
                            double dv = fabs(out_v - prev);
                            if (dv >= working.analog_delta) out_v = prev;
                        } else if (mode == 1) {
                            // Digital debounce/gating
                            double &rise = rise_times[map_key];
                            if (!sd.analog && sd.bits > 1) {
                                // Multi-bit digital (e.g., hats): gate discrete value changes
                                double prev_filtered = prev_vals.count(map_key) ? prev_vals[map_key] : v;
                                double prev_raw = prev_raw_vals.count(map_key) ? prev_raw_vals[map_key] : v;
                                double &pend = pending_vals[map_key];
                                if (!prev_raw_vals.count(map_key)) {
                                    rise = -1.0; pend = v; out_v = v;
                                } else {
                                    if (v != prev_raw) {
                                        // Value changed; start/refresh hold timer and keep previous filtered value
                                        rise = now; pend = v; out_v = prev_filtered;
                                    } else {
                                        // Stable; promote after threshold when pending matches and differs from filtered
                                        if (rise >= 0.0 && (now - rise) >= (working.digital_max_ms/1000.0) && pend == v && v != prev_filtered) {
                                            out_v = v; rise = -1.0;
                                        } else {
                                            out_v = prev_filtered;
                                        }
                                    }
                                }
                            } else {
                                // Binary digital: interpret non-analog values >0 as active
                                bool now_hi = sd.analog ? (v >= 0.5) : (v > 0.0);
                                double prev_raw = prev_raw_vals.count(map_key) ? prev_raw_vals[map_key] : v;
                                bool prev_hi = sd.analog ? (prev_raw >= 0.5) : (prev_raw > 0.0);
                                if (!prev_raw_vals.count(map_key)) rise = -1.0;
                                if (now_hi && !prev_hi) {
                                    rise = now; active_flags[map_key] = false;
                                } else if (now_hi && prev_hi) {
                                    if (!active_flags[map_key] && rise >= 0.0) {
                                        double dur = now - rise;
                                        if (dur >= (working.digital_max_ms/1000.0)) active_flags[map_key] = true;
                                    }
                                } else if (!now_hi && prev_hi) {
                                    active_flags[map_key] = false; rise = -1.0;
                                } else {
                                    rise = -1.0; active_flags[map_key] = false;
                                }
                                out_v = active_flags[map_key] ? 1.0 : 0.0;
                            }
                        }
                        // Store previous values: filtered for analog spikes, RAW for digital gating
                        prev_vals[map_key] = out_v;
                        prev_raw_vals[map_key] = v;
                        hotas_mapper.accept_sample(map_key, out_v, now);
                        // Store filtered value for UI plots
                        HidBuf &fb = g_hid_filtered_buffers[std::string(device_prefix(sd.device)) + ":" + sd.name];
                        fb.t.push_back(now);
                        fb.v.push_back(out_v);
                        // Trim to window
                        double window = g_window_seconds;
                        double t0 = now - window;
                        size_t first_keep = 0;
                        while (first_keep < fb.t.size() && fb.t[first_keep] < t0) ++first_keep;
                        if (first_keep > 0) {
                            fb.t.erase(fb.t.begin(), fb.t.begin() + first_keep);
                            fb.v.erase(fb.v.begin(), fb.v.begin() + first_keep);
                        }
                    }
                } else {
                    // If no valid HOTAS data is arriving, only re-enumerate when devices appear disconnected.
                    if (!connected && (now_tp - last_ok_tp > std::chrono::seconds(1)) && now_tp >= next_refresh_tp) {
                        hotas.stop_hid_live();
                        hotas.start_hid_live();
                        next_refresh_tp = now_tp + std::chrono::seconds(2); // cooldown to avoid busy re-enumeration
                        hotas_detected.store(false, std::memory_order_release);
                    } else if (connected) {
                        // Maintain detection flag when devices are present even if reports are momentarily idle.
                        hotas_detected.store(true, std::memory_order_release);
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
                        // Toggle: Virtual Output Monitor (X360)
                        bool open_vom = g_show_virtual_output_window;
                        if (ImGui::MenuItem("Virtual Output Monitor", nullptr, open_vom)) {
                            g_show_virtual_output_window = !g_show_virtual_output_window;
                            if (g_show_virtual_output_window && !g_output_started) {
                                // Start dedicated XInput poller at fixed 1 kHz
                                g_output_poller.start(g_output_controller_idx, 1000.0, g_window_seconds);
                                g_output_started = true;
                            } else if (!g_show_virtual_output_window && g_output_started) {
                                g_output_poller.stop();
                                g_output_started = false;
                            }
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
                // Create a three-column layout: left (Control), middle (Stick/Throttle), right (Filtered Signals)
                ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Left, 0.22f, nullptr, &dock_id); // 22% left panel
                ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Right, 0.33f, nullptr, &dock_id); // 33% right panel
                ImGuiID dock_main = dock_id; // middle area (remaining)
                ImGui::DockBuilderDockWindow("Control", dock_left);
                ImGui::DockBuilderDockWindow("Stick", dock_main);
                ImGui::DockBuilderDockWindow("Throttle", dock_main);
                ImGui::DockBuilderDockWindow("Filtered Signals", dock_right);
                ImGui::DockBuilderDockWindow("Mappings", dock_right);
                // Dock Virtual Output monitor into right column
                ImGui::DockBuilderDockWindow("Virtual Output (X360)", dock_right);
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
        // Virtual controller output toggle (driven by HotasMapper)
        if (ImGui::Checkbox("Virtual Output", &virtual_enabled)) {
            if (virtual_enabled) {
                hotas_mapper.start(1000.0);
                g_virtual_output_enabled = true;
            } else {
                hotas_mapper.stop();
                g_virtual_output_enabled = false;
            }
        }
        ImGui::SameLine(); ImGui::TextDisabled("Mapper");
        ImGui::Text("Backend: %s | Output: %s", "ViGEm (HotasMapper)", virtual_enabled ? "On" : "Off");
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

        if (stats.avg_loop_us > 0.0) {
            ImGui::Text("Avg loop: %.2f us", stats.avg_loop_us);
        }
        ImGui::TextDisabled("Polling rate: 1000 Hz (fixed)");
        // Connection status (independent of report liveness)
        {
            bool stick_conn = hotas.has_stick();
            bool throttle_conn = hotas.has_throttle();
            ImGui::Text("HOTAS Stick: %s", stick_conn ? "Connected" : "Not Connected");
            ImGui::Text("HOTAS Throttle: %s", throttle_conn ? "Connected" : "Not Connected");
        }
        // Window length controls (1 - 60 seconds)
        double win = g_window_seconds;
        double win_min = 1.0, win_max = 60.0;
        if (ImGui::SliderScalar("Window (s)", ImGuiDataType_Double, &win, &win_min, &win_max, "%.0f")) {
            poller.set_window_seconds(win);
            forwarder.set_window_seconds(win);
            g_window_seconds = win;
            // Keep Virtual Output monitor in sync
            g_output_poller.set_window_seconds(win);
            g_output_plots.set_window_seconds(win);
        }
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputDouble("Window Exact", &win, 0.1, 1.0, "%.1f")) {
            if (win < 1.0) win = 1.0; else if (win > 60.0) win = 60.0;
            poller.set_window_seconds(win);
            forwarder.set_window_seconds(win);
            g_window_seconds = win;
            g_output_poller.set_window_seconds(win);
            g_output_plots.set_window_seconds(win);
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
                    forwarder.set_trigger_modes(lt_dig, working.right_trigger_digital);
                    filter_dirty = true;
                }
                if (ImGui::Checkbox("Right Trigger Digital", &rt_dig)) {
                    working.right_trigger_digital = rt_dig;
                    forwarder.set_trigger_modes(working.left_trigger_digital, rt_dig);
                    filter_dirty = true;
                }
                if (updated) {
                    working.analog_delta = analog_delta;
                    working.analog_return = analog_return;
                    working.digital_max_ms = digital_max;
                    filter_dirty = true;
                    forwarder.set_params(analog_delta, digital_max/1000.0);
                }
                
                ImGui::SeparatorText("HOTAS Per-Input Filter Modes");
                ImGui::TextDisabled("Select per-signal mode: None (raw), Digital (debounce), Analog (spike suppression).");
                const char* items[] = { "None", "Digital", "Analog" };
                if (ImGui::BeginTable("hotas_filter_modes", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("Signal");
                    ImGui::TableSetupColumn("Mode");
                    auto sigs = hotas.list_signals();
                    for (const auto &sd : sigs) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const char* dev = (sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) ? "Stick" : "Throttle";
                        std::string disp = std::string(dev) + ": " + sd.name;
                        ImGui::TextUnformatted(disp.c_str());
                        ImGui::TableSetColumnIndex(1);
                        std::string map_key = std::string((sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) ? "stick" : "throttle") + ":" + sd.id;
                        int mode = 0; auto it = hotas_filter_modes.find(map_key); if (it != hotas_filter_modes.end()) mode = it->second;
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((std::string("##hotas_mode_") + map_key).c_str(), &mode, items, IM_ARRAYSIZE(items))) {
                            hotas_filter_modes[map_key] = mode;
                            filter_dirty = true;
                        }
                    }
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
                        }
                        // Persist current runtime + filter settings and HOTAS per-signal modes
                        SaveFilterSettings("config/filter_settings.cfg", filter_settings);
                        SaveHotasFilterModes("config/filter_settings.cfg", hotas.list_signals(), hotas_filter_modes);
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
                            // Apply persisted settings to forwarder
                            forwarder.enable_filter(working.enabled);
                            forwarder.set_params(working.analog_delta, working.digital_max_ms/1000.0);
                        }
                        if (runtime_dirty) {
                            g_window_seconds = saved_window_seconds;
                            poller.set_window_seconds(g_window_seconds);
                            forwarder.set_window_seconds(g_window_seconds);
                        }
                        filter_dirty = false;
                    }
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::End();

        // Virtual Output monitor pane
        if (g_show_virtual_output_window) {
            ImGui::Begin("Virtual Output (X360)", nullptr, ImGuiWindowFlags_NoBackground);
            {
                // Controller selection for the emulated device (index 0-3)
                g_output_controller_idx = g_output_poller.controller_index();
                ImGui::SetNextItemWidth(120);
                if (ImGui::SliderInt("Controller Index (Output)", &g_output_controller_idx, 0, 3)) {
                    g_output_poller.set_controller_index(g_output_controller_idx);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Auto Detect")) {
                    int found = -1;
                    for (int i = 0; i < 4; ++i) {
                        XINPUT_STATE st{}; if (XInputGetState(i, &st) == ERROR_SUCCESS) { found = i; break; }
                    }
                    if (found >= 0) { g_output_poller.set_controller_index(found); }
                }
                // Stats display (effective Hz, avg loop time)
                auto out_stats = g_output_poller.stats();
                ImGui::Text("Effective Hz: %.1f", out_stats.effective_hz);
                if (out_stats.avg_loop_us > 0.0) ImGui::Text("Avg loop: %.2f us", out_stats.avg_loop_us);
                ImGui::Text("XInput Connected: %s", g_output_poller.connected() ? "Yes" : "No");

                // Configure plots: fixed window, anomaly highlighting off for output
                g_output_plots.set_window_seconds(g_window_seconds);
                g_output_plots.set_filter_mode(false);
                g_output_plots.draw();
            }
            ImGui::End();
        }

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
                hotas_mapper.save_profile("config/mappings.json");
            }
            ImGui::SetItemTooltip("Save all mappings to 'mappings.json' in the application directory for persistence across runs.");
            ImGui::SameLine();
            if (ImGui::Button("Load...")) {
                hotas_mapper.load_profile("config/mappings.json");
            }
            ImGui::SetItemTooltip("Load mappings from 'mappings.json', replacing the current mapping list.");
            ImGui::Separator();

            // Add mapping form
            static char new_id[64] = "m1";
            static int device_sel = 0; // 0=All,1=Stick,2=Throttle
            const char* device_names[] = { "All", "Stick", "Throttle" };
            ImGui::Combo("Device", &device_sel, device_names, IM_ARRAYSIZE(device_names));
            ImGui::SetItemTooltip("Filter HOTAS signals by device: All (all signals), Stick (joystick inputs), or Throttle (throttle/quadrant inputs)");

            // Build signal list from HotasReader signals filtered by DeviceKind
            auto sigs = hotas.list_signals();
            struct SigChoice { std::string id; std::string display; };
            std::vector<SigChoice> sig_choices; sig_choices.reserve(sigs.size());
            for (const auto &sd : sigs) {
                bool include = (device_sel == 0) ||
                               (device_sel == 1 && sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) ||
                               (device_sel == 2 && sd.device == HotasReader::SignalDescriptor::DeviceKind::Throttle);
                if (!include) continue;
                SigChoice ch;
                ch.id = std::string(device_prefix(sd.device)) + ":" + sd.id; // always device-prefixed for unambiguous mapping
                ch.display = sd.name + std::string(" (") + sd.id + ")";
                sig_choices.push_back(std::move(ch));
            }
            if (sig_choices.empty()) {
                for (const auto &sd : sigs) {
                    SigChoice ch; ch.id = std::string(device_prefix(sd.device)) + ":" + sd.id; ch.display = sd.name + std::string(" (") + sd.id + ")";
                    sig_choices.push_back(std::move(ch));
                }
            }

            static int sig_sel = 0;
            if (sig_sel >= (int)sig_choices.size()) sig_sel = 0;
            std::vector<const char*> sig_items; sig_items.reserve(sig_choices.size());
            for (auto &ch : sig_choices) sig_items.push_back(ch.display.c_str());

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
            if (action_type_sel == 0) {
                ImGui::Combo("X360 Input", &x360_sel, x360_labels, IM_ARRAYSIZE(x360_labels));
                ImGui::SetItemTooltip("Select the target Xbox 360 input: axes (sticks, triggers) or buttons (A/B/X/Y, shoulders, DPad, thumb presses, etc.)");
            } else if (action_type_sel == 1) {
                // Place the picker button before the text field so it stays visible in narrow panes
                {
                    if (g_keyboardSRV) {
                        // Display at the exact loaded size for pixel-perfect rendering
                        ImVec2 sz((float)g_kb_width, (float)g_kb_height);
                        bool clicked = ImGui::ImageButton((void*)g_keyboardSRV, sz);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick from visual keyboard");
                        if (clicked) ImGui::OpenPopup("Select Keyboard Key");
                    } else {
                        // No fallback: keep layout spacing without a functional control
                        ImGui::Dummy(ImVec2(24, 24));
                    }
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputText("Keyboard (e.g. VK_SPACE or 'A')", keyboard_action, sizeof(keyboard_action));
                ImGui::SetItemTooltip("Enter a keyboard key code (VK_* constant) or pick from the visual keyboard.");
                if (ImGui::BeginPopupModal("Select Keyboard Key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    // Simple US keyboard layout for selection
                    struct KeySpec { const char* label; const char* code; float w; };
                    auto key_button = [&](const KeySpec &k){
                        ImGui::PushID(k.code);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6,6));
                        ImGui::PushItemWidth(k.w);
                        bool clicked = ImGui::Button(k.label, ImVec2(k.w, 32));
                        ImGui::PopItemWidth();
                        ImGui::PopStyleVar();
                        ImGui::PopID();
                        if (clicked) {
                            strncpy(keyboard_action, k.code, sizeof(keyboard_action)-1);
                            keyboard_action[sizeof(keyboard_action)-1] = '\0';
                            ImGui::CloseCurrentPopup();
                        }
                    };
                    // Function row
                    KeySpec frow[] = {
                        {"Esc","VK_ESCAPE",50}, {"F1","VK_F1",40},{"F2","VK_F2",40},{"F3","VK_F3",40},{"F4","VK_F4",40},
                        {"F5","VK_F5",40},{"F6","VK_F6",40},{"F7","VK_F7",40},{"F8","VK_F8",40},
                        {"F9","VK_F9",40},{"F10","VK_F10",40},{"F11","VK_F11",40},{"F12","VK_F12",40}
                    };
                    for (int i=0;i<IM_ARRAYSIZE(frow);++i){ key_button(frow[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    // Number row
                    KeySpec numrow[] = {
                        {"`","VK_OEM_3",40},{"1","1",40},{"2","2",40},{"3","3",40},{"4","4",40},{"5","5",40},{"6","6",40},{"7","7",40},{"8","8",40},{"9","9",40},{"0","0",40},{"-","VK_OEM_MINUS",40},{"=","VK_OEM_PLUS",40},{"Back","VK_BACK",80}
                    };
                    for (int i=0;i<IM_ARRAYSIZE(numrow);++i){ key_button(numrow[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    // Q row
                    KeySpec qrow[] = {
                        {"Tab","VK_TAB",70},{"Q","Q",40},{"W","W",40},{"E","E",40},{"R","R",40},{"T","T",40},{"Y","Y",40},{"U","U",40},{"I","I",40},{"O","O",40},{"P","P",40},{"[","VK_OEM_4",40},{"]","VK_OEM_6",40},{"\\","VK_OEM_5",70}
                    };
                    for (int i=0;i<IM_ARRAYSIZE(qrow);++i){ key_button(qrow[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    // A row
                    KeySpec arow[] = {
                        {"Caps","VK_CAPITAL",80},{"A","A",40},{"S","S",40},{"D","D",40},{"F","F",40},{"G","G",40},{"H","H",40},{"J","J",40},{"K","K",40},{"L","L",40},{";","VK_OEM_1",40},{"'","VK_OEM_7",40},{"Enter","VK_RETURN",100}
                    };
                    for (int i=0;i<IM_ARRAYSIZE(arow);++i){ key_button(arow[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    // Z row
                    KeySpec zrow[] = {
                        {"Shift","VK_SHIFT",90},{"Z","Z",40},{"X","X",40},{"C","C",40},{"V","V",40},{"B","B",40},{"N","N",40},{"M","M",40},{",","VK_OEM_COMMA",40},{".","VK_OEM_PERIOD",40},{"/","VK_OEM_2",40},{"Shift","VK_RSHIFT",90}
                    };
                    for (int i=0;i<IM_ARRAYSIZE(zrow);++i){ key_button(zrow[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    // Bottom row
                    KeySpec brow[] = {
                        {"Ctrl","VK_CONTROL",70},{"Win","VK_LWIN",60},{"Alt","VK_MENU",60},{"Space","VK_SPACE",300},{"Alt","VK_RMENU",60},{"Win","VK_RWIN",60},{"Menu","VK_APPS",60},{"Ctrl","VK_RCONTROL",70}
                    };
                    for (int i=0;i<IM_ARRAYSIZE(brow);++i){ key_button(brow[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    // Arrow keys
                    KeySpec arrows[] = {{"Up","VK_UP",50},{"Left","VK_LEFT",50},{"Down","VK_DOWN",50},{"Right","VK_RIGHT",50}};
                    for (int i=0;i<IM_ARRAYSIZE(arrows);++i){ key_button(arrows[i]); ImGui::SameLine(); }
                    ImGui::NewLine();
                    ImGui::Separator();
                    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            } else {
                ImGui::InputText("Mouse Action (e.g. left_click)", mouse_action, sizeof(mouse_action));
                ImGui::SetItemTooltip("Enter a mouse action: left_click, right_click, move_up, move_down, scroll_up, scroll_down, etc.");
            }
            if (ImGui::Button("Add Mapping")) {
                MappingEntry e;
                e.id = std::string(new_id);
                // selected signal id
                if (!sig_choices.empty()) e.signal_id = sig_choices[sig_sel].id; else e.signal_id = std::string("");
                if (action_type_sel == 0) {
                    e.action = std::string(x360_actions[x360_sel]);
                } else if (action_type_sel == 1) {
                    e.action = std::string("keyboard:") + keyboard_action;
                } else {
                    e.action = std::string("mouse:") + mouse_action;
                }
                if (!hotas_mapper.add_mapping(e)) {
                    ImGui::TextColored(ImVec4(1,0.2f,0.2f,1), "Add failed: id exists");
                }
            }
            ImGui::SetItemTooltip("Create a new mapping from the HOTAS signal to the selected action. The Mapping ID must be unique.");
            ImGui::Separator();

            // Show table of mappings
            auto entries = hotas_mapper.list_mapping_entries();
            if (ImGui::BeginTable("mappings_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("Signal");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < entries.size(); ++i) {
                    const auto &me = entries[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(me.id.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(me.signal_id.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(me.action.c_str());
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
        // Stick window: parse HID live hex and show raw integer graphs for stick inputs
        ImGui::Begin("Stick", nullptr, ImGuiWindowFlags_NoBackground);
        // Build maps dynamically from HotasReader::list_signals() (CSV-driven)
        struct HidInputMap { std::string id; std::string name; int bit_start; int bits; bool analog; };
        std::vector<HidInputMap> stick_map;
        std::vector<HidInputMap> throttle_map;
        {
            auto sigs = hotas.list_signals();
            for (const auto &sd : sigs) {
                HidInputMap m{ sd.id, sd.name, sd.bit_start, sd.bits, sd.analog };
                if (sd.device == HotasReader::SignalDescriptor::DeviceKind::Stick) stick_map.push_back(m);
                else throttle_map.push_back(m);
            }
        }

        // Per-signal sample buffers (shared)

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

        // Acquire latest HID live snapshot and extract stick and throttle reports separately
        auto hid_snap = hotas.get_hid_live_snapshot();
        std::vector<uint8_t> stick_bytes;
        std::vector<uint8_t> throttle_bytes;
        bool have_stick_report = false;
        bool have_throttle_report = false;
        for (auto &p : hid_snap) {
            const std::string &path = p.first;
            const std::string &hex = p.second;
            if (hex.empty() || hex == "(no data yet)") continue;
            if (path.find("vid_0738&pid_2221") != std::string::npos && path.find("mi_00") != std::string::npos) {
                hex_to_bytes(hex, stick_bytes);
                have_stick_report = !stick_bytes.empty();
            } else if (path.find("vid_0738&pid_a221") != std::string::npos && path.find("mi_00") != std::string::npos) {
                hex_to_bytes(hex, throttle_bytes);
                have_throttle_report = !throttle_bytes.empty();
            }
        }

        double now_ts = hotas.latest_time();
        if (!have_stick_report && !have_throttle_report) {
            ImGui::TextDisabled("No HID stick/throttle reports available yet.");
        } else {
            double window = g_window_seconds;
            double t0 = now_ts - window;
            auto extract_and_store = [&](const std::vector<HidInputMap>& maps, const std::vector<uint8_t>& bytes, const char* devpref) {
                if (bytes.empty()) return;
                for (auto &m : maps) {
                    uint64_t val = 0;
                    int last_bit = m.bit_start + m.bits - 1;
                    size_t needed_bytes = (last_bit / 8) + 1;
                    if (bytes.size() < needed_bytes) continue;
                    // Extract bits LSB-first into val
                    for (int i = 0; i < m.bits; ++i) {
                        int bit_global = m.bit_start + i;
                        size_t byte_idx = bit_global / 8;
                        int bit_in_byte = bit_global % 8; // LSB-first
                        int bitv = (bytes[byte_idx] >> bit_in_byte) & 1;
                        val |= (uint64_t(bitv) << i);
                    }
                    double plotted = 0.0;
                    double y_min = 0.0, y_max = 1.0;
                    std::string mid(m.id);
                    // Normalize specific analogs
                    if (mid == "joy_x" || mid == "joy_y" || mid == "joy_z") {
                        // 12-16 bit unsig -> -1..1
                        double maxv = (double)((1ULL << m.bits) - 1);
                        plotted = (maxv > 0.0) ? (double)val / maxv * 2.0 - 1.0 : 0.0;
                        y_min = -1.0; y_max = 1.0;
                    } else if (mid == "c_joy_x" || mid == "c_joy_y" || mid == "thumb_joy_x" || mid == "thumb_joy_y") {
                        // 8-bit joystick-like -> -1..1
                        plotted = ((double)val / 255.0) * 2.0 - 1.0;
                        y_min = -1.0; y_max = 1.0;
                    } else if (mid == "left_throttle" || mid == "right_throttle") {
                        // 10-bit throttle -> 0..1
                        plotted = (double)val / (double)((1ULL << m.bits) - 1);
                        y_min = 0.0; y_max = 1.0;
                    } else if (m.analog) {
                        // Other analogs: plot raw 0..(2^bits-1)
                        y_min = 0.0; y_max = (double)((1ULL << m.bits) - 1);
                        plotted = (double)val;
                    } else {
                        // digital/multi: raw value
                        y_min = 0.0; y_max = (double)((1ULL << m.bits) - 1);
                        plotted = (double)val;
                    }
                    HidBuf &b = g_hid_buffers[std::string(devpref) + ":" + m.name];
                    b.t.push_back(now_ts);
                    b.v.push_back(plotted);
                    size_t first_keep = 0;
                    while (first_keep < b.t.size() && b.t[first_keep] < t0) ++first_keep;
                    if (first_keep > 0) {
                        b.t.erase(b.t.begin(), b.t.begin() + first_keep);
                        b.v.erase(b.v.begin(), b.v.begin() + first_keep);
                    }
                }
            };
            if (have_stick_report) extract_and_store(stick_map, stick_bytes, "stick");
            if (have_throttle_report) extract_and_store(throttle_map, throttle_bytes, "throttle");

            // Grouped plots per request (using common PlotHidGroup helper)

            // Joy Stick: JOY_X, JOY_Y, JOY_Z (normalized -1..1)
            PlotHidGroup("Joy Stick", g_hid_buffers, { {"stick:JOY_X","x"}, {"stick:JOY_Y","y"}, {"stick:JOY_Z","z"} }, window, t0, -1.0f, 1.0f);
            // C-Joy: C_JOY_X, C_JOY_Y (normalized -1..1)
            PlotHidGroup("C-Joy", g_hid_buffers, { {"stick:C_JOY_X","x"}, {"stick:C_JOY_Y","y"} }, window, t0, -1.0f, 1.0f);
            // Triggers: TRIGGER, E (0..1)
            PlotHidGroup("Triggers", g_hid_buffers, { {"stick:TRIGGER","Trigger"}, {"stick:E","pinky trigger"} }, window, t0, 0.0f, 1.0f);
            // Buttons: A, B, C, D (0..1)
            PlotHidGroup("Buttons", g_hid_buffers, { {"stick:A","A"}, {"stick:B","B"}, {"stick:C","C"}, {"stick:D","D"} }, window, t0, 0.0f, 1.0f);
            // POV/Hats on stick: 0..15
            PlotHidGroup("POV", g_hid_buffers, { {"stick:POV","POV"} }, window, t0, 0.0f, 15.0f);
            PlotHidGroup("H1", g_hid_buffers, { {"stick:H1","H1"} }, window, t0, 0.0f, 15.0f);
            PlotHidGroup("H2", g_hid_buffers, { {"stick:H2","H2"} }, window, t0, 0.0f, 15.0f);
        }
        ImGui::End();

        // Throttle window: visualize throttle inputs using previously extracted buffers
        ImGui::Begin("Throttle", nullptr, ImGuiWindowFlags_NoBackground);
        {
            // Throttle Quadrant: LEFT/RIGHT_THROTTLE (0..1)
            {
                double window = g_window_seconds;
                double latest = hotas.latest_time();
                double t0 = latest - window;
                PlotHidGroup("Throttle", g_hid_buffers, { {"throttle:LEFT_THROTTLE","Left"}, {"throttle:RIGHT_THROTTLE","Right"} }, window, t0, 0.0f, 1.0f);
                // Throttle Thumb Joystick: THUMB_JOY_X/Y (-1..1)
                PlotHidGroup("Thumb Joystick", g_hid_buffers, { {"throttle:THUMB_JOY_X","x"}, {"throttle:THUMB_JOY_Y","y"} }, window, t0, -1.0f, 1.0f);
                // Throttle Wheels/RTY (0..255)
                PlotHidGroup("Wheels", g_hid_buffers, { {"throttle:F_WHEEL","F"}, {"throttle:G_WHEEL","G"} }, window, t0, 0.0f, 255.0f);
                PlotHidGroup("Rotaries", g_hid_buffers, { {"throttle:RTY3","RTY3"}, {"throttle:RTY4","RTY4"} }, window, t0, 0.0f, 255.0f);
                // Throttle Buttons (0..1) – general buttons (excluding TGL, SW, M1/M2/S1)
                PlotHidGroup("Throttle Buttons", g_hid_buffers, {
                    {"throttle:THUMB_JOY_PRESS","Thumb Press"}, {"throttle:E","E"}, {"throttle:F","F"}, {"throttle:G","G"}, {"throttle:H","H"}, {"throttle:I","I"},
                    {"throttle:K1_UP","K1 Up"}, {"throttle:K1_DOWN","K1 Down"}, {"throttle:SLIDE","Slide"}
                }, window, t0, 0.0f, 1.0f);
                // Toggle switches (TGL1..TGL4 up/down)
                PlotHidGroup("Toggles", g_hid_buffers, {
                    {"throttle:TGL1_UP","TGL1 Up"}, {"throttle:TGL1_DOWN","TGL1 Down"},
                    {"throttle:TGL2_UP","TGL2 Up"}, {"throttle:TGL2_DOWN","TGL2 Down"},
                    {"throttle:TGL3_UP","TGL3 Up"}, {"throttle:TGL3_DOWN","TGL3 Down"},
                    {"throttle:TGL4_UP","TGL4 Up"}, {"throttle:TGL4_DOWN","TGL4 Down"}
                }, window, t0, 0.0f, 1.0f);
                // SW bank (SW1..SW6)
                PlotHidGroup("Switches", g_hid_buffers, {
                    {"throttle:SW1","SW1"}, {"throttle:SW2","SW2"}, {"throttle:SW3","SW3"}, {"throttle:SW4","SW4"}, {"throttle:SW5","SW5"}, {"throttle:SW6","SW6"}
                }, window, t0, 0.0f, 1.0f);
                // Mode buttons (M1/M2/S1)
                PlotHidGroup("Mode Buttons", g_hid_buffers, {
                    {"throttle:M1","M1"}, {"throttle:M2","M2"}, {"throttle:S1","S1"}
                }, window, t0, 0.0f, 1.0f);
                // Throttle Hats H3/H4 (0..15)
                PlotHidGroup("H3/H4", g_hid_buffers, { {"throttle:H3","H3"}, {"throttle:H4","H4"} }, window, t0, 0.0f, 15.0f);
            }
            // (throttle plots rendered above via PlotHidGroup)
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
            double window = g_window_seconds;
            double latest = hotas.latest_time();
            double t0 = latest - window;
            // Reuse the same groupings as raw Stick/Throttle using filtered buffers
            PlotHidGroup("Joy Stick (filtered)", g_hid_filtered_buffers, { {"stick:JOY_X","x"}, {"stick:JOY_Y","y"}, {"stick:JOY_Z","z"} }, window, t0, -1.0f, 1.0f);
            PlotHidGroup("C-Joy (filtered)", g_hid_filtered_buffers, { {"stick:C_JOY_X","x"}, {"stick:C_JOY_Y","y"} }, window, t0, -1.0f, 1.0f);
            PlotHidGroup("Triggers (filtered)", g_hid_filtered_buffers, { {"stick:TRIGGER","Trigger"}, {"stick:E","pinky trigger"} }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("Buttons (filtered)", g_hid_filtered_buffers, { {"stick:A","A"}, {"stick:B","B"}, {"stick:C","C"}, {"stick:D","D"} }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("POV (filtered)", g_hid_filtered_buffers, { {"stick:POV","POV"} }, window, t0, 0.0f, 15.0f);
            PlotHidGroup("H1 (filtered)", g_hid_filtered_buffers, { {"stick:H1","H1"} }, window, t0, 0.0f, 15.0f);
            PlotHidGroup("H2 (filtered)", g_hid_filtered_buffers, { {"stick:H2","H2"} }, window, t0, 0.0f, 15.0f);
            PlotHidGroup("Throttle (filtered)", g_hid_filtered_buffers, { {"throttle:LEFT_THROTTLE","Left"}, {"throttle:RIGHT_THROTTLE","Right"} }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("Thumb Joystick (filtered)", g_hid_filtered_buffers, { {"throttle:THUMB_JOY_X","x"}, {"throttle:THUMB_JOY_Y","y"} }, window, t0, -1.0f, 1.0f);
            PlotHidGroup("Wheels (filtered)", g_hid_filtered_buffers, { {"throttle:F_WHEEL","F"}, {"throttle:G_WHEEL","G"} }, window, t0, 0.0f, 255.0f);
            PlotHidGroup("Rotaries (filtered)", g_hid_filtered_buffers, { {"throttle:RTY3","RTY3"}, {"throttle:RTY4","RTY4"} }, window, t0, 0.0f, 255.0f);
            PlotHidGroup("Throttle Buttons (filtered)", g_hid_filtered_buffers, {
                {"throttle:THUMB_JOY_PRESS","Thumb Press"}, {"throttle:E","E"}, {"throttle:F","F"}, {"throttle:G","G"}, {"throttle:H","H"}, {"throttle:I","I"},
                {"throttle:K1_UP","K1 Up"}, {"throttle:K1_DOWN","K1 Down"}, {"throttle:SLIDE","Slide"}
            }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("Toggles (filtered)", g_hid_filtered_buffers, {
                {"throttle:TGL1_UP","TGL1 Up"}, {"throttle:TGL1_DOWN","TGL1 Down"},
                {"throttle:TGL2_UP","TGL2 Up"}, {"throttle:TGL2_DOWN","TGL2 Down"},
                {"throttle:TGL3_UP","TGL3 Up"}, {"throttle:TGL3_DOWN","TGL3 Down"},
                {"throttle:TGL4_UP","TGL4 Up"}, {"throttle:TGL4_DOWN","TGL4 Down"}
            }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("Switches (filtered)", g_hid_filtered_buffers, {
                {"throttle:SW1","SW1"}, {"throttle:SW2","SW2"}, {"throttle:SW3","SW3"}, {"throttle:SW4","SW4"}, {"throttle:SW5","SW5"}, {"throttle:SW6","SW6"}
            }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("Mode Buttons (filtered)", g_hid_filtered_buffers, { {"throttle:M1","M1"}, {"throttle:M2","M2"}, {"throttle:S1","S1"} }, window, t0, 0.0f, 1.0f);
            PlotHidGroup("H3/H4 (filtered)", g_hid_filtered_buffers, { {"throttle:H3","H3"}, {"throttle:H4","H4"} }, window, t0, 0.0f, 15.0f);
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
    if (g_keyboardSRV) { g_keyboardSRV->Release(); g_keyboardSRV = nullptr; }
    CoUninitialize();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}
