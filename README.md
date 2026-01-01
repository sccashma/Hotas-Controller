# Hotas Controller

High‑frequency polling, visualization, and filtering of Xbox (XInput) controller signals. The goal is to detect/suppress ghost inputs (short unintended digital pulses & large analog spikes) and optionally forward a cleaned state to a virtual Xbox 360 device via ViGEm.

The active implementation is native C++ using Dear ImGui + ImPlot. A legacy Python prototype exists only for historical reference.

## Quick Highlights
* Adjustable polling rate (clamped 10–8000 Hz) with live effective Hz and loop cost stats.
* Rolling window plots (configurable seconds) for every signal.
* Per‑signal enable/disable of filtering (spike & short‑pulse suppression).
* Gated digital filtering: presses must reach a minimum duration before promotion.
* Analog spike suppression using absolute delta thresholds.
* Optional virtual Xbox 360 output when ViGEm client & driver are available.
* Persistent settings (`config/filter_settings.cfg`) for filter + runtime parameters.
* Automatic three‑column dock layout (Control | Raw Signals | Filtered Signals).

## Signal Names
Enumeration in `src/xinput/xinput_poll.hpp` (used in config keys):
```
left_x, left_y, right_x, right_y,
left_trigger, right_trigger,
left_shoulder, right_shoulder,
a, b, x, y, start, back, left_thumb, right_thumb,
dpad_up, dpad_down, dpad_left, dpad_right
```

## Filtering Overview
Digital inputs follow a pending→promoted state machine (see `filtered_forwarder.hpp`). A rising edge starts a timer; only after the press lasts ≥ `digital_max_ms` is it promoted (visible). Releases prior to promotion are fully suppressed.

Analog inputs apply spike suppression per axis: if `|current - previous| >= analog_delta` and filtering for that signal is enabled, the value is clamped to the previous sample. Triggers can be toggled to digital mode individually (UI checkboxes) to join the gated path.

## Requirements
* Windows 10/11
* Visual Studio 2022 (MSVC) or Build Tools + CMake ≥ 3.25
* DirectX 11 & XInput libraries (provided by VS install)
* Optional: ViGEmBus driver + client (submodule) for virtual output

## Build
Default CMake options (see [CMakeLists.txt](CMakeLists.txt)):
* `BUILD_STATIC` = OFF (set ON to link static MSVC runtime)
* `WITH_DEMOS`  = OFF (set ON to include ImGui/ImPlot demo windows)

Release build:
```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target hotas_controller
```

Binary: `build/Release/hotas_controller.exe`
Runtime config directory: `build/Release/config/`
- Contains `filter_settings.cfg`, `mappings.json`, and `X56_Hotas_hid_bit_map.csv` copied from [res/config](res/config).
- The application loads and saves these files from `config/` next to the executable.

MSVC security / hardening flags applied: `/sdl`, `/guard:cf`, `/DYNAMICBASE`, `/NXCOMPAT`.

### Submodules
Initialize ViGEm client if using virtual output:
```powershell
git submodule update --init --recursive
```

### Configuration File
`config/filter_settings.cfg` (created/saved by explicit **Save Settings** action) contains:
```
enabled=0|1
analog_delta=<float>
analog_return=<float>
digital_max_ms=<double>
target_hz=<double>
window_seconds=<double>
virtual_output=0|1
left_trigger_digital=0|1
right_trigger_digital=0|1
filter_<signal_name>=0|1   (per signal)
```
No automatic save on exit; use **Save Settings** to persist changes.

## UI Panels
* **Control**: stats, controller index selection, Hz & window adjustments, filter toggles, per‑signal overrides, save/revert, virtual output toggle (enabled only when backend is `Ready`).
* **Raw Signals**: direct polled values.
* **Filtered Signals**: post‑filter state (gating + spike suppression).

## Key Source Files
* `src/main.cpp` – Win32 & DX11 init, ImGui/ImPlot setup, docking layout, settings I/O.
* `src/xinput/xinput_poll.*` – polling loop, sample capture, stats.
* `src/xinput/filtered_forwarder.hpp` – filtering logic + ViGEm forwarding.
* `src/ui/plots_panel.*` – plot generation & step series creation.
* `src/core/ring_buffer.hpp` – storage for high‑rate sample history.
* `external/ViGEmClient/` – ViGEm client library (submodule).
* CSV descriptor source: [res/config/X56_Hotas_hid_bit_map.csv](res/config/X56_Hotas_hid_bit_map.csv) – loaded at runtime; becomes [build/Release/config/X56_Hotas_hid_bit_map.csv](build/Release/config/X56_Hotas_hid_bit_map.csv).

