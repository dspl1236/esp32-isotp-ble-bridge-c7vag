#!/usr/bin/env python3
"""
build_spiffs.py — minify the VAG-CP PWA and package it into a SPIFFS image.

Run before flashing WiFi firmware:
    pip install minify-html
    python build_spiffs.py
    esptool.py write_flash 0xD0000 spiffs.bin

Or run automatically via idf.py:
    idf.py spiffsgen  (if spiffsgen.py is in PATH)
"""
import pathlib, subprocess, sys, shutil, os

SRC_PWA  = pathlib.Path("../VAG-CP-Docs/docs/index.html")  # relative path
WEB_DIR  = pathlib.Path("web")
OUT_BIN  = pathlib.Path("spiffs.bin")
PART_SIZE = 0x130000  # must match partitions.csv

WEB_DIR.mkdir(exist_ok=True)

# Copy + minify HTML
if SRC_PWA.exists():
    try:
        import minify_html
        src = SRC_PWA.read_text()
        minified = minify_html.minify(src, minify_js=True, minify_css=True)
        (WEB_DIR / "index.html").write_text(minified)
        orig = len(src)
        mini = len(minified)
        print(f"Minified: {orig:,} → {mini:,} bytes ({100-mini*100//orig}% smaller)")
    except ImportError:
        # No minifier — just copy
        shutil.copy(SRC_PWA, WEB_DIR / "index.html")
        print("Copied index.html (install minify-html for smaller output)")
else:
    # Create minimal placeholder
    (WEB_DIR / "index.html").write_text(
        "<html><body><h1>FunkBridge</h1>"
        "<p>Flash VAG-CP PWA to see full diagnostic tool</p></body></html>"
    )
    print("Created placeholder index.html")

# Build SPIFFS image using IDF tool
spiffsgen = shutil.which("spiffsgen.py")
if not spiffsgen:
    idf_path = os.environ.get("IDF_PATH", "")
    spiffsgen = str(pathlib.Path(idf_path) / "components/spiffs/spiffsgen.py")

if pathlib.Path(spiffsgen).exists():
    result = subprocess.run(
        [sys.executable, spiffsgen,
         str(PART_SIZE), str(WEB_DIR), str(OUT_BIN)],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        size = OUT_BIN.stat().st_size
        print(f"SPIFFS image: {OUT_BIN} ({size:,} bytes / {PART_SIZE:,} partition)")
    else:
        print(f"spiffsgen failed: {result.stderr}")
else:
    print("spiffsgen.py not found — run from within ESP-IDF environment")
    print("Or flash manually: esptool.py write_flash 0xD0000 <your_spiffs.bin>")
