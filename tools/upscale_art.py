#!/usr/bin/env python3
"""
upscale_art.py - the game's walls and sprites, redrawn 4x by a neural upscaler.

    py tools/upscale_art.py                 # fetch the upscaler if needed, build work/wl6/art/art.bin
    py tools/upscale_art.py --model anime   # realesrgan-x4plus (default) | realesrgan-x4plus-anime

Optional, like DLSS (docs/art.md). Reads your own original/wl6/VSWAP.WL6 (the
page file: 64x64 wall textures and the sprites' column posts, as the modding
wiki documents it), runs each page through Real-ESRGAN (the ncnn/Vulkan build,
MIT; downloaded into the gitignored external/realesrgan/), and packs the
results into work/wl6/art/art.bin, gitignored: art derived from the game is
never committed. The hi-res renderer (src/hires.c) finds each page in guest
memory by a hash of its contents and samples the 4x art instead.

Sprites are upscaled twice: the colours (with the transparent area filled
from the nearest opaque texel, so no dark fringe bleeds in) and the coverage
mask, which gives smooth silhouettes instead of 4x-enlarged stair steps.
Standard library only, so Setup needs nothing more than Python.
"""
import os
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'original', 'wl6')
OUT = os.path.join(ROOT, 'work', 'wl6', 'art')
TOOL = os.path.join(ROOT, 'external', 'realesrgan')
TOOL_URL = ('https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/'
            'realesrgan-ncnn-vulkan-20220424-windows.zip')
SCALE = 4
MODELS = {'anime': 'realesrgan-x4plus-anime', 'x4plus': 'realesrgan-x4plus'}


# ---- PNG, just enough: 8-bit RGB/RGBA, no interlace -------------------------

def png_write(path, w, h, rows, channels):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if channels == 4 else 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def png_read(path):
    """-> (w, h, channels, bytearray of pixels). Pillow when it is there
    (fast); otherwise this decoder (correct, slow: minutes for the set)."""
    try:
        from PIL import Image
        im = Image.open(path)
        im = im.convert('RGBA' if im.mode in ('RGBA', 'LA', 'P') else 'RGB')
        return im.width, im.height, len(im.mode), bytearray(im.tobytes())
    except ImportError:
        pass
    d = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(d):
        n, t = struct.unpack_from('>I4s', d, pos)
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', body)
            assert depth == 8 and ctype in (2, 6), (path, depth, ctype)
            ch = 4 if ctype == 6 else 3
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * ch
    out, prev = bytearray(h * stride), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b, c = prev[x], prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, ch, out


# ---- the game's data --------------------------------------------------------

def vswap():
    """-> (pages, sprite_start, sound_start); pages are bytes"""
    d = open(os.path.join(GAME, 'VSWAP.WL6'), 'rb').read()
    n, spr, snd = struct.unpack_from('<3H', d)
    offs = struct.unpack_from(f'<{n}I', d, 6)
    lens = struct.unpack_from(f'<{n}H', d, 6 + 4 * n)
    return [d[o:o + l] for o, l in zip(offs, lens)], spr, snd


