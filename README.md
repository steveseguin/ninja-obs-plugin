# OBS VDO.Ninja Plugin

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

## Building

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install cmake build-essential libobs-dev libssl-dev

# Install libdatachannel
git clone https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel
cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0
cmake --build build
sudo cmake --install build
```

### Build Plugin

```bash
cd obs-vdoninja
mkdir build && cd build
cmake ..
make
sudo make install
```

### Windows Build

```powershell
# Requires Visual Studio 2022 and vcpkg
vcpkg install libdatachannel:x64-windows openssl:x64-windows
cmake -B build -G "Visual Studio 17 2022" -A x64
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
├── src/
│   ├── plugin-main.cpp        # Plugin entry point
│   ├── vdoninja-signaling.*   # WebSocket signaling
│   ├── vdoninja-peer-manager.*# Multi-peer WebRTC
│   ├── vdoninja-output.*      # OBS output (publish)
│   ├── vdoninja-source.*      # OBS source (view)
│   ├── vdoninja-data-channel.*# Data channel support
│   ├── vdoninja-utils.*       # Utilities
│   └── vdoninja-common.h      # Shared types
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
cd build
ctest --output-on-failure
```

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## License

This project is licensed under the GPL-2.0 License - see the [LICENSE](LICENSE) file for details.

## Credits

- [VDO.Ninja](https://vdo.ninja) by Steve Seguin
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) by Paul-Louis Ageneau
- [OBS Studio](https://obsproject.com)

## Links

- [VDO.Ninja Documentation](https://docs.vdo.ninja)
- [VDO.Ninja SDK](https://github.com/steveseguin/ninjasdk)
- [OBS Plugin Development](https://obsproject.com/docs/plugins.html)
