#!/bin/bash

# Check if URL argument is provided
if [ $# -ne 1 ]; then
    echo "Usage: $0 <PLAYLIST_URL>"
    echo "Example: $0 https://music.youtube.com/playlist?list=..."
    exit 1
fi

PLAYLIST_URL="$1"

# Download audio in best quality and convert to mp3
~/Downloads/yt-dlp_macos \
    --extract-audio \
    --audio-format mp3 \
    --audio-quality 0 \
    --output "%(title)s.%(ext)s" \
    --add-metadata \
    --embed-thumbnail \
    --ignore-errors \
    --cookies-from-browser chrome \
    "$PLAYLIST_URL"

# Note: Make sure you have youtube-dl and ffmpeg installed:
# On Ubuntu/Debian: sudo apt install youtube-dl ffmpeg
# On macOS with Homebrew: brew install youtube-dl ffmpeg
# On Windows with Chocolatey: choco install youtube-dl ffmpeg