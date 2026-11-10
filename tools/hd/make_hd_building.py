"""Build a high-resolution sprite pack (.hdspr) for one building.

The game keeps drawing the normal 1x SHP (it decides silhouette, depth and
selection). When the map is rendered at an HD scale S (Runtime HdScale=S), the
GPU samples this pack instead of the palette colour, so each game pixel shows
S x S real pixels of the source art.

  python tools/hd/make_hd_building.py --source art.png --stock-body ggpowr.stock.shp \
      --body ggpowr.shp --anim ggpowr_a.shp --palette unittem.pal --scale 3 --out local_data/hd

--stock-body  stock SHP whose frame-0 box the source was fitted to (same fit as
              the 1x conversion, so HD and 1x line up exactly)
--body        the 1x enhanced body SHP (normal, damaged, heavy, shadows)
--anim        optional 1x glow animation SHP (normal loop, then damaged loop)

Pack format (little endian): "CHD1", u32 version=2, u32 scale, u32 canvasW,
u32 canvasH, u32 frameCount, then per frame u32 present and, if present,
canvasW*S x canvasH*S BGRA pixels (straight alpha) followed by the same number
of team-colour mask bytes (0 = keep the art colour, 255 = player colour).
"""
import argparse
import math
import os
import random
import struct

import numpy as np
from PIL import Image


# ---------------------------------------------------------------- SHP (TS) reader
def read_shp(path):
    data = open(path, "rb").read()
    _, width, height, count = struct.unpack_from("<4H", data, 0)
    frames = []
    for i in range(count):
        x, y, w, h, comp, _, _, offset = struct.unpack_from("<4HI4sII", data, 8 + i * 24)
        canvas = np.zeros((height, width), np.uint8)
        if w and h and offset:
            pixels = np.zeros((h, w), np.uint8)
            p = offset
            kind = comp & 0xFF
            for row in range(h):
                if kind in (2, 3):
                    length = struct.unpack_from("<H", data, p)[0]
                    seg, p = data[p + 2:p + length], p + length
                    if kind == 2:
                        pixels[row, :min(w, len(seg))] = list(seg[:w])
                    else:
                        col, k = 0, 0
                        while k < len(seg):
                            if seg[k]:
                                if col < w:
                                    pixels[row, col] = seg[k]
                                col, k = col + 1, k + 1
                            else:
                                col, k = col + seg[k + 1], k + 2
                else:
                    pixels[row] = list(data[p:p + w])
                    p += w
            canvas[y:y + h, x:x + w] = pixels
        frames.append({"box": (x, y, w, h), "canvas": canvas})
    return width, height, frames


def read_pal(path):
    raw = open(path, "rb").read()[:768]
    return np.array([[min(255, c * 4) for c in raw[i * 3:i * 3 + 3]] for i in range(256)], np.float32)


# ---------------------------------------------------------------- helpers
def fit_source(source, stock_box, W, H, S):
    """Place the source art on a (W*S x H*S) canvas exactly as the 1x conversion did."""
    src = source.convert("RGBA")
    alpha = np.asarray(src)[:, :, 3]
    ys, xs = np.where(alpha >= 128)
    x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    bw, bh = x1 - x0, y1 - y0
    fx, fy, fw, fh = stock_box
    scale = (fw / float(bw) + fh / float(bh)) / 2.0           # same as enhance_building.py
    ox = fx + (fw - bw * scale) / 2.0
    oy = fy + fh - bh * scale
    # premultiply, resize the box with Lanczos, then shift by the sub-pixel remainder
    arr = np.asarray(src.crop((x0, y0, x1, y1))).astype(np.float32) / 255.0
    arr[:, :, :3] *= arr[:, :, 3:4]
    tw, th = bw * scale * S, bh * scale * S
    rw, rh = max(1, int(round(tw))), max(1, int(round(th)))
    chans = [np.asarray(Image.fromarray(arr[:, :, c]).resize((rw, rh), Image.LANCZOS)) for c in range(4)]
    small = np.clip(np.stack(chans, -1), 0, 1)
    out = np.zeros((H * S, W * S, 4), np.float32)
    px, py = ox * S, oy * S
    ix, iy = int(math.floor(px)), int(math.floor(py))
    fx_, fy_ = px - ix, py - iy
    # bilinear sub-pixel shift
    shifted = np.zeros((rh + 1, rw + 1, 4), np.float32)
    shifted[:rh, :rw] += small * (1 - fx_) * (1 - fy_)
    shifted[:rh, 1:] += small * fx_ * (1 - fy_)
    shifted[1:, :rw] += small * (1 - fx_) * fy_
    shifted[1:, 1:] += small * fx_ * fy_
    y_lo, x_lo = max(0, iy), max(0, ix)
    y_hi, x_hi = min(H * S, iy + rh + 1), min(W * S, ix + rw + 1)
    out[y_lo:y_hi, x_lo:x_hi] = shifted[y_lo - iy:y_hi - iy, x_lo - ix:x_hi - ix]
    a = out[:, :, 3:4]
    rgb = np.where(a > 1e-4, out[:, :, :3] / np.maximum(a, 1e-4), 0)
    print("fit: source box %dx%d, scale %.4f (x%d -> %.4f), origin %.2f,%.2f" % (bw, bh, scale, S, scale * S, ox, oy))
    return np.concatenate([np.clip(rgb, 0, 1) * 255, a * 255], -1)


