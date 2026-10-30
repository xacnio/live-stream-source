#!/usr/bin/env python3
"""Two RTMP streams of one live event, with a panel to break them on purpose.

Both streams follow one wall clock, so a stream started later joins at the point
the other one has reached, the way a second camera on the same person would.

  python lab.py fetch    download the talk and the song into media/src
  python lab.py render   build media/camera-a.flv and media/camera-b.flv
  python lab.py serve    panel on http://127.0.0.1:8099
"""
import bisect
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
MEDIA = os.path.join(HERE, 'media')
SOURCE = os.path.join(MEDIA, 'src')
CONFIG = os.environ.get('STREAM_LAB_CONFIG', os.path.join(HERE, 'config.json'))
PANEL = os.path.join(HERE, 'panel.html')
PORT = int(os.environ.get('STREAM_LAB_PORT', 8099))

FFMPEG = shutil.which('ffmpeg') or r'C:\ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe'
FONT = 'C\\:/Windows/Fonts/consola.ttf'

SOURCES = {
    'talk.webm': 'https://upload.wikimedia.org/wikipedia/commons/8/84/Astronaut_Moments-_Loral_O%E2%80%99Hara.webm',
    'song.wav': 'https://upload.wikimedia.org/wikipedia/commons/9/9e/Kevin_MacLeod_-_Scheming_Weasel_%28faster%29.wav',
}

WIDTH, HEIGHT, FPS = 1280, 720, 30
TALK_SECONDS = 364.7
MARK_EVERY = 5        # a flash and a tick land together, so A/V drift can be read off them
BURST_SPEED = 5       # how fast a backlog drains after a stall, as a multiple of real time
THIN_KEEP_FRAMES = 6  # low bitrate sends this many frames of each second, about 950 kbps
PIPE_BYTES = 128 * 1024

# The clap tracks are the same sound with three claps in every cycle. A clap swaps
# one cycle in on both cameras, by timeline position, so a camera running late hears
# it late, as it would a real one. Uneven, like hands.
CLAP_CYCLE_MS = 2000
CLAP_TIMES = (0.0, 0.55, 1.05)
CLAP_NOTICE_MS = 300


def fetch():
    os.makedirs(SOURCE, exist_ok=True)
    for name, url in SOURCES.items():
        target = os.path.join(SOURCE, name)
        if os.path.exists(target):
            continue
        print('downloading', name)
        request = urllib.request.Request(url, headers={'User-Agent': 'stream-lab/1.0'})
        with urllib.request.urlopen(request) as reply, open(target, 'wb') as out:
            shutil.copyfileobj(reply, out)


def video_filter(label, tint):
    def text(content, position, size, color):
        return f"drawtext=fontfile='{FONT}':text='{content}':{position}:fontsize={size}:fontcolor={color}"

    return ';'.join([
        f'color=c=white:s=14x26:r={FPS}[bar]',
        f'[0:v]scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,'
        f'pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2,fps={FPS},format=yuv420p,{tint},'
        f'drawbox=x=0:y=0:w={WIDTH}:h=66:color=black@0.6:t=fill,'
        + text(label, 'x=20:y=15', 34, 'white') + ','
        + text('%{pts\\:hms}', 'x=(w-tw)/2:y=10', 46, 'yellow') + ','
        + text('F%{frame_num}', 'x=w-tw-20:y=17', 30, 'white') + ','
        f"drawbox=x={WIDTH - 100}:y=82:w=80:h=80:color=white:t=fill:enable='lt(mod(t,{MARK_EVERY}),0.1)'[pic]",
        f"[pic][bar]overlay=x='(W-w)*mod(t,10)/10':y=H-34:shortest=1,format=yuv420p[v]",
    ])


