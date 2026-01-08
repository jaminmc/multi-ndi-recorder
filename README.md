# Multi NDI Recorder

A Qt 6 desktop utility for Windows and macOS that records multiple NDI sources in parallel with continuous or segmented MP4 output and a built-in recordings browser.

## Features at a glance
- Configure 1–10 NDI inputs, each with preview, start/stop/pause controls, and a per-source timer; global Start/Pause/Stop manage every recorder at once.
- Per-source settings dialog to pick NDI source, output folder, labeling, and continuous vs. segmented recording durations.
- Native-resolution H.264 MP4 writing with optional time-based segment rollover handled by the FFmpeg pipeline.
- Recording library tab lists completed files with open/reveal actions, plus simple metadata scanning.
- Lightweight logging to `logs/app.log` for capture and muxing events.

## Prerequisites

### Windows
- **Windows 10/11 64-bit** with the **Desktop development with C++** workload from Visual Studio 2019/2022 (MSVC, Windows SDK, CMake, and Ninja if desired).
- **Qt 6 (Widgets)**: install a matching MSVC build (e.g., 6.5+). Note the `CMAKE_PREFIX_PATH` to its `lib/cmake` directory.
- **NDI 6 SDK**: install and record the `Include` and `Lib/x64` directories.
- **FFmpeg dev libraries** built for MSVC with import libraries (`avformat`, `avcodec`, `avutil`, `swscale`) and headers available.

### macOS
- **macOS 10.15 or later** with Xcode Command Line Tools installed (`xcode-select --install`)
- **Qt 6 (Widgets)**: install via Homebrew (`brew install qt@6`) or download from qt.io. Note the `CMAKE_PREFIX_PATH` to its `lib/cmake` directory.
- **NDI 6 SDK**: download and install the macOS version. Default installation path is `/Library/NDI SDK for Apple`.
- **FFmpeg dev libraries**: install via Homebrew (`brew install ffmpeg`) or build from source.

## Configure and build

### Windows
1. Open a **x64 Native Tools** developer prompt for your Visual Studio version.
2. Clone this repository and enter it:
   ```powershell
   git clone <repo-url>
   cd multi-ndi-recorder
   ```
3. Configure CMake with your dependency paths (adjust as needed):
   ```powershell
   cmake -S . -B build -G "Ninja" \
     -DNDI_SDK_INCLUDE="C:/Program Files/NDI SDK/Include" \
     -DNDI_SDK_LIB="C:/Program Files/NDI SDK/Lib/x64" \
     -DFFMPEG_INCLUDE="C:/ffmpeg/include" \
     -DFFMPEG_LIB_ROOT="C:/ffmpeg" \
     -DCMAKE_PREFIX_PATH="C:/Qt/6.5.2/msvc2019_64/lib/cmake"
   ```
   > You can replace `Ninja` with `"Visual Studio 17 2022" -A x64` if you prefer an IDE solution.
4. Build the application:
   ```powershell
   cmake --build build --config Release
   ```
5. Run the app:
   ```powershell
   build/Release/MultiNdiRecorder.exe
   ```

### macOS
1. Clone this repository and enter it:
   ```bash
   git clone <repo-url>
   cd multi-ndi-recorder
   ```
2. Generate the app icon (optional but recommended):
   ```bash
   cd icons
   ./generate_icon.sh
   cd ..
   ```
   > If the script fails, you can manually create the `.icns` file by opening the PNG files in the `icons/app_icon.iconset` directory in Preview and exporting as `.icns`, or use a tool like Icon Composer.
3. Configure CMake with your dependency paths (defaults work for standard Homebrew/NDI SDK installations):
   ```bash
   cmake -S . -B build \
     -DNDI_SDK_INCLUDE="/Library/NDI SDK for Apple/include" \
     -DNDI_SDK_LIB="/Library/NDI SDK for Apple/lib/macOS" \
     -DFFMPEG_INCLUDE="/opt/homebrew/include" \
     -DFFMPEG_LIB_ROOT="/opt/homebrew" \
     -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)/lib/cmake"
   ```
   > For Intel Macs, use `/usr/local` instead of `/opt/homebrew` for FFmpeg paths. If Qt or other dependencies are installed elsewhere, adjust paths accordingly.
4. Build the application:
   ```bash
   cmake --build build --config Release
   ```
5. Run the app:
   ```bash
   open build/MultiNdiRecorder.app
   ```
   > The build now creates a proper macOS `.app` bundle that can be distributed and launched like any other macOS application.

## Using the application
1. **Set source count**: Use the spin box at the top to choose how many NDI tiles to display (1–10). Tiles show preview, status, and an elapsed timer.
2. **Configure each source**: Click **Settings** on a tile to pick the NDI source, output folder, label, and continuous vs. segmented duration. Press **Refresh** to rescan sources.
3. **Start recording**: Hit **Start** on a tile or **Start All** for every source. Pause/Resume keeps the file active; Stop finalizes it. Segmented mode automatically rolls over files at the chosen minute interval.
4. **Library tab**: Switch to the Recordings tab to see captured files. Double-click **Open** to launch in the default player or **Reveal** to highlight in Explorer (Windows) or Finder (macOS).
5. **Logs**: Review `logs/app.log` for capture, NDI, and FFmpeg events when diagnosing issues.

## UI Modernization
The application features a modernized UI with:
- Improved spacing and margins throughout
- macOS-native styling and appearance
- Better organized controls and layouts
- Enhanced visual hierarchy and readability
- Platform-specific button labels (e.g., "Reveal in Finder" on macOS)

## Notes and tips
- Ensure output folders exist and are writable before starting a session.
- **Windows**: NDI and FFmpeg binaries must be discoverable at run time (e.g., via PATH or next to the executable) so their dependent DLLs load correctly.
- **macOS**: NDI and FFmpeg libraries must be in the system library path (e.g., `/usr/local/lib`) or the app's bundle. You may need to set `DYLD_LIBRARY_PATH` if libraries are in non-standard locations.
- **macOS App Bundle**: The build creates a proper `.app` bundle with an icon. If the icon doesn't appear, ensure `icons/app_icon.icns` exists (run `icons/generate_icon.sh`).
- For best disk stability, record to fast local storage rather than network shares.
