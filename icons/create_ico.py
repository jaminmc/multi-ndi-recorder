#!/usr/bin/env python3
"""
Generate Windows .ico file from PNG icons in iconset directory.
Requires Pillow: pip install Pillow
"""

import os
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)

def create_ico_file():
    """Create a Windows .ico file from PNG icons."""
    script_dir = Path(__file__).parent
    iconset_dir = script_dir / "app_icon.iconset"
    ico_file = script_dir / "app_icon.ico"
    
    if not iconset_dir.exists():
        print(f"Error: Iconset directory not found: {iconset_dir}")
        sys.exit(1)
    
    # Windows .ico files typically include these sizes: 16, 32, 48, 64, 128, 256
    # We'll use the available PNG files and scale as needed
    sizes_to_include = [16, 32, 48, 64, 128, 256]
    images = []
    
    # Map of sizes to available PNG files
    size_map = {
        16: "icon_16x16.png",
        32: "icon_32x32.png",
        128: "icon_128x128.png",
        256: "icon_256x256.png",
    }
    
    for size in sizes_to_include:
        if size in size_map:
            png_path = iconset_dir / size_map[size]
            if png_path.exists():
                img = Image.open(png_path)
                # Ensure RGBA mode for .ico
                if img.mode != 'RGBA':
                    img = img.convert('RGBA')
                # Resize if needed
                if img.size[0] != size:
                    img = img.resize((size, size), Image.Resampling.LANCZOS)
                images.append(img)
            else:
                # Try to create from nearest available size
                if size == 48:
                    # Scale from 32x32
                    if (iconset_dir / "icon_32x32.png").exists():
                        img = Image.open(iconset_dir / "icon_32x32.png")
                        if img.mode != 'RGBA':
                            img = img.convert('RGBA')
                        img = img.resize((48, 48), Image.Resampling.LANCZOS)
                        images.append(img)
                elif size == 64:
                    # Scale from 128x128
                    if (iconset_dir / "icon_128x128.png").exists():
                        img = Image.open(iconset_dir / "icon_128x128.png")
                        if img.mode != 'RGBA':
                            img = img.convert('RGBA')
                        img = img.resize((64, 64), Image.Resampling.LANCZOS)
                        images.append(img)
        else:
            # Create intermediate sizes by scaling
            if size == 48:
                if (iconset_dir / "icon_32x32.png").exists():
                    img = Image.open(iconset_dir / "icon_32x32.png")
                    if img.mode != 'RGBA':
                        img = img.convert('RGBA')
                    img = img.resize((48, 48), Image.Resampling.LANCZOS)
                    images.append(img)
            elif size == 64:
                if (iconset_dir / "icon_128x128.png").exists():
                    img = Image.open(iconset_dir / "icon_128x128.png")
                    if img.mode != 'RGBA':
                        img = img.convert('RGBA')
                    img = img.resize((64, 64), Image.Resampling.LANCZOS)
                    images.append(img)
    
    if not images:
        print("Error: No valid icon images found")
        sys.exit(1)
    
    # Save as .ico file
    try:
        images[0].save(
            ico_file,
            format='ICO',
            sizes=[(img.size[0], img.size[1]) for img in images],
            append_images=images[1:] if len(images) > 1 else []
        )
        print(f"Successfully created: {ico_file}")
        return True
    except Exception as e:
        print(f"Error creating .ico file: {e}")
        sys.exit(1)

if __name__ == "__main__":
    create_ico_file()