def audio_filter(tone, claps=False):
    cycle = f'mod(t,{CLAP_CYCLE_MS / 1000:g})'
    hits = '+'.join(f'gte({cycle},{at})*exp(-70*({cycle}-{at}))' for at in CLAP_TIMES)
    parts = [
        f"aevalsrc='0.45*sin(2*PI*1000*t)*lt(mod(t,{MARK_EVERY}),0.08)':s=48000:c=stereo:d={TALK_SECONDS}[tick]",
        '[0:a]aresample=48000,volume=1.5[speech]',
        '[1:a]aresample=48000,volume=0.25[song]',
    ]
    if claps:
        parts.append(f"aevalsrc='(2*random(0)-1)*({hits})':s=48000:c=stereo:d={TALK_SECONDS},"
                     'highpass=f=700,lowpass=f=5000,volume=2.2[claps]')
    mix = '[speech][song][tick]' + ('[claps]' if claps else '')
    parts.append(f'{mix}amix=inputs={4 if claps else 3}:duration=first:normalize=0,alimiter=limit=0.95{tone}[a]')
    return ';'.join(parts)


def render():
    fetch()
    # Same timing, different look and sound: camera B is the phone in the pair.
    cameras = [
        ('camera-a.flv', 'CAMERA A', 'eq=saturation=1', ''),
        ('camera-b.flv', 'CAMERA B', 'hue=s=0.7', ',highpass=f=180,lowpass=f=7000,volume=0.7'),
    ]
    for name, label, tint, tone in cameras:
        target = os.path.join(MEDIA, name)
        print('rendering', name)
        command = [
            FFMPEG, '-hide_banner', '-y', '-v', 'error', '-stats',
            '-i', os.path.join(SOURCE, 'talk.webm'),
            '-stream_loop', '-1', '-i', os.path.join(SOURCE, 'song.wav'),
            '-filter_complex', video_filter(label, tint) + ';' + audio_filter(tone),
            '-map', '[v]', '-map', '[a]', '-t', str(TALK_SECONDS),
            '-c:v', 'libx264', '-preset', 'veryfast', '-threads', '3', '-b:v', '2500k',
            '-maxrate', '3000k', '-bufsize', '3000k', '-g', str(FPS), '-keyint_min', str(FPS),
            '-sc_threshold', '0', '-bf', '0', '-pix_fmt', 'yuv420p',
            '-c:a', 'aac', '-b:a', '128k', '-ar', '48000', '-ac', '2',
            '-flvflags', 'no_duration_filesize', '-f', 'flv', target + '.part',
        ]
        priority = subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == 'nt' else 0
        if not os.path.exists(target):
            subprocess.run(command, check=True, creationflags=priority)
            os.replace(target + '.part', target)

        target = os.path.join(MEDIA, name.replace('.flv', '-claps.flv'))
        if os.path.exists(target):
            continue
        print('rendering', os.path.basename(target))
        command = [
            FFMPEG, '-hide_banner', '-y', '-v', 'error', '-stats', '-vn',
            '-i', os.path.join(SOURCE, 'talk.webm'),
            '-stream_loop', '-1', '-i', os.path.join(SOURCE, 'song.wav'),
            '-filter_complex', audio_filter(tone, claps=True),
            '-map', '[a]', '-t', str(TALK_SECONDS),
            '-c:a', 'aac', '-b:a', '128k', '-ar', '48000', '-ac', '2',
            '-flvflags', 'no_duration_filesize', '-f', 'flv', target + '.part',
        ]
        subprocess.run(command, check=True, creationflags=priority)
        os.replace(target + '.part', target)


