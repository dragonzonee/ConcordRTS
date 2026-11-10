"""Sidebar icon (cameo) with new building art on the original background.

  python tools/hd/make_hd_cameo.py --cameo powricon.shp --palette cameo.pal \
      --art finished.png --box 15 2 45 42 --scale 3 --out local_data/hd

--cameo    original 60x48 cameo SHP (its background, corners and colours stay)
--art      the building, transparent background
--box      x0 y0 x1 y1 of the old building in the cameo (game pixels); the old
           building is removed there and the new one fitted into the box
           (bottom-centre aligned)

Writes <out>/art/<name>.shp (1x, cameo palette; original pixels kept outside the
changed area) and <out>/<name>.hdspr (scale x size, for HdScale rendering).
"""
import argparse
import os

import numpy as np
from PIL import Image

from make_hd_building import read_shp, read_pal, write_pack, write_shp, blur, fractal_noise


def harmonic_fill(img, hole, iterations=600):
    """Fill `hole` pixels smoothly from their surroundings (Jacobi relaxation)."""
    out = img.copy()
    known = ~hole
    mean = out[known].mean(axis=0)
    out[hole] = mean
    for _ in range(iterations):
        p = np.pad(out, ((1, 1), (1, 1), (0, 0)), mode="edge")
        avg = (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:]) / 4.0
        out[hole] = avg[hole]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cameo", required=True)
    ap.add_argument("--palette", required=True)
    ap.add_argument("--art", required=True)
    ap.add_argument("--box", type=int, nargs=4, required=True)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label-top", type=int, default=35, help="first row of the baked-in name label")
    ap.add_argument("--preview")
    args = ap.parse_args()
    S = args.scale
    pal = read_pal(args.palette)
    W, H, frames = read_shp(args.cameo)
    idx = frames[0]["canvas"]
    orig = pal[idx].astype(np.float32)                       # 1x original colours
    bx0, by0, bx1, by1 = args.box

    # --- the label baked into localized cameos: near-white glyphs in the bottom
    # rows plus their dark outline; laid back on top at the end, unchanged
    r, g, b = orig[..., 0], orig[..., 1], orig[..., 2]
    lum1 = orig.mean(-1)
    glyph = (lum1 > 185) & (orig.max(-1) - orig.min(-1) < 45)
    glyph[:args.label_top] = False
    label = glyph | (blur(glyph.astype(np.float32), 1) > 0.01) & (lum1 < 70)
    # --- the old building: cool/metallic or dark pixels inside the box (the
    # desert background is warm: r > g > b)
    inbox = np.zeros((H, W), bool)
    inbox[by0:by1, bx0:bx1] = True
    old = inbox & ((b >= r - 6) | (lum1 < 55) | (np.abs(r - b) < 14))
    old = (blur(old.astype(np.float32), 1) > 0.15) & inbox & ~label
    # --- background at HD: upscale, then remove the old building
    bg = np.asarray(Image.fromarray(orig.astype(np.uint8)).resize((W * S, H * S), Image.LANCZOS)).astype(np.float32)
    hole = np.repeat(np.repeat(old, S, 0), S, 1)
    hole = blur(hole.astype(np.float32), 1) > 0.05
    rng = np.random.default_rng(5)
    filled = harmonic_fill(bg, hole)
    # put back the desert grain: high-pass texture from the untouched area, tiled
    texture = bg - np.asarray(Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8)).resize(
        (W * S // 4, H * S // 4), Image.BILINEAR).resize((W * S, H * S), Image.BILINEAR)).astype(np.float32)
    grain = (fractal_noise(H * S, W * S, 3, rng, 3)[..., None] - 0.5) * 18
    filled[hole] += (np.roll(texture, (bx1 - bx0) * S, axis=1)[hole] * 0.8 + grain[hole])
    background = np.clip(filled, 0, 255)

    # --- the building, fitted into the box (bottom-centre), toned to the icon
    art = np.asarray(Image.open(args.art).convert("RGBA")).astype(np.float32)
    ys, xs = np.where(art[..., 3] >= 128)
    crop = art[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    bw, bh = crop.shape[1], crop.shape[0]
    tw, th = (bx1 - bx0) * S, (by1 - by0) * S
    k = min(tw / bw, th / bh)
    rw, rh = max(1, int(round(bw * k))), max(1, int(round(bh * k)))
    pre = crop.copy()
    pre[..., :3] *= pre[..., 3:4] / 255.0
    small = np.stack([np.asarray(Image.fromarray(pre[..., c]).resize((rw, rh), Image.LANCZOS)) for c in range(4)], -1)
    small = np.clip(small, 0, 255)
    a = small[..., 3:4] / 255.0
    rgb = np.where(a > 1e-3, small[..., :3] / np.maximum(a, 1e-3), 0)
    lum = rgb.mean(-1, keepdims=True)
    rgb = lum + (rgb - lum) * 0.88                                   # slightly muted, like the stock icons
    rgb = rgb * np.array([1.02, 1.0, 0.95])                          # warm cast of the desert light
    px = bx0 * S + (tw - rw) // 2
    py = by1 * S - rh
    canvas = background.copy()
    # soft ground shadow under the building
    sy, sx = np.mgrid[0:H * S, 0:W * S]
    cx, cy = px + rw / 2.0, by1 * S - rh * 0.06
    d = ((sx - cx) / (rw * 0.55)) ** 2 + ((sy - cy) / (rh * 0.14)) ** 2
    canvas *= (1 - 0.45 * np.clip(1 - d, 0, 1))[..., None]
    region = canvas[py:py + rh, px:px + rw]
    region[:] = rgb * a + region * (1 - a)
    # label back on top, crisp (nearest-neighbour from the original pixels)
    label_hd = np.repeat(np.repeat(label, S, 0), S, 1)
    orig_hd = np.repeat(np.repeat(orig, S, 0), S, 1)
    canvas[label_hd] = orig_hd[label_hd]
    hd = np.clip(canvas, 0, 255)

    # --- 1x: area average, cameo palette (index 0 is transparent: never use it);
    # original pixels stay where nothing changed
    small1 = hd.reshape(H, S, W, S, 3).mean(axis=(1, 3))
    cand = pal[1:]
    d = ((small1[..., None, :] - cand[None, None]) ** 2 * np.array([3, 4, 2])).sum(-1)
    new_idx = (np.argmin(d, axis=-1) + 1).astype(np.uint8)
    changed = np.zeros((H, W), bool)
    changed[max(0, by0 - 1):by1 + 1, max(0, bx0 - 1):bx1 + 1] = True
    changed &= ~label                                        # the label keeps its original pixels
    out_idx = np.where(changed, new_idx, idx)

    name = os.path.splitext(os.path.basename(args.cameo))[0].lower()
    os.makedirs(os.path.join(args.out, "art"), exist_ok=True)
    write_shp(os.path.join(args.out, "art", name + ".shp"), W, H, [out_idx])
    rgba = np.concatenate([hd, np.full((H * S, W * S, 1), 255, np.float32)], -1)
    write_pack(os.path.join(args.out, name + ".hdspr"), S, W, H, [rgba], house=False)
    print("wrote", os.path.join(args.out, "art", name + ".shp"), "and", name + ".hdspr")
    if args.preview:
        one = Image.fromarray(pal[out_idx].astype(np.uint8)).resize((W * S * 2, H * S * 2), Image.NEAREST)
        two = Image.fromarray(hd.astype(np.uint8)).resize((W * S * 2, H * S * 2), Image.LANCZOS)
        old = Image.fromarray(orig.astype(np.uint8)).resize((W * S * 2, H * S * 2), Image.NEAREST)
        sheet = Image.new("RGB", (W * S * 6 + 40, H * S * 2), (30, 30, 30))
        for i, im in enumerate((old, one, two)):
            sheet.paste(im, (i * (W * S * 2 + 20), 0))
        sheet.save(args.preview)


if __name__ == "__main__":
    main()
