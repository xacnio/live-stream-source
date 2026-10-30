# Stream Lab

Two RTMP streams of one live event, for testing sync and recovery in OBS. Both follow the same wall clock, so a camera started later joins at the moment the other has reached, as a second camera on the same speaker would.

Every picture carries the timeline counter, a frame number and a sweeping bar. Every 5 seconds a white square flashes as a 1 kHz tick plays, so drift between picture and sound can be read directly, and a difference between the two cameras shows up in their counters.

Camera B is the "phone" of the pair: same timing, duller colours and a narrower, quieter sound.

## Setup

Needs Python 3 and FFmpeg.

```
python lab.py render
python lab.py serve
```

`render` downloads the two source files and builds `media/camera-a.flv` and `media/camera-b.flv`. It only has to run once. Then open http://127.0.0.1:8099, enter the two publish URLs and press Start. URLs are saved to `config.json`, which stays out of git because a URL can hold a stream key.

## What the buttons do

| Button | What the receiver sees |
|---|---|
| **Stall** | Nothing arrives, then the backlog comes in a burst, like a TCP link that recovered |
| **Loss** | That part of the stream never arrives, picture resumes at the next keyframe |
| **Disconnect** | The publish ends, then a new one starts at the live point with timestamps from zero |
| **Low bitrate** | About 950 kbps: the picture moves for a moment each second while sound carries on |
| **Jitter** | Data arrives in uneven clumps |
| **Delay** | Holds a stream back from the live point, like a slower path. Lowering it sends a burst |
| **Clap** | Three claps within 2.3 seconds on both cameras. A camera running late hears them late, like a real clap in the room |

To try sound sync, give one camera a delay, start listening in OBS, then press Clap. The event log shows each action with the timeline counter, to compare against the picture in OBS.

## Sources

- *Astronaut Moments: Loral O'Hara*, NASA Johnson, public domain. [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Astronaut_Moments-_Loral_O%E2%80%99Hara.webm)
- *Scheming Weasel (faster)* by Kevin MacLeod, CC BY 4.0. [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Kevin_MacLeod_-_Scheming_Weasel_(faster).wav)
