# hotrainy

[中文说明](README.md) | **English**

A lightweight **C++ / Win32 + GDI+** toolkit for Windows 11 that provides:

- **Real taskbar transparency** — injects into `explorer.exe` and hides the XAML `BackgroundFill`
  of the taskbar, so the wallpaper really shows through (not a translucent colour block).
- **Island launcher panel** — an **H** button on the taskbar opens a phone-style panel with an
  embedded search box, categories, large icons, long-press drag sorting and corner resizing.
- **Top edge tag bar** — move the mouse to the top edge to reveal tabs of desktop shortcuts, with
  hover/click expansion modes, per-tab wallpapers and built-in search.
- **Audio spectrum bar** — a bar that reacts to system loudness, with adjustable size, four growth
  directions, drag-to-move and persisted position.

Portable by design: **no admin rights, no MSVC runtime (static CRT), no registry footprint**

> **No network, no telemetry** - there is no networking API in the source at all; see [Privacy & security](#privacy--security).
(the only registry write happens if *you* enable "start with Windows").

![Settings](screenshots/设置界面预览.png)

## Requirements

- **Windows 11 only — Windows 10 is *not* supported** (taskbar transparency and the island button
  both rely on the Win11 XAML taskbar)
- Visual Studio 2022 (Build Tools are enough) + Windows SDK — only if you build from source

## Download & install

1. Download `hotrainy_setup.zip` from [Releases](../../releases) and unzip it
2. Run `hotrainy_installer.exe`, choose an install folder, confirm
   (Desktop and Start Menu shortcuts are created for you)
3. After it starts, right-click the tray icon → **Settings** and enable "Transparency", "Island",
   "Audio bar" as you like

To uninstall, run `uninstall.cmd` in the install folder. It stops the app, removes the startup entry
and shortcuts, **restarts Explorer** (required to release the injected DLLs), deletes program files,
config, logs and wallpapers, verifies leftovers and finally deletes itself.

## Usage (short)

| Module | How |
|---|---|
| Taskbar transparency | Settings → Taskbar → Transparency |
| Island panel | Settings → Taskbar → Island, then click the **H** button |
| Tag bar | Move the mouse to the top edge of the screen; right-click a tab to manage it |
| Audio bar | Settings → System → Audio bar; drag the bar itself to move it |
| Search | Default hotkey `Ctrl+Alt+Space` |

Full details (in Chinese) live in [README.md](README.md) and [`docs/`](docs/).

## Build from source

```bash
git clone https://github.com/HotRain927/hotrainy.git
cd hotrainy
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build -j4
```

Artifacts: `build/hotrainy.exe`, `build/GlassTAP.dll`, `build/GlassBoot.dll`.

> Note: do a full rebuild (`ninja -t clean`, then build) after changing a header — stale object
> layouts have caused real crashes in this project.

To produce the single-file installer: run `pack_installer.cmd` (see `tools/installer/`).

## Repository layout

```
src/ include/          main program (C++), see README.md for a per-file description
src/tb/xaml/           the injected side (GlassTAP / GlassBoot): visual tree access, channel
third_party/tbg/       hand-written mini JSON (vendored, the only external dependency)
tools/installer/       single-file installer stub source
docs/                  implementation notes and release notes (Chinese, with details)
screenshots/           screenshots
```

## How it works (brief)

The Win11 taskbar is XAML: its background is a `Rectangle` named **`BackgroundFill`** in the visual
tree under `Shell_TrayWnd`. hotrainy injects a DLL into `explorer.exe`
(`SetWindowsHookEx`), grabs the XAML diagnostics interface inside Explorer
(`InitializeXamlDiagnosticsEx`), walks the tree and makes that rectangle transparent.

The accent-based approach (`SetWindowCompositionAttribute` + `ACCENT_ENABLE_BLURBEHIND`) works for
the classic Win10 taskbar, but on Win11 the taskbar content is drawn by XAML, so accent only tints
the composition layer — you get a translucent slab instead of real transparency.

Layered windows (`UpdateLayeredWindow` + `ULW_ALPHA`) drive every custom surface (bar, panels,
audio bar). See [`docs/`](docs/) for the pitfalls we hit (the `pptDst` argument moves the window;
`SW_HIDE`/`SW_SHOW` does not restore a layered surface; `WM_SETCURSOR` is what changes the border
cursor; never capture the screen inside `WM_ENTERSIZEMOVE`, …).

## Privacy & security

**The program never touches the network.** There is no networking API anywhere in the source
(no WinHttp / WinINet / socket / curl) and not a single `http(s)://` URL — it does not upload,
report or collect anything. The only IPC is a local channel between the app and its injected module.

**What it reads and writes**

| Location | Purpose |
|---|---|
| `%APPDATA%\hotrainy\` | its own config (`config.json`), per-tab wallpapers, search index, logs |
| `%LOCALAPPDATA%\hotrainy\` | logs of the injected module (`tap.log` / `boot.log` / `crash.log`) |
| `HKCU\...\CurrentVersion\Run` | only if **you** enable "start with Windows" (one per-user startup entry) |

It does not write to system folders, install services or register COM components.

**It injects into explorer.exe — why**

Taskbar transparency requires modifying the Windows 11 taskbar, whose appearance is drawn by XAML.
The app uses `SetWindowsHookEx` to load its own DLL into Explorer and does exactly one thing: hide or
restore a rectangle named `BackgroundFill` in the taskbar's visual tree. There are no keyboard hooks,
no credential access, no network access, and nothing beyond that single visual change.

**Antivirus false positives**

"Inject into Explorer and modify system UI" is the technique every tool in this space uses
(TranslucentTB, RoundedTB, ExplorerPatcher, …) and it is also used by some malware, so Defender,
Huorong or 360 may flag the installer. That is a false positive — adding an exclusion is fine. If you
want to be certain, build it from source yourself or wait for a code-signed build.

**Disclaimer**

This program modifies the system taskbar appearance. A Windows update may break parts of it, and it
can conflict with other taskbar tools (TranslucentTB, RoundedTB, ExplorerPatcher) — **keep only one**.
Use at your own risk; the project is provided "as is" under the PolyForm Noncommercial License 1.0.0
(source-available; **commercial use prohibited**), without warranty.

## License

**PolyForm Noncommercial 1.0.0** — source-available, **commercial use prohibited**; redistributors
must pass on the LICENSE text and every `Required Notice:` line (see [NOTICE](NOTICE)).
Taskbar transparency was informed by the *technical principles* of
TranslucentTB (GPL-3.0); all code in this repository is original. See
[`docs/许可证与原创性说明.md`](docs/许可证与原创性说明.md). An originality self-audit against TranslucentTB's real source (file size, identifier sets, implementation mechanism, comment language) is archived in `docs/原创性比对报告.md`.