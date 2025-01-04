# Helix-MP3-Decoder

Fast and easy to use fixed-point MP3 decoding library based on [libhelix-mp3](https://github.com/ultraembedded/libhelix-mp3) project. Wraps a rather complicated libhelix API into a much simpler dr_libs-style API to allow nearly plug-and-play substitution for [dr_mp3](https://github.com/mackron/dr_libs/blob/master/dr_mp3.h).

## Functionalities
* Pure fixed-point implementation
* Pure C and ASM code, no C++ required
* Support for MPEG-1 Layer 3
* Support for CBR and VBR
* Support for mono and stereo files (although the output is always 2-channel)
* Output in signed 16-bit PCM
* dr_libs-style API, easy to substitute for dr_mp3
* Enables to decode MP3 files in real-time on CPUs without FPU (tested on Pico and RISC-V core of Pico 2)
* ASM optimizations for ARM and RISC-V (easily extendable, see [assembly.h](src/libhelix/real/assembly.h))
* Fallback "unoptimized" routines enabling compilation for other platforms

## Motivation

Main motivation for creating this library was to be able to run real-time MP3 playback on Hazard3 core of Pico2 board.

Typically the libraries I reach for when dealing with any audio decoding related task are [dr_libs](https://github.com/mackron/dr_libs). I've used them in a lot of different projects and find them to be a great piece of software, enabling to quickly add audio decoding capabilities to your device. They are incredibly versatile, but like everything, have their limitations. One of them is dr_mp3's heavy reliance on floating-point operations, which limits its usage to either CPUs with hardware FPU or CPUs powerful enough to perform software floating-point emulation fast enough to play MP3 files.

This was never a problem for me, as the MCU's I used always had an FPU... until I tried running it on Hazard3 core of Pico2 board. Despite writing the code in the most efficient way I could think of and compiling the project with the highest optimization level, it was unable to play MP3 without stuttering. Even overclocking that poor `RP2350` chip from 150MHz to 400(!)MHz wouldn't help. Why? Hazard3 core just happen to not have an FPU. At that moment I knew only a fixed-point decoder would save me... Of course I could just switch to ARM cores which would handle that task easily, but that was not the point at all :wink:

The only fixed-point MP3 decoder I could think of was libhelix-mp3, which I tried using some time ago, but never managed to get even past a compilation stage due to `unsupported platform` errors. Another issue was that its API is completely different (and in my opinion quite complicated to use) from the dr_libs API, for which I had already written a lot of code in many projects and I definitely didn't want to rewrite it. So I decided to spend some time to finally understand libhelix-mp3 and wrap it into a dr_libs-style API to be able to replace dr_mp3 with a mostly plug-and-play approach.

And so this library was born.

## API description

See [header file](src/helix_mp3.h) and [example source code](examples/decode/example_decode.c).

## How to use it in a project

The best way is probably to add this library as a submodule to your project:
```
git add submodule https://github.com/Lefucjusz/Helix-MP3-Decoder.git
```
then add library's directory to your main CMakeLists.txt:
```
add_subdirectory(Helix-MP3-Decoder)
```
After that you can link the library wherever it is needed:
```
target_link_libraries(<target>
    PRIVATE
        helix_mp3
)
```

## Examples

Minimal example of using the library can be found [here](examples/decode/example_decode.c). This simple utility decodes the provided MP3 file to raw PCM file. 

The example of using the library with custom stream interface can be found [here](examples/decode_custom_io/example_decode_custom_io.c). This utility does exactly the same job as the minimal example, but using a custom stream interface.

To compile the examples, clone the repo and run the following commands:
```
mkdir -p build
cd build
cmake .. -DBUILD_EXAMPLES=1
make
```

To use the compiled utilities, run:
```
./examples/decode/example_decode <path_to_mp3_file>.mp3 <path_to_output_file>.raw
```
or:
```
./examples/decode_custom_io/example_decode_custom_io <path_to_mp3_file>.mp3 <path_to_output_file>.raw
```

After a successful run, you should get an output similar to:
```
./examples/decode/example_decode ~/test.mp3 output.raw

Decoding '~/test.mp3' to 'output.raw'...
Reached EOF!
Done! Decoded 721152 frames, last frame sample rate: 44100Hz, bitrate: 32kbps
```
Output file contains raw 16-bit stereo PCM frames and can be played e.g. with Audacity via Import->Raw data option.
