# Live Stream Source

Play live streams (RTMP, FLV, SRT, HLS, IVS, WHEP) directly in OBS Studio. Built for low latency and stability.

> ⚠️ Please read [Disclaimer](#disclaimer) and [AI Disclosure](#ai-disclosure) first.

<table border="0">
  <tr>
    <td>If you like my work, you can support me by buying a coffee!</td>
    <td><a href="https://buymeacoffee.com/xacnio"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" width="150" ></a></td>
  </tr>
</table>

## Features

- **Protocol Flexibility** — RTMP, RTMPS, SRT, HLS, IVS (LL-HLS), and WHEP.
- **Hardware Acceleration** — NVDEC, QSV, DXVA2, and VideoToolbox.
- **Catch-up Playback** — SoundTouch time-stretching keeps audio artifact-free when recovering from network drops. Triggers at 1 s drift, skips to live edge above 10 s.
- **Auto-Reconnect** — Configurable retry on connection failure.
- **Smart Fallbacks** — Switch to another OBS source on disconnect, low bitrate, or during initial buffering.
- **Real-time Metrics** — Bitrate, FPS, latency, and uptime via a browser dashboard, a Qt stats window, or a raw WebSocket feed.
- **Live Preview Dashboard** — Low-bandwidth JPEG+PCM preview in the browser tab with remote mute, blur, and skip-to-live controls.

## Supported Protocols

| Type | Protocols | Notes |
|------|-----------|-------|
| **Standard** | RTMP, RTMPS, SRT, RTSP | Via FFmpeg demuxer |
| **Amazon IVS** | LL-HLS | Custom low-latency HLS client |
| **HLS** | Standard HLS | Via FFmpeg |
| **WHEP** | WebRTC | Via libdatachannel |

## Installation

Download the latest release for your platform from [Releases](https://github.com/xacnio/live-stream-source/releases).

### OBS Version

**Requires OBS Studio 32.2.0 or newer.** OBS 32.2 moved to FFmpeg 8, changing the
library ABI, so a single build cannot cover both generations.

| Your OBS | Plugin version |
| -------- | -------------- |
| 32.2.0 and newer | 1.3.0 and newer |
| 32.1.x and older | 1.2.2 |

Installing the wrong pair makes OBS report *"The following OBS plugins failed to
load"* on startup — the plugin is looking for FFmpeg libraries your OBS does not
ship.

### Windows

**Installer (Recommended):**
1. Get `live-stream-source-*-windows-installer.exe` and run it.

**Manual:**
1. Download `live-stream-source-*-windows-x64.zip`.
2. Unzip to your OBS folder (`C:\Program Files\obs-studio\`).
3. Restart OBS.

### macOS

**Installer (PKG):**
1. Download `live-stream-source-*-macos-installer.zip` and run the `.pkg`.
2. Restart OBS.

> **Note:** The package is unsigned. If macOS blocks it, go to **System Settings → Privacy & Security → Open Anyway**.

**Manual:**
1. Download `live-stream-source-*-macos-universal.zip`.
2. Drop `live-stream-source.plugin` into `~/Library/Application Support/obs-studio/plugins/`.
3. Restart OBS.

### Linux

1. Download `live-stream-source-*-x86_64-linux-gnu.zip`.
2. Extract to `~/.config/obs-studio/plugins/`.
3. Restart OBS.

> Flatpak: `~/.var/app/org.obsproject.Studio/config/obs-studio/plugins/`  
> Snap: `~/snap/obs-studio/current/.config/obs-studio/plugins/`

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
| **Stream URL** | URL to the live stream |
| **Stream Type** | Standard, Amazon IVS, HLS, or WHEP |
| **Hardware Acceleration** | Enable GPU decoding |
| **Low Bitrate Threshold** | Bitrate (kbps) below which the low-bitrate source is shown |
| **Low Bitrate Overlay Source** | Source shown when bitrate drops below threshold |
| **Disconnect Fallback Source** | Source shown when stream disconnects |
| **Loading Overlay Source** | Source shown during initial connection |
| **WHEP Bearer Token** | Auth token for WHEP endpoints |

### Buttons

| Button | Action |
|--------|--------|
| **Refresh** | Reconnect to the stream |
| **Skip to Live Edge** | Re-anchor immediately to the live edge |
| **Open Statistics Dashboard** | Opens the HTML stats dashboard in a browser |
| **Show Statistics Window** | Opens the built-in Qt stats window |
| **Add Stats Overlay** | Adds a browser source overlay to the current scene |

## WebSocket Stats Server

The plugin runs a WebSocket server on port `4477` (configurable via **Tools → Live Stream Source Dashboard → Settings**) that broadcasts JSON stats and accepts JSON commands.

### JSON Stats Format

```json
{
  "sources": {
    "Source Name": {
      "connected": true,
      "width": 1920,
      "height": 1080,
      "kbps": 4500,
      "fps": 30.0,
      "latency_ms": 120,
      "total_decoded": 18000,
      "dropped_frames": 0,
      "dropped_decode": 0,
      "dropped_latency": 0,
      "total_bytes_video": 524288000,
      "total_bytes_audio": 10485760,
      "reconnects": 0,
      "catchup_active": false,
      "catchup_state": "Normal",
      "catchup_tempo": 1.000,
      "catchup_drift_ms": 0,
      "hw_accel": true,
      "video_codec": "h264",
      "audio_codec": "aac",
      "stream_delay_ms": 1800,
      "uptime_s": 3600
    }
  }
}
```

### Binary Preview Protocol

When preview is enabled, binary frames are sent alongside JSON stats. The first byte is a type tag:

| Tag | Type | Format |
|-----|------|--------|
| `0x01` | Video frame | JPEG (854×480, ~10 fps) |
| `0x02` | Audio chunk | Float32 mono PCM, 16 kHz |

### Remote Commands

Send a JSON object to control a source:

```json
{ "command": "blur", "source": "My Stream Source" }
```

| Command | Effect |
|---------|--------|
| `preview_video_on` / `preview_video_off` | Start / stop JPEG video frames |
| `preview_audio_on` / `preview_audio_off` | Start / stop PCM audio chunks |
| `mute` / `unmute` | Toggle mute overlay on the preview |
| `blur` / `unblur` | Toggle privacy blur on the preview |
| `live_to_edge` | Skip to the live edge immediately |
| `refresh` | Reconnect the stream |

## Building from Source

### Requirements

- CMake 3.16+
- C++17 compiler
- OBS Studio source/SDK
- FFmpeg dev libraries ([ffmpeg-builds](https://github.com/btbn/ffmpeg-builds/releases))
- Qt6 (Widgets, Core, Network)
- SoundTouch, libdatachannel, MbedTLS — fetched automatically via CMake FetchContent

### Build

```bash
cmake -B build \
  -DOBS_SOURCE_DIR="path/to/obs-studio" \
  -DOBS_BIN_DIR="path/to/obs-studio/bin/64bit" \
  -DFFMPEG_DIR="path/to/ffmpeg" \
  -DENABLE_WHEP=ON
cmake --build build --config Release
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_WHEP` | `ON` | Enable WHEP (WebRTC) receiver |
| `OBS_SOURCE_DIR` | — | Path to OBS Studio source tree |
| `OBS_BIN_DIR` | — | Path to OBS binary directory |
| `FFMPEG_DIR` | — | Path to FFmpeg dev build |

## Project Structure

```
src/
├── core/
│   ├── plugin-main.cpp/h          # Plugin entry point
│   ├── live-stream-source.cpp/h   # Main source plugin
│   ├── catchup-orchestrator.cpp/h # Catch-up state machine
│   ├── plugin-settings.cpp/h      # Settings UI (Qt)
│   └── common.h                   # Shared constants & types
├── media/
│   ├── stream-demuxer.cpp/h       # FFmpeg stream demuxing
│   ├── video-decoder.cpp/h        # Video decoding (SW/HW)
│   ├── audio-decoder.cpp/h        # Audio decoding + time-stretching
│   ├── audio-time-stretcher.cpp/h # SoundTouch wrapper
│   └── frame-queue.cpp/h          # Thread-safe frame queue
├── protocols/
│   ├── ll-hls/                    # Amazon IVS LL-HLS client
│   └── whep/                      # WHEP WebRTC client
├── utils/
│   ├── buffer-manager.cpp/h       # Buffer depth monitoring
│   ├── tempo-ramper.cpp/h         # Smooth tempo transitions
│   └── bitrate-monitor.cpp/h      # Bitrate tracking
├── network/
│   ├── ws-stats-server.cpp/h      # WebSocket stats + binary preview server
│   └── preview-encoder.cpp/h      # JPEG/PCM preview encoder
└── ui/
    └── dashboard-dialog.cpp/h     # Stats dashboard (Qt)
```

## Disclaimer

This is an independent, third-party plugin not affiliated with OBS Studio, the OBS Project, Streamlabs, Amazon (IVS), or any other streaming platform. It connects to publicly available streaming protocols using standard methods and does not modify or reverse-engineer any backend service. **Use at your own risk.**

## AI Disclosure

To comply with the [OBS Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/): the initial architecture was prototyped with AI assistance. The codebase has since been fully reviewed, refactored, and is now maintained through standard manual engineering.

The project is offered as-is. For issues, please [open a GitHub issue](https://github.com/xacnio/live-stream-source/issues).

## Third-Party Libraries

### SoundTouch

- **Purpose**: Audio time-stretching for catch-up playback
- **Version**: 2.3.3 — [codeberg.org/soundtouch/soundtouch](https://codeberg.org/soundtouch/soundtouch)
- **License**: LGPL 2.1 — statically linked; source available at the link above

### MbedTLS

- **Purpose**: TLS/DTLS cryptography for the WHEP transport layer
- **Version**: v3.6.2 — [github.com/Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls)
- **License**: Apache 2.0 — statically linked

### libdatachannel

- **Purpose**: WebRTC implementation for the WHEP receiver
- **Version**: v0.22.2 — [github.com/paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- **License**: MPL 2.0 — statically linked with bundled libsrtp, libjuice, usrsctp

## License

[GNU General Public License v2.0](LICENSE)
