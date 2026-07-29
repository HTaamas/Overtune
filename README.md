# Overtune

**Ride on top of everything and take the wheel on Spotify's volume — without ever touching your system volume.**

A lightweight, cross-platform Spotify controller with a custom volume OSD and an "Up Next" overlay. It lives in your system tray and uses global hotkeys to drive Spotify's own playback volume, transport, and library.

## Features

*   **Global Hotkey Control**: Intercepts the volume keys to control Spotify's *application* volume directly (not your system volume). Use Shift for fine steps; double-tap to play/pause, Shift/Ctrl to skip.
*   **Customizable OSD**: A modern, non-intrusive On-Screen Display shows track info, album art, progress, and volume — with a liked-heart and a Smart Shuffle sparkle — without stealing focus.
*   **Up Next overlay**: A draggable, resizable, always-on-top queue showing the current song and what's coming — with album art, liked hearts, and Smart Shuffle markers. Click a song to jump to it; lock it click-through with a hotkey.
*   **Liked Songs**: One hotkey (Alt+S by default) saves/removes the current track from your Liked Songs, reflected live on the heart.
*   **Fully themeable & configurable**: Every overlay color, the window opacity, and all hotkeys are remappable in a dark settings window.
*   **No admin required**: Runs as a normal user (optional elevation only if you want hotkeys over admin windows).
*   **Cross-Platform**: A single codebase supporting Windows, macOS, and Linux (X11).
*   **No-Focus Overlay**: The overlays appear over your active window (including full-screen games) without interrupting your workflow.

## Setup

No Spotify API keys, developer app, or `.env` file are required. Overtune authenticates using Spotify's OAuth2 **device flow** with the built-in Spotify desktop client id.

On first launch — or any time from **Settings → Connect Spotify** — your browser opens a Spotify authorization page. Approve it and the app connects. Your login is remembered afterwards (a refresh token is stored locally in your OS settings store), so you won't need to re-authorize on later launches.

The app never handles your password or any cookie. Once connected it talks only to Spotify's realtime Connect backend (the same protocol the official clients use), streaming playback state over a WebSocket rather than polling the public Web API — so it isn't subject to Web-API rate limits.

## Project Structure

```
src/
  main.cpp        - entry point and signal wiring between modules
  spotify/        - Spotify Connect client (auth, WebSocket state, playback commands)
  ui/             - everything visual: OSD overlay, tray icon, settings dialog
  input/          - global hotkey capture (per-platform keyboard hooks)
  settings/       - persistent app settings (overlay theme, keybinds)
  platform/       - miscellaneous platform glue (macOS app behavior)
  proto/          - Spotify Connect protobuf definitions
```

## Building from Source

This project is built using CMake. You need Qt 6 (Widgets, Network, Gui, WebSockets), Protobuf (`protoc` plus libprotobuf), zlib, CMake, a C++17 compiler, and — on Linux — X11 development headers.

### Windows (MinGW)

These instructions assume a MinGW-based Qt installation.

```powershell
# 1. Add MinGW to your PATH for this session
$env:Path = "D:\Qt\Tools\mingw1310_64\bin;" + $env:Path

# 2. Configure the project
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.1/mingw_64 `
  -DCMAKE_C_COMPILER=D:/Qt/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe

# 3. Build the executable
cmake --build build

# 4. Deploy Qt runtime DLLs
D:/Qt/6.11.1/mingw_64/bin/windeployqt.exe --release --force --dir build build/Overtune.exe
```

Adjust the `D:\Qt` paths if your Qt installation lives elsewhere.

The final executable will be at `build/Overtune.exe`.

### macOS

The app builds into a standard `.app` bundle.

```bash
# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The final bundle will be at `build/Overtune.app`. On first run, macOS will prompt for "Input Monitoring" and "Accessibility" permissions, which are required for global hotkey interception.

### Linux (X11)

Ensure you have Qt 6 development packages (`Widgets`, `Gui`, `Network`, `WebSockets`), Protobuf (`protobuf-compiler`, `libprotobuf-dev`), zlib (`zlib1g-dev`), and X11 development headers (`libx11-dev`) installed.

```bash
# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The final executable will be at `build/Overtune`. Global key interception is supported on X11 sessions but is not available on Wayland.

## Usage

1.  Run the executable (`Overtune.exe`, `open build/Overtune.app`, or `./build/Overtune`).
2.  On the first launch, you will be prompted to authorize the app with your Spotify account in your web browser.
3.  Once authorized, the app will run in the system tray.
4.  Use your keyboard's media keys (Volume Up/Down) to control Spotify's volume. The OSD will appear.
5.  Right-click the tray icon to access settings, re-link your account, or quit the application.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.