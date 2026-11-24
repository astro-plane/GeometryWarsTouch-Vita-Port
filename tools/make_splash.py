#!/usr/bin/env python3
"""Turn your own splash picture into app_data/splash.bmp (960x544, 24-bit),
which the game shows for 9 seconds at start-up while it loads.

    python3 tools/make_splash.py my_splash.jpg app_data/splash.bmp

The picture is scaled to fill the Vita's screen and the longer side cropped.
Uses Pillow if it is installed, otherwise macOS's built-in `sips`. Nothing of
the picture goes into the source tree: app_data/ is made on your machine."""
import os, shutil, subprocess, sys, tempfile

W, H = 960, 544


def with_pillow(src, dst):
    from PIL import Image
    im = Image.open(src).convert('RGB')
    s = max(W / im.width, H / im.height)
    im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x, y = (im.width - W) // 2, (im.height - H) // 2
    im.crop((x, y, x + W, y + H)).save(dst, 'BMP')


def with_sips(src, dst):
    out = subprocess.run(['sips', '-g', 'pixelWidth', '-g', 'pixelHeight', src],
                         capture_output=True, text=True, check=True).stdout.split()
    w, h = int(out[out.index('pixelWidth:') + 1]), int(out[out.index('pixelHeight:') + 1])
    s = max(W / w, H / h)
    with tempfile.TemporaryDirectory() as t:
        tmp = os.path.join(t, 'splash.bmp')
        subprocess.run(['sips', '-s', 'format', 'bmp', '-z', str(round(h * s)), str(round(w * s)),
                        src, '--out', tmp], check=True, capture_output=True)
        subprocess.run(['sips', '-c', str(H), str(W), tmp, '--out', dst], check=True, capture_output=True)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    try:
        with_pillow(src, dst)
    except ImportError:
        if not shutil.which('sips'):
            sys.exit('needs Pillow (pip3 install pillow) or macOS sips')
        with_sips(src, dst)
    print('wrote', dst)


if __name__ == '__main__':
    main()
