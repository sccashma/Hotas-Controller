# X56 HOTAS Controller

Map your Logitech X56 HOTAS to game inputs with clean, reliable behavior.

This app reads your HOTAS at a fixed 1,000 samples per second, filters out ghost inputs, and lets you map any HOTAS button/axis to:
- Virtual Xbox 360 controller (ViGEm)
- Keyboard keys
- Mouse actions

You get live graphs for raw and filtered signals, so you can see exactly what the device is doing and what the filter is producing.

## What You Need
- Windows 10 or 11
- Logitech X56 HOTAS plugged in
- Optional (for virtual controller): ViGEmBus driver

## Install & Run
Option A — Download (recommended for end users):
- Grab the latest prebuilt `hotas_controller.exe` from the Releases page and run it.
- The app ships with its required libraries; no separate dependency install.

Option B — Build from source (advanced users):
```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target hotas_controller
./build/Release/hotas_controller.exe
```
- The build scripts fetch and build dependencies automatically (ImGui, ImPlot, nlohmann/json, etc.).
- ViGEm client is integrated; if the ViGEmBus driver is present, the Virtual Output will show Ready. If not, virtual output is disabled gracefully.

## Quick Start
- Plug in the HOTAS and launch the app.
- In Control:
	- Set Window (seconds) for how much history to show.
	- Turn on Filter Mode and pick parameters:
		- Analog Rate Limit: 0–100% per sample (smooths axes)
		- Digital Pulse Max (ms): minimum press length to count (debounce)
	- Choose per‑input filter mode (None/Digital/Analog) for each HOTAS signal.
- Open Mappings:
	- Pick a HOTAS signal (Stick/Throttle, including HAT directions and POV).
	- Choose Action Type: x360, keyboard, or mouse.
	- For axes, set a small Deadband (e.g., 0.05) and Priority (higher wins).
	- Click Add Mapping, then Save Profile to persist.
- Optional: enable Virtual Output to publish a virtual Xbox 360 pad via ViGEm.

## How Filtering Works
- Digital (buttons, toggles): requires a press to last at least `Digital Pulse Max` before it “promotes” and appears; shorter pulses are ignored.
- Analog (axes): limits the maximum change per sample by a percentage of the input’s full range.
	- Axes normalized to −1..1 use a 2.0 full range.
	- Raw analog inputs use their bit‑range (e.g., 0..65535).
- Triggers can be treated as digital or analog separately.

## Keyboard & Mouse Mapping
- Keyboard events use scan codes (not just virtual keys) so browsers/games receive a proper `code` like KeyV.
- Use simple names (e.g., `VK_SPACE`, `A`) in the Mappings UI.
- Mouse actions include clicks and movement.

## Settings & Profiles
- Click Save Settings to write filter/runtime options to `config/filter_settings.cfg`.
- Click Save Profile in Mappings to write HOTAS→action pairs to `config/mappings.json`.
- No auto‑save on exit (you control when to save).

## Tips
- If Virtual Output is disabled, install ViGEmBus; the client library is built along with the app.
- Use the per‑signal filter table to apply Digital to noisy buttons and Analog to jittery axes.
- Test keyboard mappings at the W3C Key Event Viewer (the `code` field should be set).

## Known Limits
- Polling is fixed at 1 kHz and independent of UI frame rate.
- History is stored in fixed‑size rings to keep memory bounded.
- Filter modes are mutually exclusive per signal.
- UI stays in three panes (Control | Raw | Filtered).

## License
MIT — see [LICENSE](LICENSE).
