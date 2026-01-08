#!/bin/bash
# Script to install build dependencies for Multi NDI Recorder on Linux
# Supports: Fedora/RHEL, Debian/Ubuntu, Arch Linux, openSUSE

set -e

# Detect distribution
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO=$ID
        DISTRO_VERSION=$VERSION_ID
    elif [ -f /etc/redhat-release ]; then
        DISTRO="rhel"
    elif [ -f /etc/debian_version ]; then
        DISTRO="debian"
    else
        echo "ERROR: Unable to detect Linux distribution"
        exit 1
    fi
}

# Install dependencies based on distribution
install_dependencies() {
    case $DISTRO in
        fedora|rhel|centos|almalinux|rocky)
            echo "Detected Fedora/RHEL-based distribution"
            echo "Installing FFmpeg development headers..."
            sudo dnf install -y ffmpeg-devel
            
            echo "Checking Qt6 installation..."
            if ! pkg-config --exists Qt6Widgets; then
                echo "Qt6 not found. Installing Qt6 development packages..."
                sudo dnf install -y qt6-qtbase-devel qt6-qtbase-devel-tools
            fi
            ;;
        debian|ubuntu)
            echo "Detected Debian/Ubuntu-based distribution"
            echo "Installing FFmpeg development headers..."
            sudo apt-get update
            sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
            
            echo "Checking Qt6 installation..."
            if ! pkg-config --exists Qt6Widgets; then
                echo "Qt6 not found. Installing Qt6 development packages..."
                sudo apt-get install -y qt6-base-dev qt6-base-dev-tools
            fi
            ;;
        arch|manjaro)
            echo "Detected Arch Linux-based distribution"
            echo "Installing FFmpeg development headers..."
            sudo pacman -S --noconfirm ffmpeg
            
            echo "Checking Qt6 installation..."
            if ! pkg-config --exists Qt6Widgets; then
                echo "Qt6 not found. Installing Qt6 development packages..."
                sudo pacman -S --noconfirm qt6-base
            fi
            ;;
        opensuse*|sles)
            echo "Detected openSUSE/SLES distribution"
            echo "Installing FFmpeg development headers..."
            sudo zypper install -y ffmpeg-devel
            
            echo "Checking Qt6 installation..."
            if ! pkg-config --exists Qt6Widgets; then
                echo "Qt6 not found. Installing Qt6 development packages..."
                sudo zypper install -y qt6-qtbase-devel qt6-qtbase-devel-tools
            fi
            ;;
        *)
            echo "ERROR: Unsupported distribution: $DISTRO"
            echo "Please install the following packages manually:"
            echo "  - FFmpeg development libraries (libavformat-dev, libavcodec-dev, libavutil-dev, libswscale-dev, libswresample-dev)"
            echo "  - Qt6 development packages (qt6-base-dev or qt6-qtbase-devel)"
            exit 1
            ;;
    esac
}

# Main execution
echo "Multi NDI Recorder - Dependency Installer"
echo "=========================================="
detect_distro
install_dependencies
echo ""
echo "Dependencies installation complete!"
echo ""
echo "You can now build the project with:"
echo "  mkdir -p build && cd build"
echo "  cmake .."
echo "  cmake --build ."