def upsample(mask, S):
    return np.repeat(np.repeat(mask, S, 0), S, 1)


def blur(img, r):
    if r <= 0:
        return img
    k = np.ones(2 * r + 1, np.float32) / (2 * r + 1)
    out = np.apply_along_axis(lambda m: np.convolve(m, k, mode="same"), 0, img)
    return np.apply_along_axis(lambda m: np.convolve(m, k, mode="same"), 1, out)


def lum(rgb):
    return rgb[..., 0] * 0.3 + rgb[..., 1] * 0.59 + rgb[..., 2] * 0.11


def damaged_variant(hd, normal_idx, damaged_idx, pal, S):
    """Carry the 1x damage (scorch ratio and holes) onto the HD art."""
    on_n, on_d = normal_idx > 0, damaged_idx > 0
    ln, ld = lum(pal[normal_idx]), lum(pal[damaged_idx])
    ratio = np.where(on_n & on_d, (ld + 8) / (ln + 8), 1.0).astype(np.float32)
    ratio = np.clip(ratio, 0.15, 1.2)
    ratio_hd = blur(upsample(ratio, S), max(1, S // 2))
    out = hd.copy()
    out[..., :3] *= ratio_hd[..., None]
    holes = upsample(on_n & ~on_d, S)
    out[..., 3] = np.where(holes, 0, out[..., 3])
    out[..., :3] = np.clip(out[..., :3], 0, 255)
    return out


def fractal_noise(h, w, cell, rng, octaves=4):
    """Smooth 0..1 value noise with `octaves` layers, finest cell >= 2 px."""
    total = np.zeros((h, w), np.float32)
    weight = 0.0
    for o in range(octaves):
        c = max(2, int(cell / (2 ** o)))
        gh, gw = h // c + 3, w // c + 3
        grid = rng.random((gh, gw)).astype(np.float32)
        layer = np.asarray(Image.fromarray(grid).resize((gw * c, gh * c), Image.BICUBIC))[:h, :w]
        total += layer * (0.5 ** o)
        weight += 0.5 ** o
    total /= weight
    return (total - total.min()) / max(1e-6, total.max() - total.min())


def hd_damage(hd, level, seed, S):
    """Damage drawn at HD resolution (level 1 = damaged, 2 = heavily damaged):
    irregular scorch with soot tint, dimmed glows, and for level 2 jagged holes
    with charred rims and a few embers. Same blob counts/sizes as the 1x look."""
    rng = np.random.default_rng(seed)
    out = hd.copy()
    H, W = hd.shape[:2]
    rgb = out[..., :3]
    # failing power: dim the glowing parts with a soft mask (a hard per-pixel test
    # leaves speckled edges where neighbours fall either side of the threshold)
    glow = blur((is_glow(hd[..., :3]) & (hd[..., 3] > 32)).astype(np.float32), max(1, S // 2))
    dim = np.array([1 - 0.22 * level, 1 - 0.32 * level, 1 - 0.32 * level], np.float32)
    rgb *= 1 - glow[..., None] * (1 - dim)
    ys, xs = np.where(hd[..., 3] > 128)
    edge = fractal_noise(H, W, 6 * S, rng)                 # ragged burn outlines
    fine = fractal_noise(H, W, 1.5 * S, rng, octaves=3)    # crumbly edges for cavities
    grain = fractal_noise(H, W, 2 * S, rng, octaves=2)     # soot texture
    shade = np.full((H, W), 1 - 0.10 * level, np.float32)
    yy, xx = np.mgrid[0:H, 0:W]

    def blob(cy, cx, radius, noise, amount):
        d = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2) / radius
        return d * (1 - amount / 2 + amount * noise)        # noisy distance -> ragged shape

    for _ in range(8 * level):
        i = rng.integers(len(ys))
        d = blob(ys[i], xs[i], rng.uniform(3.5, 8.0) * S, edge, 0.7)
        k = 0.12 + 0.88 * np.clip(d, 0, 1) ** 1.4           # darkest at the centre
        shade *= np.where(d < 1, np.minimum(1.0, k * (0.75 + 0.5 * grain)), 1.0)   # sooty grain
    scorch = np.clip(1 - shade, 0, 1)
    rgb *= shade[..., None]
    rgb[..., 1] *= 1 - 0.12 * scorch                       # soot: shift toward brown-black
    rgb[..., 2] *= 1 - 0.25 * scorch
    if level >= 2:
        for _ in range(6):
            # blown-open cavity: dark interior with a crumbly, scorched rim (not see-through)
            i = rng.integers(len(ys))
            d = blob(ys[i], xs[i], rng.uniform(1.5, 3.2) * S, fine, 0.9)
            inside, rim = d < 1, (d >= 1) & (d < 1.6)
            rgb[inside] = rgb[inside] * 0.08 + np.array([18, 12, 8], np.float32)
            rgb[rim] *= (0.3 + 0.5 * (d[rim] - 1) / 0.6)[:, None]
        # a few soft embers deep inside the worst scorch
        ember = blur(((scorch > 0.65) & (grain > 0.9) & (out[..., 3] > 128)).astype(np.float32), 1)
        rgb = rgb * (1 - 0.6 * ember[..., None]) + np.array([255, 110, 25], np.float32) * 0.6 * ember[..., None]
    out[..., :3] = np.clip(rgb, 0, 255)
    return out


def to_1x_indices(hd, pal, S, allowed):
    """Area-average an HD frame back to game pixels and quantise to palette
    indices (house reds -> remap slots 16-31), like the 1x conversion."""
    H, W = hd.shape[0] // S, hd.shape[1] // S
    blocks = hd[:H * S, :W * S].reshape(H, S, W, S, 4)
    a = blocks[..., 3].mean(axis=(1, 3)) / 255.0
    wsum = blocks[..., 3].sum(axis=(1, 3))[..., None]
    rgb = (blocks[..., :3] * blocks[..., 3:4]).sum(axis=(1, 3)) / np.maximum(wsum, 1e-6)
    idx = np.zeros((H, W), np.uint8)
    on = a >= 0.5
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    house = on & (r > 70) & (g < 0.36 * r) & (b < 0.42 * r)
    lum = np.max(rgb, axis=-1) / 255.0
    idx[house] = (16 + np.clip(np.round((1 - lum[house] ** 0.8) * 15), 0, 15)).astype(np.uint8)
    plain = on & ~house
    cand = pal[allowed]
    px = rgb[plain]
    d = (3 * (px[:, None, 0] - cand[None, :, 0]) ** 2 + 4 * (px[:, None, 1] - cand[None, :, 1]) ** 2
         + 2 * (px[:, None, 2] - cand[None, :, 2]) ** 2)
    idx[plain] = np.array(allowed, np.uint8)[np.argmin(d, axis=1)]
    return idx


def write_shp(path, W, H, canvases):
    """TS SHP, raw (compression 1) frames cropped to their opaque box."""
    records, blobs = [], []
    offset = 8 + 24 * len(canvases)
    for c in canvases:
        ys, xs = np.where(c > 0)
        if len(ys) == 0:
            records.append(struct.pack("<4HI4sII", 0, 0, 0, 0, 0, b"\0\0\0\0", 0, 0))
            continue
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        data = np.ascontiguousarray(c[y0:y1, x0:x1]).tobytes()
        records.append(struct.pack("<4HI4sII", x0, y0, x1 - x0, y1 - y0, 1, b"\0\0\0\0", 0, offset))
        blobs.append(data)
        offset += len(data)
    with open(path, "wb") as out:
        out.write(struct.pack("<4H", 0, W, H, len(canvases)))
        out.write(b"".join(records))
        out.write(b"".join(blobs))


def is_glow(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return (r > 185) & (g > 110) & (b < 140) & (r >= g)


def glow_frames(hd, anim_frames, first, n, damaged, S):
    """HD glow overlay frames: the HD art's glowing pixels, brightness pulsing like the 1x loop."""
    rnd = random.Random(3)
    frames = []
    for i in range(n):
        f = 0.72 + 0.5 * (0.5 + 0.5 * math.sin(2 * math.pi * i / n))
        if damaged:
            f *= rnd.choice((0.55, 0.8, 1.0))
        lowres = anim_frames[first + i]["canvas"] > 0 if first + i < len(anim_frames) else None
        mask = is_glow(hd[..., :3]) & (hd[..., 3] > 32)
        if lowres is not None:
            near = blur(upsample(lowres.astype(np.float32), S), S) > 0.01
            mask &= near
        frame = np.zeros_like(hd)
        frame[..., 0] = np.minimum(255, hd[..., 0] * f)
        frame[..., 1] = np.minimum(255, hd[..., 1] * f)
        frame[..., 2] = np.minimum(255, hd[..., 2] * f * 0.9)
        frame[..., 3] = np.where(mask, hd[..., 3], 0)
        frames.append(frame)
    return frames


def house_mask(frame):
    """Team-colour pixels: true reds, the same test the 1x conversion uses for
    palette slots 16-31 (orange glow is excluded). 0..255 per pixel."""
    r, g, b = frame[..., 0], frame[..., 1], frame[..., 2]
    mask = (r > 70) & (g < 0.36 * r) & (b < 0.42 * r) & (frame[..., 3] > 0)
    return (blur(mask.astype(np.float32), 1) * 255).astype(np.uint8)


def write_pack(path, S, W, H, frames, house=True):
    """Version 2: each present frame is BGRA pixels followed by a team-colour mask plane."""
    with open(path, "wb") as out:
        out.write(b"CHD1" + struct.pack("<5I", 2, S, W, H, len(frames)))
        for frame in frames:
            if frame is None:
                out.write(struct.pack("<I", 0))
                continue
            out.write(struct.pack("<I", 1))
            rgba = np.clip(np.round(frame), 0, 255).astype(np.uint8)
            bgra = rgba[..., [2, 1, 0, 3]]
            out.write(bgra.tobytes())
            mask = house_mask(np.clip(frame, 0, 255)) if house else np.zeros(frame.shape[:2], np.uint8)
            out.write(mask.tobytes())
    print("wrote %s (%d frames, %dx%d)" % (path, len(frames), W * S, H * S))


def preview(path, frame, bg=(96, 116, 50)):
    a = frame[..., 3:4] / 255.0
    img = frame[..., :3] * a + np.array(bg, np.float32) * (1 - a)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--stock-body", required=True)
    ap.add_argument("--body", required=True)
    ap.add_argument("--anim")
    ap.add_argument("--palette", required=True)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", help="pack name (default: body SHP name)")
    ap.add_argument("--hd-damage", action="store_true",
                    help="draw damage states at HD and rebuild the 1x damaged frames from them")
    args = ap.parse_args()
    S = args.scale
    os.makedirs(args.out, exist_ok=True)
    pal = read_pal(args.palette)
    _, _, stock = read_shp(args.stock_body)
    W, H, body = read_shp(args.body)
    hd = fit_source(Image.open(args.source), stock[0]["box"], W, H, S)
    normal = body[0]["canvas"]
    # the 1x silhouette decides coverage; keep HD alpha inside it (with a soft edge)
    cover = blur(upsample((normal > 0).astype(np.float32), S), 1)
    hd[..., 3] = np.minimum(hd[..., 3], np.clip(cover * 1.5, 0, 1) * 255)
    visible = len(body) // 2 if len(body) % 2 == 0 else len(body)
    if args.hd_damage and visible >= 3:
        # Damage drawn at HD; the 1x damaged frames are rebuilt from it so the
        # game's 1x silhouette (holes included) matches the HD art exactly.
        damaged, heavy = hd_damage(hd, 1, 11, S), hd_damage(hd, 2, 23, S)
        allowed = [12] + list(range(32, 200)) + [200, 201, 202, 203]
        canvases = [f["canvas"] for f in body]
        canvases[1] = to_1x_indices(damaged, pal, S, allowed)
        canvases[2] = to_1x_indices(heavy, pal, S, allowed)
        write_shp(args.body, W, H, canvases)
        print("rewrote 1x damaged frames in", args.body)
    else:
        damaged = damaged_variant(hd, normal, body[1]["canvas"], pal, S) if len(body) > 1 else None
        heavy = damaged_variant(hd, normal, body[2]["canvas"], pal, S) if len(body) > 2 else None
    frames = [hd, damaged, heavy][:visible] + [None] * (len(body) - min(3, visible))
    name = args.name or os.path.splitext(os.path.basename(args.body))[0].lower()
    write_pack(os.path.join(args.out, name + ".hdspr"), S, W, H, frames)
    preview(os.path.join(args.out, name + "_hd_preview.png"), hd)
    if damaged is not None:
        preview(os.path.join(args.out, name + "_hd_damaged_preview.png"), damaged)
    if heavy is not None:
        preview(os.path.join(args.out, name + "_hd_heavy_preview.png"), heavy)
    if args.anim:
        aW, aH, anim = read_shp(args.anim)
        assert (aW, aH) == (W, H), "anim canvas must match the body canvas"
        n = len(anim) // 2
        glow = glow_frames(hd, anim, 0, n, False, S) + glow_frames(damaged if damaged is not None else hd, anim, n, n, True, S)
        aname = os.path.splitext(os.path.basename(args.anim))[0].lower()
        write_pack(os.path.join(args.out, aname + ".hdspr"), S, W, H, glow, house=False)


if __name__ == "__main__":
    main()
