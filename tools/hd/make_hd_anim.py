"""Animation (fire, explosion, ...) from artist frames: HD pack + matching 1x SHP.

  python tools/hd/make_hd_anim.py --frames "fire_4k/fire01/frames/*.png" \
      --stock fire01.shp --palette anim.pal --scale 3 --out local_data/hd

--frames   one image per frame, transparent background, drawn on a canvas with
           the same proportions as the stock SHP canvas (any multiple of it);
           the stock anchor (canvas centre) stays where it was
--stock    the original SHP: canvas size and frame count to match
--palette  the palette the game draws this animation with (anim.pal for fires)
--alpha    1x outline threshold (0..1); lower keeps soft edges of the HD art
           visible, since the game draws HD pixels only inside the 1x outline

Writes <out>/art/<name>.shp and <out>/<name>.hdspr.
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image

from make_hd_building import read_pal, read_shp, write_pack, write_shp


def load_frame(path, W, H, S):
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im).astype(np.float32)
    pre = a.copy()
    pre[..., :3] *= pre[..., 3:4] / 255.0
    chans = [np.asarray(Image.fromarray(pre[..., c]).resize((W * S, H * S), Image.LANCZOS)) for c in range(4)]
    out = np.clip(np.stack(chans, -1), 0, 255)
    al = out[..., 3:4] / 255.0
    out[..., :3] = np.where(al > 1e-4, out[..., :3] / np.maximum(al, 1e-4), 0)
    return np.clip(out, 0, 255)


def to_1x(hd, pal, S, allowed, threshold):
    H, W = hd.shape[0] // S, hd.shape[1] // S
    blocks = hd.reshape(H, S, W, S, 4)
    a = blocks[..., 3].mean(axis=(1, 3)) / 255.0
    wsum = blocks[..., 3].sum(axis=(1, 3))[..., None]
    rgb = (blocks[..., :3] * blocks[..., 3:4]).sum(axis=(1, 3)) / np.maximum(wsum, 1e-6)
    idx = np.zeros((H, W), np.uint8)
    on = a >= threshold
    cand = pal[allowed]
    px = rgb[on]
    d = (3 * (px[:, None, 0] - cand[None, :, 0]) ** 2 + 4 * (px[:, None, 1] - cand[None, :, 1]) ** 2
         + 2 * (px[:, None, 2] - cand[None, :, 2]) ** 2)
    idx[on] = np.array(allowed, np.uint8)[np.argmin(d, axis=1)]
    return idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", nargs="+", required=True)
    ap.add_argument("--stock", required=True)
    ap.add_argument("--palette", required=True)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--alpha", type=float, default=0.25)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", help="SHP/pack name (default: stock file name)")
    args = ap.parse_args()
    S = args.scale
    files = []
    for pattern in args.frames:
        files += sorted(glob.glob(pattern)) or [pattern]
    W, H, stock = read_shp(args.stock)
    if len(files) != len(stock):
        raise SystemExit("frame count %d does not match the stock %d" % (len(files), len(stock)))
    fw, fh = Image.open(files[0]).size
    if abs(fw / float(fh) - W / float(H)) > 0.01:
        raise SystemExit("frame canvas %dx%d is not the stock %dx%d shape" % (fw, fh, W, H))
    pal = read_pal(args.palette)
    allowed = [i for i in range(1, 256) if not 16 <= i <= 31]      # never the house remap range
    hd = [load_frame(f, W, H, S) for f in files]
    name = args.name or os.path.splitext(os.path.basename(args.stock))[0].lower()
    os.makedirs(os.path.join(args.out, "art"), exist_ok=True)
    write_shp(os.path.join(args.out, "art", name + ".shp"), W, H, [to_1x(f, pal, S, allowed, args.alpha) for f in hd])
    write_pack(os.path.join(args.out, name + ".hdspr"), S, W, H, hd, house=False)
    print("%s: %d frames, canvas %dx%d (source %dx%d = %.1fx)" % (name, len(hd), W, H, fw, fh, fw / float(W)))


if __name__ == "__main__":
    main()
