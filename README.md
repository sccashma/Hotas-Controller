# Hotas Controller

Real-time filtering and plotting of Xbox (XInput) controller state with a rolling window, designed to investigate and suppress ghost inputs before forwarding to a virtual Xbox 360 device (planned backend integration e.g. ViGEmBus).

> Two code paths still exist (legacy Python prototype + native C++), but the C++/Dear ImGui + ImPlot implementation is the active one and now includes a virtual output toggle (stubbed for future driver integration).

## Features

- Enumerates connected XInput controllers; pick which one to monitor.
- <=8 kHz target polling (best-effort, depends on system load).
- Unified rolling window for all signals.
- Tabs with grouped signals:
  - Sticks: Left (X/Y), Right (X/Y)
  - Triggers / Bumpers: Triggers (analog), Shoulder buttons (digital shown as 0/1)
  - Buttons / D-Pad: A, B, X, Y, Start, Back, Thumb buttons, D-Pad directions
- Lightweight architecture.

## Requirements

- Windows 10/11
- Visual Studio 2022 (MSVC) or Build Tools with CMake 3.25+
- No external dependencies besides those fetched automatically (Dear ImGui docking branch & ImPlot) and the Windows / DirectX 11 SDK (bundled with VS install)

## Build (C++ Version)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWITH_DEMOS=OFF
cmake --build build --config Release --target hotas_controller
```

The resulting binary: `build/Release/hotas_controller.exe`.

Key CMake options:
- `BUILD_STATIC` (ON by default): link static MSVC runtime. If you distribute widely and encounter AV or size concerns, you may set this OFF to use the dynamic runtime.
- `WITH_DEMOS` (ON by default): includes Dear ImGui and ImPlot demo windows. Set OFF for a leaner binary (also slightly reduces heuristic AV triggers).

Security / hardening flags (MSVC): `/sdl /guard:cf /DYNAMICBASE /NXCOMPAT` are applied automatically.

Version metadata is embedded via `res/version.rc`. Adjust values in `res/version.h` before release (company, version, copyright).

## Project Structure (Simplified)
```
src/                     # C++ implementation
  main.cpp               # WinMain + ImGui/ImPlot + docking + UI + virtual output toggle
  core/ring_buffer.hpp   # Lock-light sample ring buffer
  xinput/xinput_poll.*   # Polling thread & stats
  xinput/virtual_output.hpp # Virtual output sink stub
  ui/plots_panel.*       # Plot grouping & rendering + anomaly detection

res/version.{h,rc}       # Embedded version info (Windows resource)
CMakeLists.txt           # Build configuration & FetchContent for ImGui/ImPlot
```

## Updating Version Info
Edit `res/version.h` then rebuild. Increment `APP_VERSION_BUILD` for CI/nightly artifacts.

## License
MIT (add a LICENSE file if distributing).
