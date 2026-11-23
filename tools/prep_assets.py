#!/usr/bin/env python3
"""Make app_data/ for the Vita port from your own copy of the iPhone game.

    python3 tools/prep_assets.py "GWarsTouch_v1.2.0_os30.ipa" app_data

The .ipa is GWars:Touch 1.2.0 (bundle id com.activision.gw3pad). Only its
sound is used; everything you see is drawn by this project's own code.

  app_data/sfx/<name>.wav     sound effects (the .caf files are plain PCM:
                              unpacked with Python alone)
  app_data/music/<name>.aac   music as it is in the app (the Vita decodes it in hardware)
  app_data/music/<name>.ogg   the same music as Ogg Vorbis for the PC build (needs ffmpeg)
  app_data/levels.txt         the Sequence level scripts, decoded from Data.bin
  app_data/manifest.txt       what was made; the game looks for this file

Files already made are kept, so an interrupted run can be restarted.
Nothing here is ever committed: app_data/ is the game's sound, keep it private.
"""
import argparse
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

BUNDLE_ID = 'com.activision.gw3pad'
APP = 'Payload/GWTouch.app/'


def caf_to_wav(data):
    """A CAF file holding linear PCM -> the bytes of a WAV file (None if it is
    some other codec)."""
    if data[:4] != b'caff':
        return None
    pos, desc, pcm = 8, None, None
    while pos + 12 <= len(data):
        ctype = data[pos:pos + 4]
        size = struct.unpack('>q', data[pos + 4:pos + 12])[0]
        body = data[pos + 12:pos + 12 + size] if size >= 0 else data[pos + 12:]
        if ctype == b'desc':
            rate, fmt, flags, bpp, fpp, ch, bits = struct.unpack('>d4sIIIII', body[:32])
            desc = (rate, fmt, flags, ch, bits)
        elif ctype == b'data':
            pcm = body[4:]                   # 4 bytes of edit count first
        if size < 0:
            break
        pos += 12 + size
    if not desc or pcm is None:
        return None
    rate, fmt, flags, ch, bits = desc
    if fmt != b'lpcm' or bits not in (8, 16) or flags & 1:   # integer PCM only
        return None
    if bits == 16 and not (flags & 2):       # big-endian samples: swap to little
        pcm = b''.join(pcm[i + 1:i + 2] + pcm[i:i + 1] for i in range(0, len(pcm) - 1, 2))
    block = ch * bits // 8
    pcm = pcm[:len(pcm) // block * block]
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +
            struct.pack('<IHHIIHH', 16, 1, ch, int(rate), int(rate) * block, block, bits) +
            b'data' + struct.pack('<I', len(pcm)) + pcm)


def music_name(fn):
    """'Retro Evolved.wav.aac' -> 'retro_evolved'"""
    base = os.path.basename(fn).split('.wav')[0]
    return base.strip().lower().replace(' ', '_')


def flask_decode(b):
    """XtFlaskDecodeStrings (0x00088f50 in the game): a running key."""
    k, out = 0xcc, bytearray()
    for i, x in enumerate(b):
        o = (k - (x ^ 0xb3)) & 0xff
        if o == 0:
            break
        out.append(o)
        k = (k + o - i) & 0xff
    return bytes(out)


