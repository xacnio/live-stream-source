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
- **Dropout Recovery** — A jitter buffer rides out network hiccups. Under 2 s of delay, playback drifts back at up to 1.10x with pitch-corrected audio. Above that it cuts to a keyframe near the live edge, or keeps all content if you prefer. There is no visible fast-forward.
- **Stable A/V Sync** — Audio drives one shared clock for both streams, so sync holds through stalls, cuts and reconnects on Standard streams.
- **Remote Panel** — Switch scenes, start or stop the stream and adjust audio from your own panel, with program preview and audio meters. Anything other than a program on the same machine needs the access key.
- **Stream Sync** — A dock for lining sources up: pick one main source and its followers, then let them follow its delay or keep fixed offsets. Sync Now lines them up on a clap or any sound they share. Changing a delay lands at once, up to 10 seconds in either direction.
- **Auto-Reconnect** — Configurable retry on connection failure.
- **Built-in Fallbacks** — Each source shows its own text card, image, video or web page when its stream drops, runs low or is loading, sized and placed inside the source, with its own font, colours, background and in/out animations. Nine HTML templates ship with the plugin. Nothing to add to the scene. A scene source can still be used instead.
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
| **Low Bitrate Threshold** | Bitrate (kbps) below which the low-bitrate fallback is shown |
| **Fallback Settings** | Opens a window with one tab each for disconnect, low bitrate and loading |
| **WHEP Bearer Token** | Auth token for WHEP endpoints |

### Buttons

| Button | Action |
|--------|--------|
| **Refresh** | Reconnect to the stream |
| **Skip to Live Edge** | Re-anchor immediately to the live edge |
| **Open Statistics Dashboard** | Opens the HTML stats dashboard in a browser |
| **Show Statistics Window** | Opens the built-in Qt stats window |
| **Add Stats Overlay** | Adds a browser source overlay to the current scene |

Refresh and Skip to Live Edge also appear in the bar under the preview while the source is selected.

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
      "target_latency_ms": 500,
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

Browser pages have to send `{ "auth": "<access key>" }` first, even on the same machine. The dashboard and overlay pages the plugin writes to disk prove themselves with a local token. Programs on the same machine are let in without one, and connections from other machines need the access key.

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
│   ├── live-stream-playout.cpp    # Playout engine threads (Standard streams)
│   ├── plugin-settings.cpp/h      # Settings UI (Qt)
│   ├── fallback-layer.cpp/h       # Fallbacks drawn by the source itself
│   ├── fallback-fit.h             # Placement and animation maths
│   ├── fallback-template.h        # Fills in the HTML templates
│   ├── fallback-settings.h        # Fallback setting names
│   └── common.h                   # Shared constants & types
├── media/
│   ├── stream-demuxer.cpp/h       # FFmpeg stream demuxing
│   ├── video-decoder.cpp/h        # Video decoding (SW/HW)
│   ├── audio-decoder.cpp/h        # Audio decoding + time-stretching
│   ├── audio-time-stretcher.cpp/h # SoundTouch wrapper
│   ├── privacy-blur.cpp/h         # Gaussian privacy blur
│   └── frame-queue.cpp/h          # Thread-safe frame queue
├── protocols/
│   ├── ll-hls/                    # Amazon IVS LL-HLS client
│   └── whep/                      # WHEP WebRTC client
├── playout/
│   ├── packet-buffer.cpp/h        # Jitter buffer with keyframe lookup
│   ├── playout-clock.h            # Shared media-to-OBS time mapping
│   ├── recovery-policy.cpp/h      # Rate and cut decisions
│   ├── sound-match.cpp/h          # Finds the offset between two sources by sound
│   └── sync-registry.h            # Delay matching between sources
├── utils/
│   └── bitrate-monitor.cpp/h      # Bitrate tracking
├── network/
│   ├── ws-stats-server.cpp/h      # WebSocket stats + binary preview server
│   ├── ws-protocol.h              # Request parsing and access rules
│   ├── program-preview.cpp/h      # OBS program preview
│   └── preview-encoder.cpp/h      # JPEG/PCM preview encoder
└── ui/
    ├── dashboard-dialog.cpp/h     # Stats dashboard (Qt)
    ├── sync-panel.cpp/h           # Stream Sync dock
    ├── fallback-dialog.cpp/h      # Fallback settings window
    ├── context-buttons.cpp/h      # Buttons in the bar under the preview
    ├── url-field-guard.cpp/h      # Hides a filled-in stream URL
    ├── obs-remote.cpp/h           # Remote commands
    ├── audio-meters.cpp/h         # Audio levels for the panel
    └── sync-notice.cpp/h          # On-screen status notices
tests/
└── playout-logic-test.cpp         # Unit tests
tools/
└── stream-lab/                    # Two test streams with a panel to break them
```

[Stream Lab](tools/stream-lab/README.md) sends two RTMP streams of one live event, with buttons for stalls, loss, disconnects, delay and claps, for testing sync and recovery in OBS.

## Disclaimer

This is an independent, third-party plugin not affiliated with OBS Studio, the OBS Project, Streamlabs, Amazon (IVS), or any other streaming platform. It connects to publicly available streaming protocols using standard methods and does not modify or reverse-engineer any backend service. **Use at your own risk.**

## AI Disclosure

To comply with the [OBS Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/): the initial architecture was prototyped with AI assistance. The codebase has since been fully reviewed, refactored, and is now maintained through standard manual engineering.

The project is offered as-is. For issues, please [open a GitHub issue](https://github.com/xacnio/live-stream-source/issues).

## Third-Party Libraries

### SoundTouch

- **Purpose**: Pitch-corrected audio while drifting back to the target latency
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
