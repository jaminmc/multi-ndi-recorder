# Multi NDI Recorder

A Qt 6 desktop utility for Windows, macOS, and Linux that records multiple NDI sources in parallel with continuous or segmented MP4 output and a built-in recordings browser.

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

### Linux
- **Ubuntu 20.04+ / Debian 11+ / Fedora 34+ / Arch Linux / openSUSE** or equivalent distribution
- **Qt 6 (Widgets)**: install via package manager (`sudo apt install qt6-base-dev qt6-base-dev-tools` on Debian/Ubuntu, `sudo dnf install qt6-qtbase-devel` on Fedora, `sudo pacman -S qt6-base` on Arch, or `sudo zypper install qt6-qtbase-devel` on openSUSE)
- **NDI 6 SDK**: download and install the Linux version. The build system will look for it at `$HOME/src/NDI SDK for Linux` by default.
- **FFmpeg dev libraries**: install via package manager (`sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev` on Debian/Ubuntu, `sudo dnf install ffmpeg-devel` on Fedora, `sudo pacman -S ffmpeg` on Arch, or `sudo zypper install ffmpeg-devel` on openSUSE)
- **Python 3 with Pillow**: required for Windows icon generation (`pip3 install Pillow`)
- **VAAPI support** (optional): For hardware-accelerated encoding with `h264_vaapi` or `hevc_vaapi`, ensure your system has VAAPI drivers installed

## Configure and build

### Windows
1. Open a **x64 Native Tools** developer prompt for your Visual Studio version.
2. Clone this repository and enter it:
   ```powershell
   git clone <repo-url>
   cd multi-ndi-recorder
   ```
3. Generate the app icon (optional but recommended):
   ```powershell
   cd icons
   python create_ico.py
   cd ..
   ```
   > This creates `icons/app_icon.ico` which will be embedded in the executable. Requires Python 3 with Pillow (`pip install Pillow`).
4. Configure CMake with your dependency paths (adjust as needed):
   ```powershell
   cmake -S . -B build -G "Ninja" \
     -DNDI_SDK_INCLUDE="C:/Program Files/NDI SDK/Include" \
     -DNDI_SDK_LIB="C:/Program Files/NDI SDK/Lib/x64" \
     -DFFMPEG_INCLUDE="C:/ffmpeg/include" \
     -DFFMPEG_LIB_ROOT="C:/ffmpeg" \
     -DCMAKE_PREFIX_PATH="C:/Qt/6.5.2/msvc2019_64/lib/cmake"
   ```
   > You can replace `Ninja` with `"Visual Studio 17 2022" -A x64` if you prefer an IDE solution.
5. Build the application:
   ```powershell
   cmake --build build --config Release
   ```
6. Run the app:
   ```powershell
   build/Release/MultiNdiRecorder.exe
   ```
   > The Windows executable will have the application icon embedded if `icons/app_icon.ico` exists.

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

### Linux
1. Clone this repository and enter it:
   ```bash
   git clone <repo-url>
   cd multi-ndi-recorder
   ```
2. Install dependencies (recommended):
   ```bash
   ./install_dependencies.sh
   ```
   > This script automatically detects your Linux distribution and installs the required packages. Supported distributions: Fedora/RHEL, Debian/Ubuntu, Arch Linux, and openSUSE.
   
   Alternatively, install dependencies manually:
   - **Fedora/RHEL**: `sudo dnf install ffmpeg-devel qt6-qtbase-devel qt6-qtbase-devel-tools`
   - **Debian/Ubuntu**: `sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev qt6-base-dev qt6-base-dev-tools`
   - **Arch Linux**: `sudo pacman -S ffmpeg qt6-base`
   - **openSUSE**: `sudo zypper install ffmpeg-devel qt6-qtbase-devel qt6-qtbase-devel-tools`
3. Install the NDI SDK:
   - Download the NDI SDK for Linux from the NewTek website
   - Extract it to `$HOME/src/NDI SDK for Linux` (or update the paths in CMakeLists.txt)
   - The default paths assume: `$HOME/src/NDI SDK for Linux/include` and `$HOME/src/NDI SDK for Linux/lib/x86_64-linux-gnu`
4. Configure CMake (paths are auto-detected, but can be overridden):
   ```bash
   cmake -S . -B build
   ```
   > The build system automatically detects:
   > - NDI SDK at `$HOME/src/NDI SDK for Linux`
   > - FFmpeg headers at `/usr/include/ffmpeg` (Fedora) or `/usr/include` (other distros)
   > - FFmpeg libraries at `/usr/lib64` (Fedora) or `/usr/lib` (other distros)
   > 
   > To override defaults, use:
   > ```bash
   > cmake -S . -B build \
   >   -DNDI_SDK_INCLUDE="/path/to/ndi/include" \
   >   -DNDI_SDK_LIB="/path/to/ndi/lib/x86_64-linux-gnu" \
   >   -DFFMPEG_INCLUDE="/usr/include/ffmpeg" \
   >   -DFFMPEG_LIB_ROOT="/usr"
   > ```
5. Build the application:
   ```bash
   cmake --build build -j$(nproc)
   ```
6. Install the application (optional, for desktop integration):
   ```bash
   sudo cmake --install build --prefix /usr/local
   ```
   > This installs the executable, desktop file, and icon for system-wide access.
7. Run the app:
   ```bash
   build/MultiNdiRecorder
   ```
   > Or if installed: `MultiNdiRecorder` (should appear in your application menu)
   
   **Note**: If using VAAPI hardware encoding (`h264_vaapi` or `hevc_vaapi`), ensure your system has VAAPI drivers installed and the NDI library is accessible:
   ```bash
   export LD_LIBRARY_PATH="$HOME/src/NDI SDK for Linux/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
   ```

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
- **Windows**: 
  - NDI and FFmpeg binaries must be discoverable at run time (e.g., via PATH or next to the executable) so their dependent DLLs load correctly.
  - The application icon is embedded in the executable if `icons/app_icon.ico` exists (generate with `icons/create_ico.py`).
- **macOS**: 
  - NDI and FFmpeg libraries must be in the system library path (e.g., `/usr/local/lib`) or the app's bundle. You may need to set `DYLD_LIBRARY_PATH` if libraries are in non-standard locations.
  - The build creates a proper `.app` bundle with an icon. If the icon doesn't appear, ensure `icons/app_icon.icns` exists (run `icons/generate_icon.sh`).
- **Linux**: 
  - NDI and FFmpeg libraries must be in the system library path (e.g., `/usr/lib` or `/usr/local/lib`). You may need to set `LD_LIBRARY_PATH` if libraries are in non-standard locations:
    ```bash
    export LD_LIBRARY_PATH="$HOME/src/NDI SDK for Linux/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
    ```
  - For desktop integration, install the application (`sudo cmake --install build`) which installs the `.desktop` file and icon.
  - The application will appear in your application menu after installation.
  - **VAAPI Hardware Encoding**: The application supports hardware-accelerated encoding using VAAPI (`h264_vaapi`, `hevc_vaapi`). Ensure your system has VAAPI drivers installed (usually provided by Mesa or Intel/AMD GPU drivers). The build system automatically sets up the required hardware contexts for VAAPI encoding.
- For best disk stability, record to fast local storage rather than network shares.
