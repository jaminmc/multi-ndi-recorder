#!/usr/bin/env python3
"""
Create .icns file from PNG images using PIL and subprocess to call iconutil.
This is a workaround for iconutil's strict requirements.
"""
import os
import sys
import subprocess
import tempfile
import shutil
from pathlib import Path

def create_icns():
    script_dir = Path(__file__).parent
    iconset_dir = script_dir / "app_icon.iconset"
    icns_file = script_dir / "app_icon.icns"
    
    if not iconset_dir.exists():
        print(f"Error: {iconset_dir} does not exist")
        return 1
    
    # Create a temporary iconset with proper structure
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_iconset = Path(tmpdir) / "app_icon.iconset"
        tmp_iconset.mkdir()
        
        # Copy all PNG files
        for png_file in iconset_dir.glob("*.png"):
            shutil.copy(png_file, tmp_iconset / png_file.name)
        
        # Copy Contents.json if it exists
        contents_json = iconset_dir / "Contents.json"
        if contents_json.exists():
            shutil.copy(contents_json, tmp_iconset / "Contents.json")
        
        # Try to create .icns
        try:
            result = subprocess.run(
                ["iconutil", "-c", "icns", str(tmp_iconset), "-o", str(icns_file)],
                capture_output=True,
                text=True,
                check=True
            )
            print(f"Successfully created {icns_file}")
            return 0
        except subprocess.CalledProcessError as e:
            print(f"Error creating .icns: {e.stderr}")
            print("Trying alternative method...")
            
            # Alternative: Use sips to convert largest PNG
            largest_png = max(iconset_dir.glob("*.png"), key=lambda p: p.stat().st_size)
            try:
                # Create a simple .icns using sips (may not work in sandbox)
                subprocess.run(
                    ["sips", "-s", "format", "icns", str(largest_png), "--out", str(icns_file)],
                    check=True
                )
                print(f"Successfully created {icns_file} using sips")
                return 0
            except Exception as e2:
                print(f"Alternative method also failed: {e2}")
                print("\nYou may need to:")
                print("1. Open the PNG files in Preview")
                print("2. Export as .icns using Get Info")
                print("3. Or use a tool like Icon Composer")
                return 1

if __name__ == "__main__":
    sys.exit(create_icns())