class Master:
    """A rendered file as a list of FLV tags that can loop forever."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self.claps = None   # the same sound with claps in it, see CLAP_CYCLE_MS
        self.setup = []     # metadata and codec headers, sent once per connection
        self.tags = []      # (ms, is_video, is_key, start, end)
        at = struct.unpack('>I', self.data[5:9])[0] + 4
        while at + 11 <= len(self.data):
            kind = self.data[at]
            size = int.from_bytes(self.data[at + 1:at + 4], 'big')
            ms = int.from_bytes(self.data[at + 4:at + 7], 'big') | self.data[at + 7] << 24
            body = at + 11
            is_video = kind == 9
            is_header = kind == 18 or (kind in (8, 9) and self.data[body + 1] == 0)
            is_key = is_video and self.data[body] >> 4 == 1
            if is_header:
                self.setup.append((kind, body, body + size))
            elif kind in (8, 9):
                self.tags.append((ms, is_video, is_key, at, body + size))
            at = body + size + 4
        self.times = [tag[0] for tag in self.tags]
        self.length_ms = self.times[-1] + 1000 // FPS
        self.byte_rate = len(self.data) / (self.length_ms / 1000)

    def tag_bytes(self, index, ms):
        _, _, _, start, end = self.tags[index]
        return self.packet(self.data[start], self.data[start + 11:end], ms)

    def clap_bytes(self, ms, out_ms):
        claps = self.claps
        index = min(bisect.bisect_left(claps.times, ms), len(claps.tags) - 1)
        _, _, _, start, end = claps.tags[index]
        return self.packet(claps.data[start], claps.data[start + 11:end], out_ms)

    @staticmethod
    def packet(kind, body, ms):
        ms = int(ms) & 0xFFFFFFFF
        head = bytes([kind]) + len(body).to_bytes(3, 'big') + (ms & 0xFFFFFF).to_bytes(3, 'big')
        head += bytes([ms >> 24]) + b'\0\0\0'
        return head + body + struct.pack('>I', len(head) + len(body))

    def setup_bytes(self):
        return b''.join(self.packet(kind, self.data[start:end], 0) for kind, start, end in self.setup)

    def key_at_or_after(self, position_ms):
        """(tag index, lap) of the first keyframe at this point on the endless timeline."""
        lap = int(position_ms // self.length_ms)
        index = bisect.bisect_left(self.times, position_ms - lap * self.length_ms)
        while True:
            if index >= len(self.tags):
                index, lap = 0, lap + 1
            if self.tags[index][2]:
                return index, lap
            index += 1


class Clock:
    """The one live event both streams belong to."""

    def __init__(self):
        self.started = None
        self.clap_from = self.clap_to = 0.0

    def clap(self, length_ms):
        """The next whole cycle on the timeline, which is where the track has its claps."""
        position = self.position_ms()
        local = position % length_ms
        cycle = math.ceil((local + CLAP_NOTICE_MS) / CLAP_CYCLE_MS) * CLAP_CYCLE_MS
        self.clap_from = position - local + cycle
        self.clap_to = self.clap_from + CLAP_CYCLE_MS
        return cycle

    def in_clap(self, position_ms):
        return self.clap_from <= position_ms < self.clap_to

    def position_ms(self):
        return 0.0 if self.started is None else (time.monotonic() - self.started) * 1000

    def start(self):
        if self.started is None:
            self.started = time.monotonic()


def stamp(ms):
    seconds = int(ms // 1000)
    return f'{seconds // 60:02d}:{seconds % 60:02d}.{int(ms % 1000):03d}'


class Stream:
    def __init__(self, name, master, clock, log):
        self.name, self.master, self.clock, self.log = name, master, clock, log
        self.url = ''
        self.wanted = False
        self.delay_ms = 0
        self.stall_until = self.loss_until = self.thin_until = self.jitter_until = 0.0
        self.offline_until = 0.0
        self.next_jitter = 0.0
        self.publisher = None
        self.state = 'stopped'
        self.failure = ''
        self.sent_ms = 0.0
        self.window = deque()
        threading.Thread(target=self.run, daemon=True).start()

    def note(self, message):
        self.log(f'{self.name}: {message}')

    # What the panel can do. Times are from now.
    def start(self, url):
        self.url = url
        self.wanted = True
        self.clock.start()

    def stop(self):
        self.wanted = False
        self.close_publisher()

    def stall(self, seconds):
        self.stall_until = time.monotonic() + seconds
        self.note(f'stall {seconds:g} s, the backlog drains afterwards')

    def loss(self, seconds):
        self.loss_until = time.monotonic() + seconds
        self.note(f'loss {seconds:g} s, that part of the stream never arrives')

    def disconnect(self, seconds):
        self.offline_until = time.monotonic() + seconds
        self.close_publisher()
        self.note(f'disconnect {seconds:g} s, then a fresh publish at the live point')

    def thin(self, seconds):
        self.thin_until = time.monotonic() + seconds
        self.note(f'low bitrate {seconds:g} s')

    def jitter(self, seconds):
        self.jitter_until = time.monotonic() + seconds
        self.note(f'jitter {seconds:g} s')

    def set_delay(self, milliseconds):
        self.delay_ms = milliseconds
        self.note(f'delay set to {milliseconds} ms')

    def status(self):
        now = time.monotonic()
        while self.window and now - self.window[0][0] > 2:
            self.window.popleft()
        kbps = sum(size for _, size in self.window) * 8 / 2000
        behind = self.clock.position_ms() - self.sent_ms if self.state == 'live' else 0
        return {'state': self.state, 'url': self.url, 'delay_ms': self.delay_ms, 'kbps': round(kbps),
                'sent': stamp(self.sent_ms % self.master.length_ms), 'behind_ms': round(behind)}

    def close_publisher(self):
        publisher, self.publisher = self.publisher, None
        if publisher:
            try:
                publisher.stdin.close()
            except OSError:
                pass
            publisher.kill()

    def run(self):
        while True:
            if not self.wanted:
                self.state = 'stopped'
                time.sleep(0.05)
                continue
            wait = self.offline_until - time.monotonic()
            if wait > 0:
                self.state = 'offline'
                time.sleep(min(wait, 0.05))
                continue
            try:
                self.state = 'connecting'
                self.publish()
            except (OSError, ValueError):
                publisher = self.publisher
                if self.wanted and self.offline_until < time.monotonic():
                    self.state = 'retrying'
                    self.close_publisher()
                    if publisher:
                        publisher.wait()
                    self.note(f'the server connection ended, retrying: {self.failure or "no reason given"}')
                    time.sleep(2)

    def read_errors(self, publisher):
        for line in publisher.stderr:
            self.failure = line.decode('utf-8', 'replace').strip()[:160]

    def publish(self):
        # The smallest probe that works: FFmpeg holds back whatever it reads while probing.
        self.publisher = subprocess.Popen(
            [FFMPEG, '-hide_banner', '-v', 'error', '-fflags', 'nobuffer', '-f', 'flv', '-probesize', '32768',
             '-analyzeduration', '0', '-i', 'pipe:0', '-c', 'copy', '-f', 'flv', '-flvflags', 'no_duration_filesize',
             self.url],
            stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        self.failure = ''
        threading.Thread(target=self.read_errors, args=(self.publisher,), daemon=True).start()
        pipe = self.publisher.stdin
        master = self.master

        # A real encoder comes up at a keyframe at the current moment, and counts from zero.
        index, lap = master.key_at_or_after(max(self.clock.position_ms() - self.delay_ms, 0) + 50)
        first_ms = master.times[index] + lap * master.length_ms
        pipe.write(b'FLV\x01\x05\0\0\0\x09\0\0\0\0' + master.setup_bytes())
        pipe.flush()
        self.note(f'publishing from {stamp(first_ms % master.length_ms)}')

        waiting_for_key = False
        written = 0
        gop_frame = 0
        thinned = False
        while self.publisher and self.wanted:
            ms, is_video, is_key, _, _ = master.tags[index]
            position = ms + lap * master.length_ms

            overdue = self.wait_until(position)
            if overdue is None:
                return

            now = time.monotonic()
            send = True
            if is_video:
                gop_frame = 0 if is_key else gop_frame + 1
                # Frames lean on the ones before them, so this ends at a keyframe.
                thinned = now < self.thin_until or (thinned and not is_key)
            if now < self.loss_until:
                send = False
                waiting_for_key = True
            elif waiting_for_key:
                waiting_for_key = not (is_video and is_key)
                send = not waiting_for_key
            elif is_video and thinned:
                send = gop_frame < THIN_KEEP_FRAMES

            if send:
                if not is_video and master.claps and self.clock.in_clap(position):
                    packet = master.clap_bytes(ms, position - first_ms)
                else:
                    packet = master.tag_bytes(index, position - first_ms)
                pipe.write(packet)
                pipe.flush()
                written += len(packet)
                # Past what the pipe can hold, FFmpeg is really taking it, so the server is.
                if written > PIPE_BYTES and self.state != 'live':
                    self.state = 'live'
                self.window.append((now, len(packet)))
                self.sent_ms = position
                if overdue > 0.05:
                    time.sleep(len(packet) / (master.byte_rate * BURST_SPEED))

            index += 1
            if index == len(master.tags):
                index, lap = 0, lap + 1

    def wait_until(self, position_ms):
        """Holds a tag until it's due. Returns how late it is, or None to give up."""
        while True:
            if not (self.publisher and self.wanted):
                return None
            now = time.monotonic()
            if now < self.jitter_until and now >= self.next_jitter:
                hold = random.uniform(0.05, 0.6)
                self.stall_until = max(self.stall_until, now + hold)
                self.next_jitter = now + hold + random.uniform(0.1, 0.8)
            if now < self.stall_until:
                time.sleep(min(self.stall_until - now, 0.02))
                continue
            late = (self.clock.position_ms() - self.delay_ms - position_ms) / 1000
            if late >= 0:
                return late
            time.sleep(min(-late, 0.02))


