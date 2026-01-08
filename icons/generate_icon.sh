#!/bin/bash

# Generate .icns file from SVG
# This script creates PNG files at various sizes and packages them into an .icns file

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ICONSET_DIR="${SCRIPT_DIR}/app_icon.iconset"
SVG_FILE="${SCRIPT_DIR}/app_icon.svg"
ICNS_FILE="${SCRIPT_DIR}/app_icon.icns"

# Create iconset directory
rm -rf "${ICONSET_DIR}"
mkdir -p "${ICONSET_DIR}"

# Generate PNG files at various sizes required for .icns
# macOS requires specific sizes and naming conventions

echo "Generating icon sizes..."

# 16x16 (1x)
rsvg-convert -w 16 -h 16 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_16x16.png"

# 32x32 (16x16@2x)
rsvg-convert -w 32 -h 32 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_16x16@2x.png"

# 32x32 (1x)
rsvg-convert -w 32 -h 32 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_32x32.png"

# 64x64 (32x32@2x)
rsvg-convert -w 64 -h 64 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_32x32@2x.png"

# 128x128 (1x)
rsvg-convert -w 128 -h 128 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_128x128.png"

# 256x256 (128x128@2x)
rsvg-convert -w 256 -h 256 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_128x128@2x.png"

# 256x256 (1x)
rsvg-convert -w 256 -h 256 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_256x256.png"

# 512x512 (256x256@2x)
rsvg-convert -w 512 -h 512 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_256x256@2x.png"

# 512x512 (1x)
rsvg-convert -w 512 -h 512 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_512x512.png"

# 1024x1024 (512x512@2x)
rsvg-convert -w 1024 -h 1024 "${SVG_FILE}" -o "${ICONSET_DIR}/icon_512x512@2x.png"

# Create .icns file
echo "Creating .icns file..."
iconutil -c icns "${ICONSET_DIR}" -o "${ICNS_FILE}"

# Clean up iconset directory
rm -rf "${ICONSET_DIR}"

echo "Icon created successfully: ${ICNS_FILE}"

