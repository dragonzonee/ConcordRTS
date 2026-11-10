"""Build-up animation from artist frames: HD pack + matching 1x SHP.

  python tools/hd/make_hd_buildup.py --frames "buildup_source/buildup_*.webp" \
      --stock-body ggpowr.stock.shp --buildup local_data/hd/art/gtpowrmk.shp \
      --palette unittem.pal --scale 3 --out local_data/hd

--frames      the construction frames in order (glob or list); the last one is
              the finished building. White or transparent background.
--stock-body  stock body SHP the finished building is fitted to (same fit as the
              body conversion, so the last build-up frame lines up with the body)
--buildup     the 1x build-up SHP to replace; its canvas, frame count and shadow
              frames (second half) are kept. Frames are spread over its count.

All frames share one placement/scale (taken from the last frame): the camera is
fixed, so the pad stays put while the plant grows.
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image, ImageDraw

from make_hd_building import read_pal, read_shp, write_pack, write_shp, to_1x_indices, blur


def cutout(path):
    """RGBA with a white (or near-white, low-saturation) border-connected
    background removed; soft alpha on the anti-aliased edge."""
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im).astype(np.float32)
    if a[..., 3].min() < 250:                  # already transparent
        return a
    rgb = a[..., :3]
    lo, hi = rgb.min(-1), rgb.max(-1)
    candidate = ((lo > 222) & (hi - lo < 28)).astype(np.uint8) * 255
    mask = Image.fromarray(candidate, "L")
    h, w = candidate.shape
    seeds = [(x, y) for x in range(0, w, 50) for y in (0, h - 1)] + [(x, y) for y in range(0, h, 50) for x in (0, w - 1)]
    for s in seeds:
        if mask.getpixel(s) == 255:
            ImageDraw.floodfill(mask, s, 128)
    bg = np.asarray(mask) == 128
    near = blur(bg.astype(np.float32), 2) > 0.01            # edge band next to the background
    alpha = np.where(bg, 0.0, 1.0)
    edge = near & ~bg
    alpha[edge] = np.clip((250 - lo[edge]) / 70.0, 0, 1)     # light fringe -> partly transparent
    # un-mix the white fringe: c = (observed - (1-a)*255) / a
    out = a.copy()
    safe = np.maximum(alpha, 1e-3)[..., None]
    out[..., :3] = np.where(edge[..., None], np.clip((rgb - (1 - alpha[..., None]) * 255) / safe, 0, 255), rgb)
    out[..., 3] = alpha * 255
    return out


def bbox(alpha, thresh=128):
    ys, xs = np.where(alpha >= thresh)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def place(rgba, src_origin, scale, origin, W, H, S):
    """Resample source pixels into a (W*S x H*S) canvas: source point src_origin
    maps to canvas point origin (game pixels), `scale` game px per source px."""
    x0, y0 = src_origin
    pre = rgba.copy()
    pre[..., :3] *= pre[..., 3:4] / 255.0
    k = scale * S
    sw, sh = rgba.shape[1], rgba.shape[0]
    rw, rh = max(1, int(round(sw * k))), max(1, int(round(sh * k)))
    chans = [np.asarray(Image.fromarray(pre[..., c]).resize((rw, rh), Image.LANCZOS)) for c in range(4)]
    small = np.clip(np.stack(chans, -1), 0, 255)
    # source origin lands at canvas (origin*S); shift by integer + bilinear fraction
    px, py = origin[0] * S - x0 * k, origin[1] * S - y0 * k
    ix, iy = int(np.floor(px)), int(np.floor(py))
    fx, fy = px - ix, py - iy
    shifted = np.zeros((rh + 1, rw + 1, 4), np.float32)
    shifted[:rh, :rw] += small * (1 - fx) * (1 - fy)
    shifted[:rh, 1:] += small * fx * (1 - fy)
    shifted[1:, :rw] += small * (1 - fx) * fy
    shifted[1:, 1:] += small * fx * fy
    out = np.zeros((H * S, W * S, 4), np.float32)
    y_lo, x_lo = max(0, iy), max(0, ix)
    y_hi, x_hi = min(H * S, iy + rh + 1), min(W * S, ix + rw + 1)
    if y_hi > y_lo and x_hi > x_lo:
        out[y_lo:y_hi, x_lo:x_hi] = shifted[y_lo - iy:y_hi - iy, x_lo - ix:x_hi - ix]
    a = out[..., 3:4] / 255.0
    out[..., :3] = np.where(a > 1e-4, out[..., :3] / np.maximum(a, 1e-4), 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", nargs="+", required=True)
    ap.add_argument("--stock-body", required=True)
    ap.add_argument("--buildup", required=True)
    ap.add_argument("--palette", required=True)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", help="write a contact sheet of the HD frames here")
    args = ap.parse_args()
    S = args.scale
    files = []
    for pattern in args.frames:
        files += sorted(glob.glob(pattern)) or [pattern]
    pal = read_pal(args.palette)
    _, _, stock = read_shp(args.stock_body)
    W, H, mk = read_shp(args.buildup)
    visible = len(mk) // 2
    print("%d source frames -> %d build-up frames on a %dx%d canvas" % (len(files), visible, W, H))
    frames = [cutout(f) for f in files]

    # one placement for all frames, from the finished building (same formula as the body)
    x0, y0, x1, y1 = bbox(frames[-1][..., 3])
    bw, bh = x1 - x0, y1 - y0
    fx, fy, fw, fh = stock[0]["box"]
    scale = (fw / float(bw) + fh / float(bh)) / 2.0
    ox = fx + (fw - bw * scale) / 2.0
    oy = fy + fh - bh * scale
    print("fit from last frame: box %dx%d, scale %.4f, origin %.2f,%.2f" % (bw, bh, scale, ox, oy))
    hd_frames = [place(f, (x0, y0), scale, (ox, oy), W, H, S) for f in frames]

    # spread the source frames over the game's frame count (hold frames evenly)
    n = len(hd_frames)
    pick = [min(n - 1, int(round(i * (n - 1) / float(visible - 1)))) for i in range(visible)]
    hd_out = [hd_frames[p] for p in pick]
    allowed = [12] + list(range(32, 200)) + [200, 201, 202, 203]
    one_x = {p: to_1x_indices(hd_frames[p], pal, S, allowed) for p in set(pick)}
    canvases = [one_x[p] for p in pick] + [f["canvas"] for f in mk[visible:]]
    write_shp(args.buildup, W, H, canvases)
    name = os.path.splitext(os.path.basename(args.buildup))[0].lower()
    write_pack(os.path.join(args.out, name + ".hdspr"), S, W, H, hd_out + [None] * (len(mk) - visible))
    print("rewrote", args.buildup, "frames:", pick)
    if args.preview:
        cols = 8
        rows = (n + cols - 1) // cols
        sheet = Image.new("RGB", (cols * W * S, rows * H * S), (96, 116, 50))
        for i, f in enumerate(hd_frames):
            a = f[..., 3:4] / 255.0
            img = f[..., :3] * a + np.array([96, 116, 50], np.float32) * (1 - a)
            sheet.paste(Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)), ((i % cols) * W * S, (i // cols) * H * S))
        sheet.save(args.preview)
        print("preview", args.preview)


if __name__ == "__main__":
    main()