class Lab:
    def __init__(self):
        self.events = deque(maxlen=200)
        self.clock = Clock()
        self.streams = {
            'a': Stream('A', Master(os.path.join(MEDIA, 'camera-a.flv')), self.clock, self.log),
            'b': Stream('B', Master(os.path.join(MEDIA, 'camera-b.flv')), self.clock, self.log),
        }
        for key, stream in self.streams.items():
            path = os.path.join(MEDIA, f'camera-{key}-claps.flv')
            if os.path.exists(path):
                stream.master.claps = Master(path)
        self.config = {'a': 'rtmp://SERVER/live/lab-a', 'b': 'rtmp://SERVER/live/lab-b'}
        if os.path.exists(CONFIG):
            with open(CONFIG, encoding='utf-8') as f:
                self.config.update(json.load(f))

    def log(self, message):
        length = self.streams['a'].master.length_ms if hasattr(self, 'streams') else 1
        self.events.appendleft(f'{stamp(self.clock.position_ms() % length)}  {message}')

    def state(self):
        length = self.streams['a'].master.length_ms
        return {'now': stamp(self.clock.position_ms() % length), 'config': self.config,
                'streams': {key: stream.status() for key, stream in self.streams.items()},
                'events': list(self.events)}

    def act(self, order):
        action = order['action']
        if action == 'urls':
            urls = {key: order[key].strip() for key in 'ab'}
            if not all(url.startswith(('rtmp://', 'rtmps://')) for url in urls.values()):
                raise ValueError('publish addresses start with rtmp://')
            self.config.update(urls)
            with open(CONFIG, 'w', encoding='utf-8') as f:
                json.dump(self.config, f, indent=2)
            return
        if action == 'clap':
            master = self.streams['a'].master
            if not master.claps:
                self.log('claps: run python lab.py render first')
                return
            self.log(f'claps at {stamp(self.clock.clap(master.length_ms))}, on both cameras')
            return
        for key in ('ab' if order.get('stream') == 'both' else order['stream']):
            stream = self.streams[key]
            if action == 'start':
                stream.start(self.config[key])
            elif action == 'stop':
                stream.stop()
            elif action == 'delay':
                stream.set_delay(int(order['value']))
            elif action in ('stall', 'loss', 'disconnect', 'thin', 'jitter'):
                getattr(stream, action)(float(order['value']))
            else:
                raise ValueError(action)


