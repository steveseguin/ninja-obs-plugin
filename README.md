# OBS VDO.Ninja Plugin

[![CI](https://github.com/steveseguin/ninja-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/steveseguin/ninja-plugin/actions/workflows/ci.yml)
[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](LICENSE)

A native OBS Studio plugin for [VDO.Ninja](https://vdo.ninja) integration, enabling WebRTC streaming directly from OBS.

## Features

### Output (Publishing)
- Stream directly to VDO.Ninja from OBS
- Support for multiple simultaneous P2P viewers
- H.264, VP8, and VP9 video codec support
- Opus audio codec
- Configurable bitrate and quality
- Automatic reconnection on connection loss
- Data channel support for tally lights, chat, and remote control

### Source (Viewing)
- View VDO.Ninja streams as an OBS source
- Low-latency WebRTC playback
- Room support for multi-stream sessions
- Password-protected streams

### Virtual Camera Integration
- Use VDO.Ninja as a destination when starting virtual camera
- Seamless integration with OBS workflow

## Requirements

- OBS Studio 30.0 or later
- libdatachannel (WebRTC library)
- OpenSSL

## Installation

### Pre-built Releases (Recommended)

Download the latest release from the [Releases page](https://github.com/steveseguin/ninja-plugin/releases).

**Linux (Debian/Ubuntu):**
```bash
sudo dpkg -i obs-vdoninja-*.deb
```

**Windows:**
1. Extract the ZIP file
2. Copy `obs-vdoninja.dll` and `datachannel.dll` to:
   - `C:\Program Files\obs-studio\obs-plugins\64bit\`

**macOS:**
1. Extract the ZIP file
2. Copy `obs-vdoninja.plugin` to:
   - `~/Library/Application Support/obs-studio/plugins/`

### Using Installer Scripts

Clone this repository and run the appropriate installer:

**Linux:**
```bash
./scripts/install-linux.sh
```

**Windows (PowerShell as Administrator):**
```powershell
.\scripts\install-windows.ps1
```

**macOS:**
```bash
./scripts/install-macos.sh
```

### Building from Source

#### Prerequisites

**Ubuntu/Debian:**
```bash
sudo apt install cmake build-essential libobs-dev libssl-dev git
```

**Windows:**
- Visual Studio 2022 with C++ workload
- CMake
- Git
- vcpkg (for OpenSSL)

**macOS:**
```bash
brew install cmake openssl@3 git
```

#### Build Steps

1. **Build libdatachannel** (required dependency):

```bash
git clone --depth 1 --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel
git submodule update --init --recursive --depth 1
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNO_EXAMPLES=ON -DNO_TESTS=ON
cmake --build build -j$(nproc)
sudo cmake --install build
```

2. **Build the plugin:**

```bash
cd obs-vdoninja
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

#### Windows Build

```powershell
# Install OpenSSL via vcpkg
vcpkg install openssl:x64-windows

# Build libdatachannel
git clone --depth 1 --branch v0.20.2 https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel
git submodule update --init --recursive --depth 1
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DNO_EXAMPLES=ON -DNO_TESTS=ON
cmake --build build --config Release
cmake --install build --prefix ../libdatachannel-install

# Build plugin
cd ../obs-vdoninja
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="../libdatachannel-install"
cmake --build build --config Release
```

## Usage

### Publishing to VDO.Ninja

1. Go to **Settings → Stream**
2. Select **VDO.Ninja** as the service
3. Enter your **Stream ID** (this will be your view link: `https://vdo.ninja/?view=YourStreamID`)
4. Optionally set a **Room ID** and **Password**
5. Click **Start Streaming**

Your stream will be available at: `https://vdo.ninja/?view=<StreamID>`

### Viewing VDO.Ninja Streams

1. Add a new source → **VDO.Ninja Source**
2. Enter the **Stream ID** of the stream you want to view
3. If the stream is in a room, enter the **Room ID**
4. If password-protected, enter the **Password**

### Settings

| Setting | Description | Default |
|---------|-------------|---------|
| Stream ID | Unique identifier for your stream | Required |
| Room ID | Room for multi-stream sessions | Optional |
| Password | Encryption password | VDO.Ninja default |
| Video Codec | H.264, VP8, or VP9 | H.264 |
| Bitrate | Target video bitrate (kbps) | 4000 |
| Max Viewers | Maximum P2P connections | 10 |
| Enable Data Channel | Tally, chat support | Yes |
| Auto Reconnect | Reconnect on failure | Yes |
| Force TURN | Force relay mode | No |

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     OBS VDO.Ninja Plugin                      │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────┐         ┌─────────────────────────┐    │
│  │  VDONinja       │         │  VDONinja               │    │
│  │  Output         │         │  Source                 │    │
│  │  (Publisher)    │         │  (Viewer)               │    │
│  └────────┬────────┘         └───────────┬─────────────┘    │
│           │                              │                   │
│           └──────────┬───────────────────┘                   │
│                      │                                       │
│           ┌──────────▼──────────┐                           │
│           │  Peer Manager       │                           │
│           │  (Multi-connection) │                           │
│           └──────────┬──────────┘                           │
│                      │                                       │
│           ┌──────────▼──────────┐                           │
│           │  Signaling Client   │                           │
│           │  (WebSocket)        │                           │
│           └──────────┬──────────┘                           │
│                      │                                       │
├──────────────────────┼───────────────────────────────────────┤
│                      │                                       │
│           ┌──────────▼──────────┐                           │
│           │  libdatachannel     │                           │
│           │  (WebRTC Stack)     │                           │
│           └─────────────────────┘                           │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

## VDO.Ninja Protocol

This plugin implements the VDO.Ninja signaling protocol:

- **WebSocket Signaling**: Connects to `wss://wss.vdo.ninja`
- **Room Management**: Join/leave rooms with hashed IDs
- **Stream Publishing**: `seed` request with hashed stream ID
- **Stream Viewing**: `play` request for specific streams
- **ICE Candidate Bundling**: Batched candidate exchange
- **Data Channels**: P2P messaging for tally, chat, control

### Message Types

- `joinroom` - Join a room
- `seed` - Publish a stream
- `play` - Request to view a stream
- `offer/answer` - SDP exchange
- `candidate` - ICE candidates

## Development

### Project Structure

```
obs-vdoninja/
├── CMakeLists.txt
├── LICENSE                   # AGPL-3.0 License
├── src/
│   ├── plugin-main.cpp        # Plugin entry point
│   ├── vdoninja-signaling.*   # WebSocket signaling
│   ├── vdoninja-peer-manager.*# Multi-peer WebRTC
│   ├── vdoninja-output.*      # OBS output (publish)
│   ├── vdoninja-source.*      # OBS source (view)
│   ├── vdoninja-data-channel.*# Data channel support
│   ├── vdoninja-utils.*       # Utilities
│   └── vdoninja-common.h      # Shared types
├── tests/
│   ├── test-utils.cpp         # Utility function tests
│   ├── test-json.cpp          # JSON parser tests
│   ├── test-data-channel.cpp  # Data channel tests
│   └── stubs/                 # OBS API stubs for testing
├── scripts/
│   ├── install-linux.sh       # Linux installer
│   ├── install-windows.ps1    # Windows installer
│   └── install-macos.sh       # macOS installer
├── .github/workflows/
│   ├── ci.yml                 # CI workflow
│   └── release.yml            # Release workflow
└── data/
    └── locale/
        └── en-US.ini          # Localization
```

### Building for Development

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Running Tests

```bash
# Configure with tests enabled
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug

# Build tests
cmake --build build --target vdoninja-tests

# Run tests
ctest --test-dir build --output-on-failure

# Or run directly
./build/vdoninja-tests
```

### Test Coverage

The test suite covers:
- **Utility functions**: UUID/session ID generation, SHA256 hashing, stream ID sanitization
- **JSON handling**: Builder and parser for VDO.Ninja protocol messages
- **Base64 encoding/decoding**: For binary data handling
- **String utilities**: URL encoding, trimming, splitting
- **Data channel**: Message parsing, creation, tally state management
- **VDO.Ninja protocol**: Message format validation

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests to ensure they pass
5. Submit a pull request

## License

This project is licensed under the **GNU Affero General Public License v3.0** - see the [LICENSE](LICENSE) file for details.

## Credits

- [VDO.Ninja](https://vdo.ninja) by Steve Seguin
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) by Paul-Louis Ageneau
- [OBS Studio](https://obsproject.com)

## Links

- [VDO.Ninja Documentation](https://docs.vdo.ninja)
- [VDO.Ninja SDK](https://github.com/steveseguin/ninjasdk)
- [OBS Plugin Development](https://obsproject.com/docs/plugins.html)
