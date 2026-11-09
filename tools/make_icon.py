#!/usr/bin/env python3
"""Generates the WinRepair logo: res/winrepair.ico (16..256 px), docs/branding/logo-512.png. Run: python3 tools/make_icon.py"""
import struct, io, math
from PIL import Image, ImageDraw, ImageFilter

N = 1024  # supersampled canvas

def gradient(size, top, bottom):
    img = Image.new('RGB', (size, size))
    px = img.load()
    for y in range(size):
        t = y / (size - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(size):
            px[x, y] = c
    return img

def rounded_mask(size, r):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], r, fill=255)
    return m

def wrench_layer():
    L = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    cx, cy = N // 2, N // 2
    white = (255, 255, 255, 255)
    # handle (vertical bar), head (circle with open jaw) on top; later rotated 45 degrees
    d.rounded_rectangle([cx - 62, cy - 120, cx + 62, cy + 330], 62, fill=white)
    d.ellipse([cx - 175, cy - 330, cx + 175, cy + 20], fill=white)
    d.rounded_rectangle([cx - 66, cy - 360, cx + 66, cy - 150], 8, fill=(0, 0, 0, 0))   # jaw cut (transparent)
    # clear the jaw properly
    cut = Image.new('L', (N, N), 255)
    ImageDraw.Draw(cut).rounded_rectangle([cx - 66, cy - 380, cx + 66, cy - 170], 14, fill=0)
    a = L.split()[3]
    a = Image.composite(a, Image.new('L', (N, N), 0), cut)
    L.putalpha(a)
    return L.rotate(-45, resample=Image.BICUBIC, center=(cx, cy))

def build():
    bg = gradient(N, (14, 34, 66), (24, 118, 222)).convert('RGBA')
    # soft highlight
    glow = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-200, -300, 900, 500], fill=(255, 255, 255, 36))
    bg = Image.alpha_composite(bg, glow.filter(ImageFilter.GaussianBlur(60)))
    # shadow + wrench
    w = wrench_layer()
    sh = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    sh.putalpha(w.split()[3].point(lambda v: v * 0.35))
    sh = sh.filter(ImageFilter.GaussianBlur(14)).transform((N, N), Image.AFFINE, (1, 0, -10, 0, 1, -16))
    bg = Image.alpha_composite(bg, sh)
    bg = Image.alpha_composite(bg, w)
    # badge: green shield with check, bottom-right
    d = ImageDraw.Draw(bg)
    bx, by, s = 700, 690, 250
    shield = [(bx, by - s), (bx + s * .86, by - s * .62), (bx + s * .86, by + s * .1), (bx, by + s), (bx - s * .86, by + s * .1), (bx - s * .86, by - s * .62)]
    d.polygon([(x + 8, y + 12) for x, y in shield], fill=(0, 0, 0, 70))
    d.polygon(shield, fill=(255, 255, 255, 255))
    inner = [(bx + (x - bx) * .84, by + (y - by) * .84) for x, y in shield]
    d.polygon(inner, fill=(34, 178, 106, 255))
    d.line([(bx - 105, by + 5), (bx - 30, by + 85), (bx + 120, by - 90)], fill=(255, 255, 255, 255), width=46, joint='curve')
    for px_, py_ in [(bx - 105, by + 5), (bx + 120, by - 90)]:
        d.ellipse([px_ - 23, py_ - 23, px_ + 23, py_ + 23], fill=(255, 255, 255, 255))
    out = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    out.paste(bg, (0, 0), rounded_mask(N, 230))
    return out

def png_bytes(im):
    b = io.BytesIO(); im.save(b, 'PNG'); return b.getvalue()

def dib_bytes(im):
    w, h = im.size
    px = im.convert('RGBA').load()
    hdr = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            rows += bytes((b, g, r, a))
    mask_row = ((w + 31) // 32) * 4
    return hdr + bytes(rows) + bytes(mask_row * h)    # AND mask all zero: alpha channel does the work

def write_ico(path, base, sizes):
    entries = []
    for s in sizes:
        im = base.resize((s, s), Image.LANCZOS)
        data = png_bytes(im) if s >= 256 else dib_bytes(im)
        entries.append((s, data))
    out = bytearray(struct.pack('<HHH', 0, 1, len(entries)))
    off = 6 + 16 * len(entries)
    for s, data in entries:
        out += struct.pack('<BBBBHHII', 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(data), off)
        off += len(data)
    for _, data in entries: out += data
    open(path, 'wb').write(out)

if __name__ == '__main__':
    base = build()
    base.resize((512, 512), Image.LANCZOS).save('docs/branding/logo-512.png')
    base.resize((256, 256), Image.LANCZOS).save('docs/branding/logo-256.png')
    write_ico('res/winrepair.ico', base, [16, 24, 32, 48, 64, 128, 256])
    print('ok')
