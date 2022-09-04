# asr
Adaptive Stream Recorder (`asr`) is a simple recorder for adaptive media
streaming protocols.

## Features

At the moment only HTTP Live Streaming (HLS) is supported. If a master playlist
is passed to the program, the stream with the highest bandwidth is chosen.

## Getting Started

### Prerequisites

To build `asr`, the minimum software version requirements are CMake 3.18.0,
Boost 1.80.0, OpenSSL 3.0.0, and a C++17 compiler. In addition, liburing 2.1
is required on Linux.

The runtime operating system requirements are Linux 5.15, macOS 12.5 (Monterey),
or Windows 11.

### Building

Often it is sufficient to run:
```
mkdir build
cd build
cmake ..
cmake --build .
```
If CMake is unable to find any dependency, refer to CMake's (and possibly the
compiler's) documentation for the necessary options to specify the location.

### Installing

To install, continue with:
```
cmake --install .
```

### Usage

Run `asr` without any parameters to obtain the usage information.

## License

Refer to the [LICENSE](LICENSE) file for details.
