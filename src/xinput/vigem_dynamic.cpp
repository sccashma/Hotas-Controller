#include "vigem_dynamic.hpp"
#include <cstdio>

ViGEmDynamic::ViGEmDynamic() {}
ViGEmDynamic::~ViGEmDynamic() { unload(); }

void ViGEmDynamic::unload() {
    // Cannot properly free without function pointers; rely on process teardown if not fully initialized
    if (_lib) {
        if (_target && _client && _vigem_target_remove) {
            _vigem_target_remove(_client, _target);
        }
        if (_target && _vigem_target_free) _vigem_target_free(_target);
        if (_client && _vigem_free) _vigem_free(_client);
        FreeLibrary(_lib);
    }
    _lib=nullptr; _client=nullptr; _target=nullptr; _available=false; _target_added=false; 
    // Preserve specific failure reasons (including "Missing symbol:")
    if (!(_status.rfind("ViGEmClient.dll not found",0)==0 ||
           _status.rfind("Missing symbol",0)==0 ||
           _status=="connect failed" || _status=="target add failed" || _status=="alloc failed" || _status=="target alloc failed" || _status=="target alloc failed")) {
        _status = "Unloaded";
    }
}

void ViGEmDynamic::ensure() {
    if (_available || _lib) return; // already attempted successfully or still loaded
    _attempted = true;
    _lib = LoadLibraryA("ViGEmClient.dll");
    if (!_lib) { _status = "ViGEmClient.dll not found"; return; }
    auto loadSym = [&](auto &fp, const char* base){
        // Try undecorated first
        fp = reinterpret_cast<std::remove_reference_t<decltype(fp)>>(GetProcAddress(_lib, base));
        if (!fp) {
            // Some builds might export with leading underscore (rare on x64) or stdcall decorated names on 32-bit; attempt underscore variant.
            std::string alt = std::string("_") + base;
            fp = reinterpret_cast<std::remove_reference_t<decltype(fp)>>(GetProcAddress(_lib, alt.c_str()));
        }
        return fp!=nullptr;
    };
    const char* missing = nullptr;
    if (!loadSym(_vigem_alloc, "vigem_alloc")) missing = "vigem_alloc";
    else if (!loadSym(_vigem_connect, "vigem_connect")) missing = "vigem_connect";
    else if (!loadSym(_vigem_free, "vigem_free")) missing = "vigem_free";
    else if (!loadSym(_vigem_target_x360_alloc, "vigem_target_x360_alloc")) missing = "vigem_target_x360_alloc";
    else if (!loadSym(_vigem_target_add, "vigem_target_add")) missing = "vigem_target_add";
    else if (!loadSym(_vigem_target_remove, "vigem_target_remove")) missing = "vigem_target_remove";
    else if (!loadSym(_vigem_target_free, "vigem_target_free")) missing = "vigem_target_free";
    else if (!loadSym(_vigem_target_x360_update, "vigem_target_x360_update")) missing = "vigem_target_x360_update";
    if (missing) { _status = std::string("Missing symbol: ") + missing; unload(); return; }
    if (!create_client() || !create_target()) { unload(); return; }
    _available = true; _status = "Ready";
}

void ViGEmDynamic::reload() {
    unload();
    _status = "Reloading";
    _vigem_alloc = nullptr; _vigem_connect=nullptr; _vigem_free=nullptr; _vigem_target_x360_alloc=nullptr; _vigem_target_add=nullptr; _vigem_target_remove=nullptr; _vigem_target_free=nullptr; _vigem_target_x360_update=nullptr;
    _attempted = false;
    ensure();
}

bool ViGEmDynamic::create_client() {
    _client = _vigem_alloc();
    if (!_client) { _status="alloc failed"; return false; }
    int r = _vigem_connect(_client);
    if (r != 0) { _status="connect failed"; return false; }
    return true;
}
bool ViGEmDynamic::create_target() {
    _target = _vigem_target_x360_alloc();
    if (!_target) { _status="target alloc failed"; return false; }
    int r = _vigem_target_add(_client, _target);
    if (r != 0) { _status="target add failed"; return false; }
    _target_added = true;
    return true;
}

void ViGEmDynamic::set_enabled(bool e) {
    ensure();
    if (!_available) { _enabled=false; return; }
    _enabled = e;
}

void ViGEmDynamic::update(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                          uint8_t lt, uint8_t rt, uint16_t buttons) {
    if (!_enabled || !_available || !_target_added) return;
    XUSB_REPORT rep{};
    rep.wButtons = buttons;
    rep.bLeftTrigger = lt;
    rep.bRightTrigger = rt;
    rep.sThumbLX = lx; rep.sThumbLY = ly; rep.sThumbRX = rx; rep.sThumbRY = ry;
    _vigem_target_x360_update(_client, _target, &rep);
}
