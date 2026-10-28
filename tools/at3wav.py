#!/usr/bin/env python3
"""Decode the game's music (ATRAC3 in RIFF, sound/music/musTrackN.at3) to 16-bit WAV.

Usage: python3 -I tools/at3wav.py assets/files/sound/music assets/music

Needs `ffmpeg` on PATH (e.g. `nix shell nixpkgs#ffmpeg-headless -c python3 -I ...`),
which has the ATRAC3 decoder; the engine then streams plain WAV. Also writes
music.json with each track's loop (from the file's smpl chunk, in decoded samples:
the encoder delay in the fact chunk comes off) and the playlists BOOT.BIN uses
(0x8850848: state 0 plays track 10, state 1 cycles 1-5, state 2 cycles 6-10;
notes/formats.md).
"""
import json
import os
import shutil
import struct
import subprocess
import sys


def chunks(data):
    i = 12
    while i + 8 <= len(data):
        cid, n = data[i:i + 4], struct.unpack('<I', data[i + 4:i + 8])[0]
        yield cid, data[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)


def info(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError(f'{path}: not a RIFF/WAVE file')
    out = {'samples': 0, 'delay': 0, 'loop': None}
    for cid, body in chunks(data):
        if cid == b'fact' and len(body) >= 8:
            out['samples'], out['delay'] = struct.unpack('<II', body[:8])
        elif cid == b'smpl' and len(body) >= 60 and struct.unpack('<I', body[28:32])[0] >= 1:
            start, end = struct.unpack('<II', body[44:52])
            out['loop'] = [max(0, start - out['delay']), max(0, end - out['delay'])]
    return out


def main(src, dst):
    if not shutil.which('ffmpeg'):
        sys.exit('at3wav: ffmpeg not on PATH (try: nix shell nixpkgs#ffmpeg-headless -c python3 -I tools/at3wav.py ...)')
    os.makedirs(dst, exist_ok=True)
    tracks = {}
    for name in sorted(os.listdir(src)):
        if not name.lower().endswith('.at3'):
            continue
        meta = info(os.path.join(src, name))
        wav = os.path.splitext(name)[0] + '.wav'
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', os.path.join(src, name),
                        '-c:a', 'pcm_s16le', '-ar', '44100', '-ac', '2', os.path.join(dst, wav)], check=True)
        tracks[os.path.splitext(name)[0]] = {'file': wav, 'samples': meta['samples'], 'loop': meta['loop']}
        print(f'{name}: {meta["samples"] / 44100:.1f} s, loop {meta["loop"]}')
    doc = {'source': 'GAMEDATA sound/music, decoded by tools/at3wav.py',
           'playlists': {'title': ['musTrack10'],
                         'quiet': ['musTrack1', 'musTrack2', 'musTrack3', 'musTrack4', 'musTrack5'],
                         'battle': ['musTrack6', 'musTrack7', 'musTrack9', 'musTrack10', 'musTrack8']},
           'tracks': tracks}
    json.dump(doc, open(os.path.join(dst, 'music.json'), 'w'), indent=1)
    print(f'{len(tracks)} tracks -> {dst}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
