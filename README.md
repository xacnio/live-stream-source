# Live Stream Source

Play live streams (RTMP, FLV, SRT, HLS, IVS, WHEP) directly in OBS Studio. Built for low latency and stability, it's a solid choice for restreaming IRL sessions or complex setups.

> ⚠️ Please read [Disclaimer](#disclaimer) and [AI Disclosure](#ai-disclosure) first.

<table border="0">
  <tr>
    <td>If you like my work, you can support me by buying a coffee!</td>
    <td><a href="https://buymeacoffee.com/xacnio"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" width="150" ></a></td>
  </tr>
</table>

## Features

- **Protocol Flexibility** — Support for RTMP, RTMPS, SRT, HLS, IVS (LL-HLS), and WHEP.
- **Hardware Acceleration** — Efficient decoding with NVDEC, QSV, DXVA2, and VideoToolbox.
- **Low Latency** — Tuned for minimum delay and smooth audio/video syncing.
- **Built-in Reliability** — Configurable auto-reconnect for unstable connections.
- **Real-time Metrics** — Monitor bitrate, FPS, latency, and uptime on the fly.
- **Smart Fallbacks** — Switch to another source or overlay when the stream drops or buffers.
- **Multiple Dashboards** — View stats via a built-in window, a browser overlay, or a custom WebSocket feed.

## Supported Protocols

| Type | Protocols | Notes |
|------|-----------|-------|
| **Standard** | RTMP, RTMPS, SRT, RTSP | Via FFmpeg demuxer |
| **Amazon IVS** | LL-HLS | Custom low-latency HLS client |
| **HLS** | Standard HLS | Via FFmpeg |
| **WHEP** | WebRTC | Via libdatachannel, supports video+audio/video-only/audio-only |

## Installation

Download the latest release for your platform from [Releases](https://github.com/xacnio/live-stream-source/releases).

### Windows

**Installer (Recommended):**
1. Get the latest `live-stream-source-*-windows-installer.exe`.
2. Run it and you're done.

**Manual Setup:**
1. Download `live-stream-source-*-windows-x64.zip`.
2. Unzip it to your OBS folder (typically `C:\Program Files\obs-studio\`).
3. Restart OBS Studio.

### macOS

**Installer (PKG):**
1. Download `live-stream-source-*-macos-installer.zip`.
2. Run the `.pkg` file.
3. Restart OBS.

> **Heads up:** The package is unsigned. If macOS blocks it, just head to **System Settings → Privacy & Security** and hit **Open Anyway**.

**Manual Setup:**
1. Grab `live-stream-source-*-macos-universal.zip`.
2. Drop the `live-stream-source.plugin` folder into:
   ```
   ~/Library/Application Support/obs-studio/plugins/
   ```
3. Restart OBS.

### Linux
1. Download `live-stream-source-*-x86_64-linux-gnu.zip`.
2. Extract the ZIP file to `~/.config/obs-studio/plugins/`.
3. Restart OBS Studio

> **Note:** If OBS Studio is installed as a Flatpak or Snap package, the plugin path may be different.
```bash
# Flatpak
~/.var/app/org.obsproject.Studio/config/obs-studio/plugins/

# Snap
~/snap/obs-studio/current/.config/obs-studio/plugins/
```

## Usage

1. Add a new **Live Stream Source** in OBS
2. Enter the stream URL
3. Select the stream type
    - Standard: RTMP, RTMPS, SRT, RTSP
    - Amazon IVS: LL-HLS
    - HLS: Standard HLS
    - WHEP: WebRTC
4. Configure optional settings as needed

### Source Settings

| Setting | Description |
|---------|-------------|
| **Stream URL** | The URL to the live stream |
| **Stream Type** | Standard, Amazon IVS, HLS, or WHEP |
| **Hardware Acceleration** | Enable GPU decoding |
| **Low Bitrate Threshold** | Bitrate (kbps) below which the low-bitrate source is shown |
| **Low Bitrate Overlay Source** | Source to show when bitrate drops below threshold |
| **Disconnect Fallback Source** | Source to show when the stream disconnects |
| **Loading Overlay Source** | Source to show during initial connection |
| **WHEP Bearer Token** | Authentication token for WHEP endpoints |

### Buttons

| Button | Action |
|--------|--------|
| **Refresh** | Reconnect to the stream |
| **Open Statistics Dashboard** | Opens the HTML stats dashboard in a browser |
| **Show Statistics Window** | Opens the built-in Qt stats window |
| **Add Stats Overlay** | Adds a browser source overlay to the current scene |

## WebSocket Stats Server

The plugin runs a WebSocket server (default port `4477`) that broadcasts JSON stats for all active sources. Configure the port and network interface from **Tools → Live Stream Source Dashboard → Settings**.

### JSON Format

```json
{
  "sources": {
    "Source Name": {
      "connected": true,
      "kbps": 4500,
      "fps": 30.0,
      "width": 1920,
      "height": 1080,
      "latency_ms": 120,
      "stream_delay_ms": 1800,
      "dropped_frames": 0,
      "hw_accel": true,
      "uptime_seconds": 3600
    }
  }
}
```

## Building from Source

### Requirements

- CMake 3.16+
- C++17 compiler
- OBS Studio source/SDK
- FFmpeg development libraries [FFMPEG-Builds](https://github.com/btbn/ffmpeg-builds/releases)
- Qt6 (Widgets, Core, Network)
- libdatachannel (fetched automatically for WHEP support)

### Build (Windows)

```bash
cmake -B build -DOBS_SOURCE_DIR="path/to/obs-studio" -DOBS_BIN_DIR="path/to/obs-studio/bin/64bit" -DFFMPEG_DIR="path/to/ffmpeg"
cmake --build build --config Release
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_WHEP` | `ON` | Enable WHEP (WebRTC) receiver support |
| `OBS_SOURCE_DIR` | — | Path to OBS Studio source tree |
| `OBS_BIN_DIR` | — | Path to OBS binary directory |
| `FFMPEG_DIR` | — | Path to FFmpeg dev build |

## Project Structure

```
src/
├── live-stream-source.cpp/h   # Main source plugin
├── stream-demuxer.cpp/h       # FFmpeg stream demuxing
├── video-decoder.cpp/h        # Video decoding (SW/HW)
├── audio-decoder.cpp/h        # Audio decoding
├── whep-client.cpp/h          # WHEP WebRTC client
├── whep-signaling.cpp/h       # WHEP signaling
├── ll-hls-client.cpp/h        # Amazon IVS LL-HLS client
├── ll-hls-parser.cpp/h        # HLS playlist parser
├── ll-hls-fetcher.cpp/h       # HLS segment fetcher
├── bitrate-monitor.cpp/h      # Bitrate tracking
├── catchup-controller.cpp/h   # A/V sync catch-up
├── frame-queue.cpp/h          # Thread-safe frame queue
├── overlay-renderer.cpp/h     # On-source text overlay
├── reconnect-manager.cpp/h    # Auto-reconnect logic
├── ws-stats-server.cpp/h      # WebSocket stats server
├── plugin-settings.cpp/h      # Settings UI (Qt)
├── stats-window.cpp/h         # Stats window (Qt)
├── plugin-main.cpp/h          # Plugin entry point
└── common.h                   # Shared constants & types
```

## Disclaimer

This is an independent, third-party plugin. It is not affiliated with, endorsed by, or officially connected to OBS Studio, the OBS Project, Streamlabs, Amazon (IVS), or any other streaming platform. All product names and trademarks mentioned belong to their respective owners and are used purely for identification and compatibility.

The plugin simply connects to publicly available streaming protocols (RTMP, HLS, SRT, WHEP, etc.) using standard methods. It does not modify, reverse-engineer, or interfere with the internal workings of OBS Studio or any backend streaming service. **Use of this tool is entirely at your own risk.**

## AI Disclosure

To remain fully transparent and comply with the [OBS Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/), I'd like to clarify how this plugin was created.

The initial architecture of the project was prototyped rapidly using an AI-assisted "vibe coding" workflow. Once the foundation was down, I went through the entire codebase to manually review, extensively refactor, and validate everything.

**We have completely phased out the vibe-coding methodology.** The project has fully transitioned back to standard manual software engineering. Moving forward, I am personally writing the code, handling optimizations, and making the technical decisions to ensure long-term maintainability and high code quality.

The project is offered "as is" and without warranty. If you run into any issues, please [open an issue](https://github.com/xacnio/live-stream-source/issues).

## License

This project is licensed under the [GNU General Public License v2.0](LICENSE).