def serve():
    lab = Lab()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, body, kind='application/json'):
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def local_page(self):
            # A site that rebinds its name to this address gets its own name in Host.
            return self.headers.get('Host') in (f'127.0.0.1:{PORT}', f'localhost:{PORT}')

        def do_GET(self):
            if not self.local_page():
                self.reply(403, b'{}')
            elif self.path == '/':
                with open(PANEL, 'rb') as f:
                    self.reply(200, f.read(), 'text/html; charset=utf-8')
            elif self.path == '/api/state':
                self.reply(200, json.dumps(lab.state()).encode())
            else:
                self.reply(404, b'{}')

        def do_POST(self):
            # A page on another site can't send this type without asking first.
            if not self.local_page() or self.path != '/api/act' or \
                    self.headers.get('Content-Type') != 'application/json':
                self.reply(400, b'{}')
                return
            size = int(self.headers.get('Content-Length', 0))
            try:
                lab.act(json.loads(self.rfile.read(size)))
            except (KeyError, ValueError) as error:
                self.reply(400, json.dumps({'error': str(error)}).encode())
                return
            self.reply(200, b'{}')

    print(f'Panel: http://127.0.0.1:{PORT}')
    ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()


if __name__ == '__main__':
    command = sys.argv[1] if len(sys.argv) > 1 else 'serve'
    {'fetch': fetch, 'render': render, 'serve': serve}[command]()
