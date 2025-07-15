#!/bin/bash
# Auto-generated picotool script for multi-audio binary
# Binary file: wav_audio.bin
# Files: 8, Sample Rate: 24000 Hz
# File size: 6,903,024 bytes (6.58 MB)
# Flash address: 0x10200000

echo "Loading multi-audio binary to flash..."
echo "File: wav_audio.bin"
echo "Size: 6,903,024 bytes (6.58 MB)"
echo "Address: 0x10200000"
echo "Files: 8"
echo ""
picotool load -x wav_audio.bin -o 0x10200000
echo "Multi-audio binary loaded successfully!"