def level_scripts(data):
    """The Sequence level scripts (DbLevelScript records) out of Data.bin: each is an
    encoded string with the level number 8 bytes before it."""
    found = {}
    keys = (b'circle ', b'line ', b'scatter ', b'clump ', b'wait ')
    for i in range(16, len(data) - 8):
        head = flask_decode(data[i:i + 10])
        if not head.startswith(keys):
            continue
        level = struct.unpack_from('<I', data, i - 8)[0]
        if level >= 32 or (level + 1) in found:
            continue
        text = flask_decode(data[i:i + 8192]).decode('latin-1')
        if '(' in text[:24]:
            found[level + 1] = text
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ipa', help='GWarsTouch_v1.2.0_os30.ipa (your own copy)')
    ap.add_argument('out', help='output folder, e.g. app_data')
    ap.add_argument('--ffmpeg', default=shutil.which('ffmpeg'), help='ffmpeg, for the music')
    ap.add_argument('--builtin-vorbis', action='store_true',
                    help="use ffmpeg's own Vorbis encoder straight away (it is tried anyway when libvorbis is missing)")
    a = ap.parse_args()

    z = zipfile.ZipFile(a.ipa)
    names = z.namelist()
    if APP + 'Info.plist' not in names:
        sys.exit('This does not look like GWars:Touch (no %sInfo.plist).' % APP)
    info = plistlib.loads(z.read(APP + 'Info.plist'))
    bid, ver = info.get('CFBundleIdentifier'), info.get('CFBundleVersion')
    print('%s %s' % (bid, ver))
    if bid != BUNDLE_ID:
        print('warning: expected %s; carrying on' % BUNDLE_ID)

    sfx_dir = os.path.join(a.out, 'sfx')
    mus_dir = os.path.join(a.out, 'music')
    os.makedirs(sfx_dir, exist_ok=True)
    os.makedirs(mus_dir, exist_ok=True)

    # ---- sound effects ----
    cafs = sorted(n for n in names if n.startswith(APP) and n.endswith('.caf') and n.count('/') == 2)
    made = skipped = bad = 0
    for n in cafs:
        base = os.path.basename(n).split('.wav')[0].split('.caf')[0]
        dst = os.path.join(sfx_dir, base + '.wav')
        if os.path.exists(dst):
            skipped += 1
            continue
        wav = caf_to_wav(z.read(n))
        if wav is None:
            print('  skipped %s: not plain PCM' % base)
            bad += 1
            continue
        with open(dst, 'wb') as f:
            f.write(wav)
        made += 1
    print('%d sound effects (%d new, %d already there, %d skipped)' % (made + skipped, made, skipped, bad))

    # ---- Sequence levels ----
    if APP + 'Data.bin' in names:
        levels = level_scripts(z.read(APP + 'Data.bin'))
        with open(os.path.join(a.out, 'levels.txt'), 'w') as f:
            for n in sorted(levels):
                f.write('level %d\n%s\n' % (n, levels[n].replace('\r', '')))
        print('%d Sequence level scripts' % len(levels))

    # ---- music ----
    aacs = sorted(n for n in names if n.startswith(APP) and n.endswith('.aac') and n.count('/') == 2)
    tracks = []
    # the original AAC, as it is: the Vita plays it on its hardware decoder
    # (src/vita_aac.c); the Ogg copies below are for the PC build and a fallback
    for n in aacs:
        dst = os.path.join(mus_dir, music_name(n) + '.aac')
        if not os.path.exists(dst):
            with open(dst, 'wb') as f:
                f.write(z.read(n))
    print('%d music tracks kept as AAC (for the Vita hardware decoder)' % len(aacs))
    if not a.ffmpeg:
        print('ffmpeg not found: no Ogg music for the PC build this time (the Vita plays the AAC)')
    else:
        for n in aacs:
            name = music_name(n)
            dst = os.path.join(mus_dir, name + '.ogg')
            tracks.append(name)
            if os.path.exists(dst):
                continue
            with tempfile.TemporaryDirectory() as td:
                src = os.path.join(td, 'in.aac')
                with open(src, 'wb') as f:
                    f.write(z.read(n))
                tmp = dst + '.part'
                # libvorbis if this ffmpeg has it; otherwise ffmpeg's own Vorbis
                # encoder, which only takes stereo (the tracks are mono: copied to both)
                encoders = [['-c:a', 'libvorbis', '-q:a', '4'],
                            ['-ac', '2', '-c:a', 'vorbis', '-strict', '-2', '-q:a', '5']]
                if a.builtin_vorbis:
                    encoders = encoders[1:]
                for enc in encoders:
                    r = subprocess.run([a.ffmpeg, '-v', 'error', '-y', '-i', src, '-vn', '-ar', '44100'] +
                                       enc + ['-f', 'ogg', tmp], stderr=subprocess.PIPE, text=True)
                    if r.returncode == 0:
                        break
                if r.returncode != 0:
                    print(r.stderr.strip()[-300:])
                    print('  ffmpeg failed on %s' % name)
                    tracks.pop()
                    continue
                os.replace(tmp, dst)
            print('  music: %s' % name)
        print('%d music tracks' % len(tracks))

    with open(os.path.join(a.out, 'manifest.txt'), 'w') as f:
        f.write('gwvita 1\nsource %s %s\nsfx %d\nmusic %s\n' % (bid, ver, made + skipped, ' '.join(tracks)))
    print('done: %s' % os.path.abspath(a.out))


if __name__ == '__main__':
    main()
