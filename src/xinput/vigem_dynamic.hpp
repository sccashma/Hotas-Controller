#pragma once
// Minimal dynamic loader for ViGEmClient.dll (optional at runtime).
// We avoid a hard build dependency; if the DLL is present the virtual device
// will be created, otherwise we operate in a no-op mode and report status.

#include <windows.h>
#include <cstdint>
#include <string>

class ViGEmDynamic {
public:
    ViGEmDynamic();
    ~ViGEmDynamic();
    bool available() const { return _available; }
    const char* status() const { return _status.c_str(); }
    // Attempt lazy init (safe to call repeatedly)
    void ensure();
    // Update virtual X360 controller (only if available & enabled)
    void update(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                uint8_t lt, uint8_t rt, uint16_t buttons);
    void set_enabled(bool e);
    bool enabled() const { return _enabled; }
    void reload(); // force re-attempt even after previous failure/unload
    bool had_attempt() const { return _attempted; }
private:
    void unload();
    bool create_client();
    bool create_target();

    HMODULE _lib = nullptr;
    bool _available = false;
    bool _enabled = false;
    bool _target_added = false;
    std::string _status = "Not initialized";
    bool _attempted = false;

    // Opaque pointers from ViGEm
    void* _client = nullptr;
    void* _target = nullptr;

    // Function pointer typedefs (subset)
    using PFN_vigem_alloc = void* (*)();
    using PFN_vigem_connect = int (*)(void*);
    using PFN_vigem_free = void (*)(void*);
    using PFN_vigem_target_x360_alloc = void* (*)();
    using PFN_vigem_target_add = int (*)(void*, void*);
    using PFN_vigem_target_remove = int (*)(void*, void*);
    using PFN_vigem_target_free = void (*)(void*);
    using PFN_vigem_target_x360_update = int (*)(void*, void*, const struct XUSB_REPORT*);

    PFN_vigem_alloc _vigem_alloc = nullptr;
    PFN_vigem_connect _vigem_connect = nullptr;
    PFN_vigem_free _vigem_free = nullptr;
    PFN_vigem_target_x360_alloc _vigem_target_x360_alloc = nullptr;
    PFN_vigem_target_add _vigem_target_add = nullptr;
    PFN_vigem_target_remove _vigem_target_remove = nullptr;
    PFN_vigem_target_free _vigem_target_free = nullptr;
    PFN_vigem_target_x360_update _vigem_target_x360_update = nullptr;
};

// Minimal reproduction of XUSB_REPORT used by ViGEm (avoid header dependency)
struct XUSB_REPORT {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};