def palette():
    """The game palette (768 bytes, 6-bit), found in the unpacked executable
    by its first eight entries -- the EGA colours, as every id VGA palette
    starts. tools/lift.py writes work/wl6/unpacked.exe."""
    img = open(os.path.join(ROOT, 'work', 'wl6', 'unpacked.exe'), 'rb').read()
    sig = bytes([0, 0, 0, 0, 0, 42, 0, 42, 0, 0, 42, 42, 42, 0, 0, 42, 0, 42, 42, 21, 0, 42, 42, 42])
    at = img.find(sig)
    if at < 0:
        sys.exit('palette not found in work/wl6/unpacked.exe; run py tools/lift.py wl6 first')
    pal = img[at:at + 768]
    return [((pal[3 * i] * 255 + 31) // 63, (pal[3 * i + 1] * 255 + 31) // 63, (pal[3 * i + 2] * 255 + 31) // 63)
            for i in range(256)]


def sprite_posts(page):
    """{column: [(start, end, source)]} of a t_compshape page"""
    left, right = struct.unpack_from('<2H', page)
    cols = {}
    for c in range(left, right + 1):
        cmd, posts = struct.unpack_from('<H', page, 4 + 2 * (c - left))[0], []
        while cmd + 6 <= len(page):
            end, src, st = struct.unpack_from('<HhH', page, cmd)
            if not end:
                break
            posts.append((st // 2, end // 2, src))
            cmd += 6
        cols[c] = posts
    return cols


def sprite_key(page):
    """What src/hires.c art_sprite_key() computes from guest memory: FNV-1a
    over the header, every post command and every pixel the posts use."""
    left, right = struct.unpack_from('<2H', page)
    h = 0x811C9DC5
    def mix(bs):
        nonlocal h
        for b in bs:
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    mix(page[:4 + 2 * (right - left + 1)])
    for c, posts in sorted(sprite_posts(page).items()):
        for st, end, src in posts:
            mix(struct.pack('<HhH', end * 2, src, st * 2))
            mix(bytes(page[(src + r) & 0xFFFF] for r in range(st, end)))
    return h


def wall_key(page):
    h = 0x811C9DC5
    for b in page[:4096]:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    model = MODELS[sys.argv[sys.argv.index('--model') + 1]] if '--model' in sys.argv else MODELS['x4plus']
    exe = os.path.join(TOOL, 'realesrgan-ncnn-vulkan.exe')
    if not os.path.exists(exe):
        print(f'fetching the upscaler (Real-ESRGAN ncnn/Vulkan, MIT, ~45 MB) into {TOOL}')
        os.makedirs(TOOL, exist_ok=True)
        z = TOOL + '.zip'
        if subprocess.run(['curl', '-sSfL', '--retry', '5', '-o', z, TOOL_URL]).returncode:
            sys.exit('download failed; run this again')
        shutil.unpack_archive(z, TOOL)
        os.remove(z)
    pages, spr, snd = vswap()
    pal = palette()
    tmp_in, tmp_out = os.path.join(OUT, 'in'), os.path.join(OUT, 'out')
    for d in (tmp_in, tmp_out):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    # Walls: column-major 64x64 palette indices -> RGB rows, tiled 3x3: walls
    # repeat, and an upscaler that saw only one tile would invent edges at
    # its borders; the middle of the 3x3 result wraps without a seam.
    walls = [i for i in range(spr) if len(pages[i]) == 4096]
    for i in walls:
        p = pages[i]
        rows = [[v for x in range(64) for v in pal[p[x * 64 + y]]] * 3 for y in range(64)] * 3
        png_write(os.path.join(tmp_in, f'w{i:04d}.png'), 192, 192, rows, 3)
    # Sprites: colours with the transparent area filled outward, and the mask.
    sprites = [i for i in range(spr, snd) if len(pages[i]) >= 4]
    for i in sprites:
        idx = [[None] * 64 for _ in range(64)]
        for c, posts in sprite_posts(pages[i]).items():
            for st, end, src in posts:
                for r in range(st, end):
                    idx[r][c] = pages[i][(src + r) & 0xFFFF]
        col = [[pal[v] if v is not None else None for v in row] for row in idx]
        for _ in range(8):                                   # grow colours into the gaps
            nxt = [row[:] for row in col]
            for y in range(64):
                for x in range(64):
                    if col[y][x] is None:
                        n = [col[yy][xx] for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1))
                             if 0 <= yy < 64 and 0 <= xx < 64 and col[yy][xx] is not None]
                        if n:
                            nxt[y][x] = tuple(sum(c[k] for c in n) // len(n) for k in range(3))
            col = nxt
        png_write(os.path.join(tmp_in, f's{i:04d}.png'), 64, 64,
                  [[v for c in row for v in (c or (0, 0, 0))] for row in col], 3)
        png_write(os.path.join(tmp_in, f'm{i:04d}.png'), 64, 64,
                  [[v for x in range(64) for v in ((255,) * 3 if idx[y][x] is not None else (0,) * 3)] for y in range(64)], 3)
    print(f'{len(walls)} walls, {len(sprites)} sprites -> {model} x{SCALE}')
    r = subprocess.run([exe, '-i', tmp_in, '-o', tmp_out, '-n', model, '-s', str(SCALE), '-f', 'png'],
                       cwd=TOOL, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'the upscaler failed:\n{r.stderr[-2000:]}')

    # Pack: header, then per page {type, key, w, h, RGBA}.
    S = 64 * SCALE
    with open(os.path.join(OUT, 'art.bin.part'), 'wb') as f:
        f.write(b'WOLFART1' + struct.pack('<4I', len(walls) + len(sprites), S, 0, 0))
        f.write(bytes(v for c in pal for v in c))
        for i in walls:
            w, h, ch, big = png_read(os.path.join(tmp_out, f'w{i:04d}.png'))
            px = b''.join(big[(S + y) * w * ch + S * ch:(S + y) * w * ch + 2 * S * ch] for y in range(S))
            rgba = bytearray([255]) * (S * S * 4)
            for k in range(3):
                rgba[k::4] = px[k::ch]
            f.write(struct.pack('<4I', 0, wall_key(pages[i]), S, S) + rgba)
        for i in sprites:
            w, h, ch, px = png_read(os.path.join(tmp_out, f's{i:04d}.png'))
            _, _, mch, m = png_read(os.path.join(tmp_out, f'm{i:04d}.png'))
            rgba = bytearray(S * S * 4)
            for k in range(3):
                rgba[k::4] = px[k::ch]
            rgba[3::4] = m[0::mch]
            f.write(struct.pack('<4I', 1, sprite_key(pages[i]), S, S) + rgba)
    os.replace(os.path.join(OUT, 'art.bin.part'), os.path.join(OUT, 'art.bin'))
    shutil.rmtree(tmp_in)
    print(f'-> {os.path.join(OUT, "art.bin")} ({os.path.getsize(os.path.join(OUT, "art.bin")) >> 20} MB); '
          f'upscaled PNGs kept in {tmp_out} to look at')


if __name__ == '__main__':
    main()