## Performance Notes
* Very high target rates (> 4 kHz) may be CPU intensive; the loop uses sleep + short spin to hit deadlines.
* Each signal stores samples in a ring sized at `1<<19` entries to accommodate large windows at high rates.

## Troubleshooting
| Issue | Suggestions |
|-------|------------|
| Virtual output toggle disabled | ViGEmBus driver or client missing; ensure submodule built and driver installed; status should be `Ready`. |
| High CPU usage | Lower `target_hz`; disable demos; reduce window length. |

## Versioning
`res/version.h` + `res/version.rc` embed version and manifest (if present) in the binary. Edit before releases.

## License
MIT – see `LICENSE`.

## Disclaimer
High polling rates and spin waits increase power usage. Choose rates appropriate for diagnostics, not continuous background use.

## Mappings (HOTAS → Action)

Configure how HOTAS signals map to output actions via the UI or by editing the JSON profile.

Quick steps (UI)
- Open Edit → Mappings in the app (see UI in [`src/main.cpp`](src/main.cpp)).
- Device: choose All / Stick / Throttle.
- Signal: populated from the runtime HOTAS signal list (`[`HotasReader::list_signals`](src/xinput/hotas_reader.cpp)` / [`src/xinput/hotas_reader.hpp`](src/xinput/hotas_reader.hpp)`).
- Action Type: select one of:
  - x360 — maps to a virtual Xbox 360 input (labels defined in [`src/main.cpp`](src/main.cpp))
  - keyboard — sends key (e.g. "VK_SPACE" or 'A'); use the new visual keyboard picker ("Pick...") next to the field to choose keys without typing codes
  - mouse — sends mouse actions (e.g. left_click)
- Click "Add Mapping" to create the mapping; use Save/Load to persist via the UI (calls [`HotasMapper::save_profile`](src/xinput/hotas_mapper.cpp) / [`HotasMapper::load_profile`](src/xinput/hotas_mapper.cpp)).

Mapping internals
- A mapping is represented by [`MappingEntry`](src/xinput/hotas_mapper.hpp) and stored/loaded as JSON in `config/mappings.json` (default in [res/config/mappings.json](res/config/mappings.json)).
- New logical samples are fed into the mapper via [`HotasMapper::accept_sample`](src/xinput/hotas_mapper.cpp) (called from the HOTAS raw parser / UI code).
- The mapper publishes mapped X360 reports at its publish rate and uses ViGEm to emit a virtual controller (see [`src/xinput/hotas_mapper.cpp`](src/xinput/hotas_mapper.cpp) and the ViGEm client submodule at [`external/ViGEmClient`](external/ViGEmClient)).

Injection flow (high level)
- HOTAS raw reports → parsed by [`HotasReader::get_hid_live_snapshot` / `list_signals`](src/xinput/hotas_reader.cpp) using descriptors from `config/X56_Hotas_hid_bit_map.csv` → logical values passed to [`HotasMapper::accept_sample`](src/xinput/hotas_mapper.cpp) → mapper publishes to virtual device (ViGEm) in [`HotasMapper::publisher_thread_main`](src/xinput/hotas_mapper.cpp).
- The UI wires mapper injection into the app poller via [`HotasMapper::set_inject_callback`](src/xinput/hotas_mapper.cpp) / the call in [`src/main.cpp`](src/main.cpp) which injects mapped states into the `XInputPoller` for filtering/forwarding.

File references
- UI wiring / Mappings dialog: [`src/main.cpp`](src/main.cpp)
- Mapper interface & MappingEntry: [`src/xinput/hotas_mapper.hpp`](src/xinput/hotas_mapper.hpp)
- Mapper implementation (save/load/publish): [`src/xinput/hotas_mapper.cpp`](src/xinput/hotas_mapper.cpp)
- HOTAS signal discovery & HID live: [`src/xinput/hotas_reader.hpp`](src/xinput/hotas_reader.hpp) / [`src/xinput/hotas_reader.cpp`](src/xinput/hotas_reader.cpp)
- Example mapping profile: [`mappings.json`](mappings.json)
- Virtual output (ViGEm) client code: [`external/ViGEmClient`](external/ViGEmClient)

Notes & tips
- Use the device filter to reduce the signal list to Stick or Throttle when authoring mappings.
- The keyboard picker mirrors a standard US layout and fills the field with supported names (VK_* constants or single characters). Keys auto‑repeat while held based on system keyboard settings.
- After editing `mappings.json` manually, use Load in the Mappings UI to apply.
- If ViGEm is not Ready the x360 action targets will not be emitted; check ViGEm status in the Control panel (see [`src/xinput/hotas_mapper.cpp`](src/xinput/hotas_mapper.cpp) and [`external/ViGEmClient`](external/ViGEmClient)).
