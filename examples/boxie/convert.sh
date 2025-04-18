#!/bin/bash

# Make sure we're in UTF-8 mode
export LC_ALL=en_US.UTF-8

# Process files one by one
find . -type f -name "*.mp3" -print0 | while IFS= read -r -d $'\0' file; do
    # Skip files that don't exist (just in case)
    [ ! -f "$file" ] && continue

    # Get the sample rate, redirecting stderr to null to avoid noise
    sample_rate=$(ffprobe -v quiet -select_streams a:0 -show_entries stream=sample_rate -of default=noprint_wrappers=1:nokey=1 "$file" 2>/dev/null)

    # If sample rate is not 44100 Hz and we got a valid sample rate, convert it
    if [ -n "$sample_rate" ] && [ "$sample_rate" != "44100" ]; then
        echo "Converting: $file (current rate: ${sample_rate}Hz)"

        # Create temporary file in the same directory as the original
        temp_file="${file%/*}/.temp_${file##*/}"

        # Convert to 44.1kHz
        if ffmpeg -i "$file" -ar 44100 -y "$temp_file" -loglevel error; then
            # Only replace if conversion was successful
            if mv "$temp_file" "$file"; then
                echo "Successfully converted: $file"
            else
                echo "Error moving temporary file for: $file"
                rm -f "$temp_file"  # Clean up temp file if move failed
            fi
        else
            echo "Error converting: $file"
            rm -f "$temp_file"  # Clean up temp file if conversion failed
        fi
    else
        echo "Skipping: $file (current rate: ${sample_rate:-unknown}Hz)"
    fi
done