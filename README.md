# paws

A from-scratch, zero-dependency upscaling suite built using only standard OS/graphics SDKs (Win32, DirectX 11, DXGI, X11, OpenGL/GLX) and AMD's official FidelityFX FSR SDK.

`paws` ships two components that work together automatically:
1. **Universal FSR 1.0 Spatial Upscaler & Smart Launcher (`paws` / `paws.exe`)** — captures any windowed application and upscales it fullscreen using a 2-pass FSR 1.0 pipeline (**EASU** + **RCAS**).
2. **GTA V Legacy DX11 `.asi` Plugin (`paws_gtav.asi`)** — hooks DirectX 11 via raw vtable patching, captures the 3D color and depth buffers before the 2D HUD draws, injects 16-phase Halton `(2, 3)` sub-pixel jitter, generates `R16G16_FLOAT` camera motion vectors from the depth buffer, and runs FSR 2/3 internally.

---

## Auto-Detection & Routing Flow

When you run `paws.exe <path-to-exe-or-window-title>`:

```
paws.exe <target>
   │
   ├── Is target (or matched process) GTA5.exe? (case-insensitive)
   │      ├── YES (Windows):
   │      │     1. Auto-copies paws_gtav.asi into the GTA5.exe folder (if missing)
   │      │     2. Verifies ASI loader (dinput8.dll) and warns if missing
   │      │     3. Launches GTA5.exe directly (NO FSR 1.0 overlay on top)
   │      │     4. Logs exit status when GTA5.exe closes
   │      │
   │      └── NO (all other programs / windows):
   │            1. Launches executable (if path) or attaches to existing window title
   │            2. Spawns borderless click-through fullscreen FSR 1.0 (EASU + RCAS) overlay
```

---

## ⚠️ GTA V Legacy: Story Mode Only

> **WARNING:** `paws_gtav.asi` is strictly for **GTA V Legacy — Story Mode ONLY**.
> Do **NOT** join **GTA Online** with `dinput8.dll` or any `.asi` plugin installed in your game directory. Anti-cheat (BattlEye) and online integrity checks disallow modified DLLs/ASI plugins. Remove `dinput8.dll` and `paws_gtav.asi` before playing GTA Online.

---

## Building from Source

### Windows (MSVC x64)
Builds both `paws.exe` and `paws_gtav.asi` into the same output directory (`build/bin/Release/`):

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

#### Enabling AMD FidelityFX FSR 2/3 SDK (`HAS_FFX_SDK`)
By default, `paws_gtav.asi` compiles cleanly without external SDK headers (running the DX11 vtable hooks, Halton jitter tracker, Motion Vector compute shader, and Depth/Motion Vector debug views). To enable full AMD FidelityFX FSR 2/3 dispatch:

```powershell
cmake -B build -A x64 -DPAWS_FFX_SDK_DIR="C:\path\to\FidelityFX-SDK"
cmake --build build --config Release
```

### Linux (X11 + OpenGL/GLX)
Install development headers (`libx11-dev libxcomposite-dev libxfixes-dev libgl-dev` on Debian/Ubuntu, or `libx11 libxcomposite libxfixes libglvnd` on Arch Linux):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The binary is output to `build/bin/paws`.

---

## Usage & Hotkeys

### Windows
```powershell
# GTA V Legacy (auto-deploys paws_gtav.asi & launches directly)
.\build\bin\Release\paws.exe "C:\Games\GTAV\GTA5.exe"

# Any other windowed game or application (universal FSR 1.0 overlay)
.\build\bin\Release\paws.exe "C:\Path\To\App.exe"
.\build\bin\Release\paws.exe "Window Title" --sharpness 0.2
```

### Linux
```bash
./build/bin/paws /path/to/app
./build/bin/paws "Window Title" --sharpness 0.2
```

### Hotkeys

| Component | Key | Action |
| :--- | :---: | :--- |
| Universal FSR 1.0 (`paws` / `paws.exe`) | `F8` | Toggle RCAS sharpening ON / OFF |
| Universal FSR 1.0 (`paws` / `paws.exe`) | `END` | Quit overlay |
| GTA V ASI Plugin (`paws_gtav.asi`) | `F8` | Cycle debug view: `Normal` → `Depth Buffer` → `Motion Vectors` |
| GTA V ASI Plugin (`paws_gtav.asi`) | `F9` | Toggle FSR 2/3 ON / OFF |

`paws_gtav.asi` writes detected color/depth buffer formats, hotkey changes, and periodic FPS stats to `paws_gtav.log` inside the GTA V directory.

---

## OBS Studio Capture Tip

- **Universal FSR 1.0 Overlay (Windows):** The fullscreen overlay sets `WDA_EXCLUDEFROMCAPTURE` so DXGI Desktop Duplication does not recursively capture the overlay itself. Because of this flag, OBS **Display Capture** will see the underlying window rather than the upscaled overlay. To record the game cleanly without capturing desktop clutter, use **OBS Game Capture** (or **Window Capture**) targeted directly at the game executable.
- **GTA V (`paws_gtav.asi`):** Because FSR 2/3 runs internally inside GTA V's DirectX 11 swapchain, **OBS Game Capture** captures the upscaled output directly at full performance with zero overlay overhead.
