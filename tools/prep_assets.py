#!/usr/bin/env python3
"""Make app_data/ for the Vita port from your own copy of the iPhone game.

    python3 tools/prep_assets.py "GWarsTouch_v1.2.0_os30.ipa" app_data

The .ipa is GWars:Touch 1.2.0 (bundle id com.activision.gw3pad). Only its
sound is used; everything you see is drawn by this project's own code.

  app_data/sfx/<name>.wav     sound effects (the .caf files are plain PCM:
                              unpacked with Python alone)
  app_data/music/<name>.ogg   music (AAC in the app: needs ffmpeg)
  app_data/levels.txt         the Sequence level scripts, decoded from Data.bin
  app_data/icon0.png          the Vita's home-screen icon, from the app's own icon
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


def trim_box(ffmpeg, path):
    """The square around a picture's content, leaving out a near-black border:
    (w, h, x, y) for ffmpeg's crop, or None. Reads the picture as grey bytes."""
    try:
        info = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-frames:v', '1', '-f', 'rawvideo',
                               '-pix_fmt', 'gray', '-'], capture_output=True, check=True).stdout
        probe = subprocess.run([ffmpeg, '-hide_banner', '-i', path], capture_output=True, text=True).stderr
        import re
        w, h = map(int, re.search(r', (\d{2,5})x(\d{2,5})', probe).groups())
        if len(info) != w * h:
            return None
        row = info[(h // 2) * w:(h // 2 + 1) * w]
        col = info[w // 2::w][:h]
        lit = lambda line: [i for i, v in enumerate(line) if v > 40]
        xs, ys = lit(row), lit(col)
        if not xs or not ys:
            return None
        x0, x1, y0, y1 = xs[0], xs[-1], ys[0], ys[-1]
        side = max(x1 - x0, y1 - y0) + 4
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        side = min(side, w, h)
        x = max(0, min(w - side, cx - side // 2))
        y = max(0, min(h - side, cy - side // 2))
        return side, side, x, y
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ipa', help='GWarsTouch_v1.2.0_os30.ipa (your own copy)')
    ap.add_argument('out', help='output folder, e.g. app_data')
    ap.add_argument('--ffmpeg', default=shutil.which('ffmpeg'), help='ffmpeg, for the music')
    ap.add_argument('--builtin-vorbis', action='store_true',
                    help="use ffmpeg's own Vorbis encoder straight away (it is tried anyway when libvorbis is missing)")
    ap.add_argument('--music-dir', help='a folder of your own, better copies of the tracks (any format ffmpeg '
                    'reads: flac, wav, mp3, m4a...), named like the game\'s: "Pacifism.flac", "Main Menu.mp3", '
                    '"retro_evolved.wav"; these are used instead of the app\'s mono AAC, in stereo')
    ap.add_argument('--redo-icon', action='store_true', help='make app_data/icon0.png again')
    ap.add_argument('--icon', help='a picture of your own for the Vita icon instead of the app\'s (any size; '
                    'the middle square is used; dark borders around a round picture are trimmed)')
    ap.add_argument('--redo-music', action='store_true',
                    help='convert the music again even where app_data already has it (after a quality change)')
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

    # ---- the Vita's home-screen icon: the app's own icon ----
    # iTunesArtwork (512x512) is the same picture as the app's 57/72 px icons,
    # at the best resolution; the Vita wants 128x128 with 8-bit colour.
    art = 'iTunesArtwork' if 'iTunesArtwork' in names else None
    if not art:
        try:
            info = plistlib.loads(z.read(APP + 'Info.plist'))
            files = [APP + f for f in info.get('CFBundleIconFiles', []) if APP + f in names]
            art = max(files, key=lambda f: z.getinfo(f).file_size) if files else None
        except Exception:
            art = None
    icon = os.path.join(a.out, 'icon0.png')
    if (art or a.icon) and a.ffmpeg and (not os.path.exists(icon) or a.redo_icon or a.icon):
        with tempfile.TemporaryDirectory() as td:
            if a.icon:
                src = a.icon
            else:
                src = os.path.join(td, 'art')
                with open(src, 'wb') as f:
                    f.write(z.read(art))
            # the middle square, less any near-black border (a round picture on black
            # then fills the Vita's round icon), at 128x128 in 256 colours
            vf = r'crop=min(iw\,ih):min(iw\,ih),'
            trim = trim_box(a.ffmpeg, src) if a.icon else None
            if trim:
                vf = 'crop=%d:%d:%d:%d,' % trim
            r = subprocess.run([a.ffmpeg, '-v', 'error', '-y', '-i', src, '-vf',
                                vf + 'scale=128:128:flags=lanczos,split[a][b];[a]palettegen=max_colors=256[p];'
                                '[b][p]paletteuse=dither=none', '-frames:v', '1', '-pix_fmt', 'pal8', icon])
            print('Vita icon from %s' % (os.path.basename(a.icon) if a.icon else art)
                  if r.returncode == 0 else 'could not make the Vita icon')

    # ---- music ----
    aacs = sorted(n for n in names if n.startswith(APP) and n.endswith('.aac') and n.count('/') == 2)
    tracks = []
    own = {}                                   # track name -> a better file of the user's
    if a.music_dir:
        exts = ('.flac', '.wav', '.aif', '.aiff', '.mp3', '.m4a', '.aac', '.ogg', '.opus', '.wma', '.alac')
        key = lambda t: ''.join(ch for ch in t.lower() if ch.isalnum())
        for fn in sorted(os.listdir(a.music_dir)):
            base, ext = os.path.splitext(fn)
            if ext.lower() in exts:
                own[key(base.split('.wav')[0])] = os.path.join(a.music_dir, fn)
        wanted = {key(music_name(n)) for n in aacs}
        for k in sorted(set(own) - wanted):
            print('  --music-dir: %s matches no track (they are: %s)' % (
                os.path.basename(own[k]), ', '.join(sorted(music_name(n) for n in aacs))))
    if not a.ffmpeg:
        print('ffmpeg not found: no music this time (install it and run again; the sound effects are kept)')
    else:
        for n in aacs:
            name = music_name(n)
            dst = os.path.join(mus_dir, name + '.ogg')
            tracks.append(name)
            mine = own.get(''.join(ch for ch in name if ch.isalnum()))
            if os.path.exists(dst) and not mine and not a.redo_music:
                continue
            with tempfile.TemporaryDirectory() as td:
                if mine:
                    src = mine
                else:
                    src = os.path.join(td, 'in.aac')
                    with open(src, 'wb') as f:
                        f.write(z.read(n))
                tmp = dst + '.part'
                # libvorbis if this ffmpeg has it; otherwise ffmpeg's own Vorbis
                # encoder, which only takes stereo (the app's tracks are mono: copied
                # to both). Quality 6 (about 190 kbps stereo) for the app's 144 kbps
                # AAC, so converting loses little more; 7 for files of your own.
                q = '7' if mine else '6'
                encoders = [['-ac', '2', '-c:a', 'libvorbis', '-q:a', q] if mine else ['-c:a', 'libvorbis', '-q:a', q],
                            ['-ac', '2', '-c:a', 'vorbis', '-strict', '-2', '-q:a', q]]
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
            print('  music: %s%s' % (name, ' (from %s)' % os.path.basename(mine) if mine else ''))
        print('%d music tracks' % len(tracks))

    with open(os.path.join(a.out, 'manifest.txt'), 'w') as f:
        f.write('gwvita 1\nsource %s %s\nsfx %d\nmusic %s\n' % (bid, ver, made + skipped, ' '.join(tracks)))
    print('done: %s' % os.path.abspath(a.out))


if __name__ == '__main__':
    main()
