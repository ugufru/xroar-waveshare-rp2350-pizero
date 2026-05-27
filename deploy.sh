#!/bin/bash
set -euo pipefail

# coco-rp2350-43b Build & Deploy Script
# Usage: ./deploy.sh [--build-only] [--monitor]

cd "$(dirname "$0")"

BUILD_ONLY=false
MONITOR=false

for arg in "$@"; do
    case $arg in
        --build-only) BUILD_ONLY=true ;;
        --monitor)    MONITOR=true ;;
        --help|-h)
            echo "Usage: $0 [--build-only] [--monitor]"
            echo ""
            echo "  --build-only  Build without uploading"
            echo "  --monitor     Open serial monitor after upload"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            exit 1
            ;;
    esac
done

# Check dependencies
if ! command -v pio &> /dev/null; then
    echo "Error: PlatformIO CLI (pio) not found."
    echo "Install: https://docs.platformio.org/en/latest/core/installation/methods/installer-script.html"
    exit 1
fi

# Build
echo "=== Building for RP2350 ==="
START=$(date +%s)
pio run -e rp2350
END=$(date +%s)
echo "Build completed in $((END - START))s"

# Show firmware size if arm-none-eabi-size is available
FIRMWARE=$(find .pio/build/rp2350 -name "firmware.elf" 2>/dev/null | head -1)
if [ -n "$FIRMWARE" ] && command -v arm-none-eabi-size &> /dev/null; then
    echo ""
    arm-none-eabi-size "$FIRMWARE"
fi
echo ""

if [ "$BUILD_ONLY" = true ]; then
    echo "Build-only mode — skipping upload."
    exit 0
fi

# Upload
echo "=== Uploading to RP2350 ==="
echo "Make sure the board is connected via USB-C."
echo "If upload fails, hold BOOTSEL while plugging in, then retry."
echo ""
pio run -e rp2350 -t upload
echo ""
echo "Upload complete!"

# Monitor (optional)
if [ "$MONITOR" = true ]; then
    echo "=== Opening serial monitor (115200 baud) ==="
    echo "Press Ctrl+C to exit."
    pio device monitor -b 115200
fi
