"""Draws HF Blade's PlayStation button icons and writes src/buttonicons.h (and a preview PNG).

The icons are drawn here from simple shapes and text; nothing is taken from the game. The tables
below only give where each Xbox icon sits in the game's 512x512 button atlas (ui_core.dtt, texture 2;
two layouts: the PC one and the one DLC 3 uses) and which PlayStation icon replaces it. In game,
the mod writes these icons over those rectangles when the atlas is loaded (see src/buttonicons.inl).

Usage: py -3 tools/make_button_icons.py      (needs Pillow and the Windows Bahnschrift font)
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'bahnschrift.ttf')
SS = 8          # supersampling factor for drawing
MARGIN = 1      # rectangles are cleared 1 px beyond the original icon (its soft edge)

# FNV-1a of the first 0x40080 bytes of the atlas DDS (header + 512x512 DXT5), per layout.
HASH_PC, HASH_DLC3 = 0x1F00D3E4, 0x1EF6D0B8

# (x, y, w, h, 'shape label'): the bounding box of each Xbox icon in the atlas.
LAYOUT_PC = [
    (  7,   7, 68, 41, 'trig_big L2'),
    (  7,  63, 68, 35, 'bump_big R1'),
    (  7, 119, 68, 41, 'trig_big R2'),
    (  7, 175, 68, 35, 'bump_big L1'),
    (  4, 229, 37, 20, 'bumpl_fill L1'),
    (  3, 316, 34, 22, 'box_out R2'),
    (  4, 348, 30, 22, 'stick_press R'),
    (  3, 380, 30, 23, 'stick_press R'),
    ( 47, 259, 33, 28, 'options -'),
    ( 43, 347, 30, 26, 'stick_out R'),
    ( 43, 436, 23, 22, 'face_fill circle'),
    ( 48, 316, 30, 22, 'stick_press L'),
    ( 87, 319, 33, 28, 'options -'),
    ( 87, 356, 30, 23, 'stick_press L'),
    ( 87, 391, 28, 28, 'create -'),
    ( 87, 460, 23, 22, 'face_fill cross'),
    ( 91,   9, 50, 40, 'stick_big R'),
    ( 88,  65, 37, 20, 'bumpr_fill R1'),
    ( 91, 121, 50, 40, 'stick_big L'),
    ( 88, 289, 33, 20, 'box_fill R2'),
    (143, 351, 30, 26, 'stick_out L'),
    (143, 387, 28, 28, 'create -'),
    (147, 179, 35, 35, 'face_big square'),
    (144, 289, 33, 20, 'box_fill L2'),
    (144, 320, 30, 22, 'stick_fill R'),
    (159,  63, 35, 35, 'face_big triangle'),
    (159, 119, 35, 35, 'face_big cross'),
    (195, 176, 39, 22, 'bumpr_out R1'),
    (195, 208, 39, 22, 'bumpl_out L1'),
    (195, 296, 34, 22, 'box_out L2'),
    (196, 328, 30, 22, 'stick_fill L'),
    (195, 436, 23, 22, 'face_fill triangle'),
    (211, 119, 35, 35, 'face_big circle'),
    (227, 404, 23, 22, 'face_fill square'),
    (227, 469, 22, 21, 'face_out circle'),
    (255,  61, 22, 21, 'face_out cross'),
    (259,   5, 22, 21, 'face_out triangle'),
    (259, 117, 22, 21, 'face_out square'),
]
LAYOUT_DLC3 = [
    (  7,   7, 68, 41, 'trig_big L2'),
    (  7,  63, 68, 35, 'bump_big R1'),
    (  7, 119, 68, 41, 'trig_big R2'),
    (  7, 175, 68, 35, 'bump_big L1'),
    (  4, 229, 37, 20, 'bumpl_fill L1'),
    (  3, 316, 34, 22, 'box_out R2'),
    (  4, 348, 30, 22, 'stick_press R'),
    (  3, 379, 30, 26, 'stick_out R'),
    ( 47, 260, 34, 22, 'box_out L2'),
    ( 44, 316, 30, 22, 'stick_press L'),
    ( 43, 448, 23, 22, 'face_fill circle'),
    ( 51, 228, 30, 23, 'stick_press L'),
    ( 87, 319, 33, 28, 'options -'),
    ( 87, 355, 30, 26, 'stick_out L'),
    ( 87, 387, 28, 28, 'create -'),
    ( 87, 456, 23, 22, 'face_fill cross'),
    ( 91,   9, 50, 40, 'stick_big R'),
    ( 88,  65, 37, 20, 'bumpr_fill R1'),
    ( 91, 121, 50, 40, 'stick_big L'),
    ( 88, 289, 33, 20, 'box_fill R2'),
    (143, 352, 30, 23, 'stick_press R'),
    (143, 481, 22, 21, 'face_out square'),
    (147, 179, 35, 35, 'face_big square'),
    (144, 289, 33, 20, 'box_fill L2'),
    (144, 320, 30, 22, 'stick_fill R'),
    (159,  63, 35, 35, 'face_big triangle'),
    (159, 119, 35, 35, 'face_big cross'),
    (195, 176, 39, 22, 'bumpr_out R1'),
    (195, 208, 39, 22, 'bumpl_out L1'),
    (195, 295, 33, 28, 'options -'),
    (196, 332, 30, 22, 'stick_fill L'),
    (195, 363, 28, 28, 'create -'),
    (195, 432, 23, 22, 'face_fill triangle'),
    (211, 119, 35, 35, 'face_big circle'),
    (231, 365, 22, 21, 'face_out circle'),
    (227, 400, 23, 22, 'face_fill square'),
    (227, 465, 22, 21, 'face_out triangle'),
    (259, 117, 22, 21, 'face_out cross'),
]


def font(px, style='Bold Condensed'):
    f = ImageFont.truetype(FONT, max(4, int(px)))
    f.set_variation_by_name(style)
    return f


def text_in(d, box, s, fill, style='Bold Condensed', hfrac=1.0):
    """Draw s centred in box, as large as fits (cap height ~ hfrac of the box height)."""
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    size = bh * 1.45 * hfrac
    while size > 4:
        f = font(size, style)
        l, t, r, b = d.textbbox((0, 0), s, font=f)
        if r - l <= bw and b - t <= bh * hfrac:
            break
        size *= 0.95
    l, t, r, b = d.textbbox((0, 0), s, font=f)
    d.text((x0 + (bw - (r - l)) / 2 - l, y0 + (bh - (b - t)) / 2 - t), s, font=f, fill=fill)


def rrect(size, box, r_small, r_big=0, big_corners=(False, False, False, False)):
    """Mask of a rounded rectangle; the corners flagged in big_corners (tl, tr, br, bl) use r_big."""
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, r_small, fill=255)
    if any(big_corners):
        n = Image.new('L', size, 0)
        ImageDraw.Draw(n).rounded_rectangle(box, r_big, fill=255, corners=big_corners)
        m = ImageChops.darker(m, n)
    return m


def outline(mask, t):
    k = 2 * int(round(t)) + 1
    padded = Image.new('L', (mask.width + 2 * k, mask.height + 2 * k), 0)   # so edges on the border erode too
    padded.paste(mask, (k, k))
    inner = padded.filter(ImageFilter.MinFilter(k)).crop((k, k, k + mask.width, k + mask.height))
    return ImageChops.subtract(mask, inner)


def symbol(d, kind, cx, cy, r, t, fill):
    if kind == 'cross':
        a = r * 0.62
        d.line((cx - a, cy - a, cx + a, cy + a), fill=fill, width=int(t))
        d.line((cx - a, cy + a, cx + a, cy - a), fill=fill, width=int(t))
    elif kind == 'circle':
        a = r * 0.78
        d.ellipse((cx - a, cy - a, cx + a, cy + a), outline=fill, width=int(t))
    elif kind == 'square':
        a = r * 0.66
        d.rectangle((cx - a, cy - a, cx + a, cy + a), outline=fill, width=int(t))
    elif kind == 'triangle':
        a = r * 0.86
        pts = [(cx, cy - a), (cx + a * 0.866, cy + a * 0.5), (cx - a * 0.866, cy + a * 0.5)]
        d.line(pts + [pts[0], pts[1]], fill=fill, width=int(t), joint='curve')


def draw_icon(w, h, label):
    """Alpha mask (w x h) of one icon."""
    shape, arg = label.split()
    W, H = w * SS, h * SS
    m = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(m)
    full = (0, 0, W - 1, H - 1)
    if shape.startswith('face'):
        r = min(W, H) / 2 - SS * 0.5
        cx, cy = W / 2, H / 2
        if shape == 'face_out':
            ring = max(SS * 1.6, r * 0.13)
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
            d.ellipse((cx - r + ring, cy - r + ring, cx + r - ring, cy + r - ring), fill=0)
            symbol(d, arg, cx, cy, r * 0.62, max(SS * 1.7, r * 0.16), 255)
        else:
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
            symbol(d, arg, cx, cy, r * 0.62, max(SS * 1.8, r * 0.17), 0)
    elif shape.split('_')[0] in ('trig', 'bump', 'bumpl', 'bumpr', 'box'):
        form, style = shape.split('_')
        rs = min(W, H) * (0.16 if style == 'big' else 0.2)
        if form == 'trig':
            body = rrect((W, H), full, rs, H * 0.55, (False, False, True, True))
        elif form == 'bumpl':
            body = rrect((W, H), full, rs, H * 0.75, (True, False, False, False))
        elif form == 'bumpr':
            body = rrect((W, H), full, rs, H * 0.75, (False, True, False, False))
        else:
            body = rrect((W, H), full, rs)
        pad_x = W * (0.16 if form.startswith('bump') and form != 'bump' else 0.12)
        tb = (pad_x if form != 'bumpl' else W * 0.24, H * 0.2,
              W - (pad_x if form != 'bumpr' else W * 0.24), H * (0.72 if form == 'trig' else 0.8))
        if style == 'out':
            m = outline(body, max(SS * 1.3, H * 0.075))
            text_in(ImageDraw.Draw(m), tb, arg, 255)
        else:
            m = body
            text_in(ImageDraw.Draw(m), tb, arg, 0)
    elif shape.startswith('stick'):
        style = shape.split('_')[1]
        lbl = arg + '3' if style == 'press' else arg
        split = H * 0.44
        text_in(d, (W * 0.15, 0, W * 0.85, split - SS * 0.6), lbl, 255, 'Bold SemiCondensed')
        cap = (W * 0.02, split + (H - split) * 0.02, W * 0.98, split + (H - split) * 0.66)
        stem_w = W * 0.36
        body = Image.new('L', (W, H), 0)
        bd = ImageDraw.Draw(body)
        bd.rectangle((W / 2 - stem_w / 2, (cap[1] + cap[3]) / 2, W / 2 + stem_w / 2, H - 1), fill=255)
        bd.ellipse(cap, fill=255)
        if style == 'out':
            body = outline(body, max(SS * 1.2, H * 0.05))
        elif style == 'press':
            cy = (cap[1] + cap[3]) / 2 + (cap[3] - cap[1]) * 0.05
            a = (cap[3] - cap[1]) * 0.42
            bd.polygon([(W / 2 - a * 1.25, cy - a * 0.55), (W / 2 + a * 1.25, cy - a * 0.55), (W / 2, cy + a * 0.85)], fill=0)
        m = ImageChops.lighter(m, body)
    elif shape in ('options', 'create'):
        split = H * 0.56
        pw = W * 0.62
        pill = (W / 2 - pw / 2, H * 0.03, W / 2 + pw / 2, split - SS * 0.4)
        d.rounded_rectangle(pill, (pill[3] - pill[1]) / 2, fill=255)
        ph = pill[3] - pill[1]
        cx, cy = W / 2, (pill[1] + pill[3]) / 2
        if shape == 'options':
            for k in (-1, 0, 1):
                y = cy + k * ph * 0.22
                d.line((cx - pw * 0.2, y, cx + pw * 0.2, y), fill=0, width=int(max(SS * 1.1, ph * 0.11)))
        else:
            t = int(max(SS * 1.1, ph * 0.11))
            for dx, dy in ((0, -1), (-0.8, -0.55), (0.8, -0.55)):   # three rays (a small burst)
                d.line((cx + dx * ph * 0.14, cy + ph * 0.18 + dy * ph * 0.14,
                        cx + dx * ph * 0.36, cy + ph * 0.18 + dy * ph * 0.42), fill=0, width=t)
        text_in(d, (0, split + SS * 0.3, W, H), shape.upper(), 255, 'Bold Condensed')
    else:
        raise ValueError(label)
    return m.resize((w, h), Image.LANCZOS)


def main():
    blobs, offsets, data = {}, {}, bytearray()
    layouts = []
    for name, table in (('Pc', LAYOUT_PC), ('Dlc3', LAYOUT_DLC3)):
        rects = []
        for x, y, w, h, label in table:
            key = (w, h, label)
            if key not in blobs:
                icon = draw_icon(w, h, label)
                padded = Image.new('L', (w + 2 * MARGIN, h + 2 * MARGIN), 0)
                padded.paste(icon, (MARGIN, MARGIN))
                blobs[key] = padded
                offsets[key] = len(data)
                data += padded.tobytes()
            rects.append((x - MARGIN, y - MARGIN, w + 2 * MARGIN, h + 2 * MARGIN, offsets[key]))
        layouts.append((name, rects))

    out = ['// Generated by tools/make_button_icons.py. Do not edit; run the script instead.',
           '// PlayStation button icons drawn by HF Blade (same license as the project), as 8-bit alpha',
           '// masks, and where they go in the game\'s button atlas (see buttonicons.inl).',
           '#pragma once', '',
           'struct BtnIconRect { uint16_t x, y, w, h; uint32_t offset; };',
           f'static const uint32_t kBtnAtlasHashPc = 0x{HASH_PC:08X}, kBtnAtlasHashDlc3 = 0x{HASH_DLC3:08X};', '']
    for name, rects in layouts:
        out.append(f'static const BtnIconRect kBtnRects{name}[{len(rects)}] = {{')
        out += [f'    {{ {x}, {y}, {w}, {h}, {o} }},' for x, y, w, h, o in rects]
        out.append('};')
    out.append(f'static const uint8_t kBtnAlpha[{len(data)}] = {{')
    for i in range(0, len(data), 32):
        out.append('    ' + ','.join(str(v) for v in data[i:i + 32]) + ',')
    out.append('};')
    open(os.path.join(ROOT, 'src', 'buttonicons.h'), 'w', newline='\n').write('\n'.join(out) + '\n')

    # Preview: the PC layout, white icons on a dark background, 3x.
    prev = Image.new('L', (290, 512), 0)
    for x, y, w, h, label in LAYOUT_PC:
        prev.paste(blobs[(w, h, label)], (x - MARGIN, y - MARGIN))
    bg = Image.new('RGB', prev.size, (38, 40, 46))
    bg.paste(Image.new('RGB', prev.size, (255, 255, 255)), (0, 0), prev)
    bg.resize((prev.width * 3, prev.height * 3), Image.NEAREST).save(os.path.join(ROOT, 'docs', 'button-icons.png'))
    print(f'{len(blobs)} icons, {len(data)} bytes of alpha, {sum(len(r) for _, r in layouts)} rectangles.')


if __name__ == '__main__':
    main()
