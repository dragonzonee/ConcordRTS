"""Review AI-generated 4K art from one or more model output folders.

  python tools/hd/review_art.py --roots F:/Ra2Mod/art_4k F:/Ra2Mod/art_antigravity_4k \
      --inventory F:/Ra2Mod/shp_inventory.csv --exports local_data/resources/mix_exports \
      --out F:/Ra2Mod/art_review [--all]

For every asset folder (<category>/[<side>/]<GAME ID>/) that is new or changed
since the last run (state in <out>/state.json; --all re-checks everything):

- layout: category, file names, numbering, one asset per folder
- game fit: original sprite found, frame count, canvas proportion, transparency,
  minimum size, team-colour red present (buildings) and stray reds
- quality: sharpness (Laplacian energy at a common scale), and for sequences
  frame-size consistency, anchor drift, colour drift; build-up last frame vs
  finished, damaged vs finished alignment
- comparison sheet <out>/compare/<ID>.png: original | each model, when the same
  asset exists in more than one root

Writes <out>/report_<time>.md and <out>/report_<time>.json; prints a summary.
"""
import argparse
import csv
import datetime
import json
import os
import re
import struct
import time

import numpy as np
from PIL import Image, ImageDraw

CATEGORIES = {"buildings", "effects", "icons", "infantry", "interface", "loading"}
SIDES = {"allied", "soviet", "yuri", "civilian"}
MAP_TYPES = ("tem", "sno", "urb", "des", "lun", "ubn")
IMG_EXT = (".png", ".webp", ".jpg", ".jpeg")


# ---------------------------------------------------------------- originals
def read_shp(path):
    data = open(path, "rb").read()
    _, width, height, count = struct.unpack_from("<4H", data, 0)
    frames = []
    for i in range(count):
        x, y, w, h, comp, _, _, offset = struct.unpack_from("<4HI4sII", data, 8 + i * 24)
        canvas = np.zeros((height, width), np.uint8)
        if w and h and offset:
            p, kind = offset, comp & 0xFF
            px = np.zeros((h, w), np.uint8)
            for row in range(h):
                if kind in (2, 3):
                    length = struct.unpack_from("<H", data, p)[0]
                    seg, p = data[p + 2:p + length], p + length
                    if kind == 2:
                        px[row, :min(w, len(seg))] = list(seg[:w])
                    else:
                        col, k = 0, 0
                        while k < len(seg):
                            if seg[k]:
                                if col < w:
                                    px[row, col] = seg[k]
                                col, k = col + 1, k + 1
                            else:
                                col, k = col + seg[k + 1], k + 2
                else:
                    px[row] = list(data[p:p + w])
                    p += w
            canvas[y:y + h, x:x + w] = px
        frames.append(canvas)
    return width, height, frames


def read_pal(path):
    raw = open(path, "rb").read()[:768]
    return np.array([[min(255, c * 4) for c in raw[i * 3:i * 3 + 3]] for i in range(256)], np.uint8)


class Originals:
    def __init__(self, inventory, exports):
        self.exports = exports
        self.rows = {}
        for r in csv.DictReader(open(inventory)):
            self.rows.setdefault(r["name"].upper(), []).append(r)
        # rules IDs (AMRADR, ATESLA, ...) -> their sprite (Image=) name
        self.image_of = {}
        rules = os.path.join(exports, "ra2md", "localmd", "rulesmd.ini")
        if os.path.exists(rules):
            sec = None
            for raw in open(rules, encoding="latin-1"):
                line = raw.split(";")[0].strip()
                if line.startswith("[") and "]" in line:
                    sec = line[1:line.index("]")].strip().upper()
                elif sec and line.upper().startswith("IMAGE="):
                    self.image_of[sec] = line.split("=", 1)[1].strip().upper()
        pals = os.path.join(exports, "palettes")
        self.pal = {k: read_pal(os.path.join(pals, f)) for k, f in
                    (("unit", "unittem.pal"), ("anim", "anim.pal"), ("cameo", "cameo.pal"))
                    if os.path.exists(os.path.join(pals, f))}

    def find(self, game_id):
        """Original record for a game ID; building IDs also match their theater variants."""
        gid = game_id.upper()
        cands = list(self.rows.get(gid, []))
        if len(gid) > 2 and gid[0] in "GNCY":
            for theater in "GTAUDLN":
                variant = gid[0] + theater + gid[2:]
                if variant != gid:
                    cands += self.rows.get(variant, [])
        if not cands:
            return None
        # prefer the generic / temperate copy over snow, urban, desert, lunar variants
        def rank(r):
            f = r["file"].lower()
            return 0 if ("generic" in f or "conquer" in f or "isotemp" in f or "cameo" in f) else 2 if ("snow" in f or "sno" in f) else 1
        return sorted(cands, key=rank)[0]

    def render(self, rec, category, frame=0):
        w, h, frames = read_shp(os.path.join(self.exports, rec["file"]))
        pal = self.pal.get("cameo" if category == "icons" else "anim" if category == "effects" else "unit")
        idx = frames[min(frame, len(frames) - 1)]
        rgba = np.zeros((h, w, 4), np.uint8)
        rgba[..., :3] = pal[idx]
        rgba[..., 3] = np.where(idx > 0, 255, 0)
        return Image.fromarray(rgba, "RGBA"), len(frames), (w, h)


# ---------------------------------------------------------------- image measures
def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA")).astype(np.float32)


def background_kind(a):
    """'alpha' (real transparency), 'white', or 'opaque-other'."""
    if a[..., 3].min() < 250:
        return "alpha"
    corners = np.concatenate([a[:8, :8, :3].reshape(-1, 3), a[:8, -8:, :3].reshape(-1, 3),
                              a[-8:, :8, :3].reshape(-1, 3), a[-8:, -8:, :3].reshape(-1, 3)])
    return "white" if corners.min() > 225 else "opaque-other"


def subject_mask(a, bg):
    if bg == "alpha":
        return a[..., 3] > 128
    rgb = a[..., :3]
    return ~((rgb.min(-1) > 222) & (rgb.max(-1) - rgb.min(-1) < 28))


def bbox(mask):
    ys, xs = np.where(mask)
    if len(ys) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def sharpness(a, mask, target=512):
    """Mean |Laplacian| of luminance inside the subject, after scaling the
    subject's box to `target` px on its long side (comparable across models)."""
    b = bbox(mask)
    if not b:
        return 0.0
    x0, y0, x1, y1 = b
    crop = a[y0:y1, x0:x1]
    k = target / float(max(x1 - x0, y1 - y0))
    im = Image.fromarray(np.clip(crop, 0, 255).astype(np.uint8), "RGBA").resize(
        (max(8, int((x1 - x0) * k)), max(8, int((y1 - y0) * k))), Image.LANCZOS)
    arr = np.asarray(im).astype(np.float32)
    lum = arr[..., :3] @ np.array([0.299, 0.587, 0.114])
    m = arr[..., 3] > 128 if arr[..., 3].min() < 250 else np.ones(lum.shape, bool)
    lap = np.abs(4 * lum[1:-1, 1:-1] - lum[:-2, 1:-1] - lum[2:, 1:-1] - lum[1:-1, :-2] - lum[1:-1, 2:])
    mm = m[1:-1, 1:-1]
    return float(lap[mm].mean()) if mm.any() else 0.0


def red_fraction(a, mask):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    red = (r > 70) & (g < 0.36 * r) & (b < 0.42 * r) & mask
    return float(red.sum()) / max(1, int(mask.sum()))


def iou(m1, m2):
    if m1.shape != m2.shape:
        m2 = np.asarray(Image.fromarray(m2.astype(np.uint8) * 255).resize(m1.shape[::-1])) > 127
    return float((m1 & m2).sum()) / max(1, int((m1 | m2).sum()))


# ---------------------------------------------------------------- asset review
def asset_folders(root):
    """Yield (category, side, game_id, path) for every asset folder under root."""
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if not d.startswith("_")]      # model scratch (_work, ...) is not a delivery
        imgs = [f for f in files if f.lower().endswith(IMG_EXT)]
        if not imgs:
            continue
        rel = os.path.relpath(dirpath, root).replace("\\", "/").split("/")
        cat = rel[0].lower() if rel else ""
        side = rel[1].lower() if cat == "buildings" and len(rel) >= 3 else ""
        yield cat, side, rel[-1].upper(), dirpath, rel


def numbered(files, prefix, digits):
    # at least `digits` digits: frame_99 is followed by frame_100 in long sequences
    pat = re.compile(r"^%s_(\d{%d,})\.(png|webp|jpg|jpeg)$" % (prefix, digits), re.I)
    nums = sorted(int(m.group(1)) for f in files for m in [pat.match(f)] if m)
    return nums


def review_asset(cat, side, gid, path, rel, originals):
    issues, notes, metrics = [], [], {}
    files = sorted(f for f in os.listdir(path) if f.lower().endswith(IMG_EXT))
    others = [f for f in os.listdir(path) if not f.lower().endswith(IMG_EXT)]
    if cat not in CATEGORIES:
        issues.append("unknown category folder '%s'" % "/".join(rel))
    if cat == "buildings" and side not in SIDES:
        issues.append("buildings must be buildings/<allied|soviet|yuri|civilian>/<ID>, got '%s'" % "/".join(rel))
    if others:
        issues.append("extra non-image files: %s" % ", ".join(others[:5]))
    if gid != gid.upper() or not re.match(r"^[A-Z0-9_]+$", gid):
        issues.append("folder name '%s' is not a game ID" % gid)
    rec = originals.find(gid)
    if not rec and gid in originals.image_of and originals.find(originals.image_of[gid]):
        sprite = originals.image_of[gid]
        issues.append("folder named by the unit ID %s; the sprite is %s (rename the folder to %s)" % (gid, sprite, sprite))
        rec = originals.find(sprite)
    elif not rec:
        issues.append("no original sprite named %s (check the ID against shp_inventory.csv)" % gid)
    is_anim_layer = cat == "buildings" and any(f.lower().startswith("frame_") for f in files)
    orig_frames = int(rec["frames"]) if rec else None
    orig_canvas = (int(rec["width"]), int(rec["height"])) if rec else None

    # expected file sets
    seq = []
    if cat in ("effects", "infantry", "interface") or (cat == "buildings" and any(f.lower().startswith("frame_") for f in files)):
        digits = 4 if cat == "infantry" else 2
        nums = numbered(files, "frame", digits)
        seq = ["frame_%0*d" % (digits, n) for n in nums]
        if not nums:
            issues.append("no frame_%s.png files" % ("0000" if digits == 4 else "00"))
        else:
            if nums != list(range(len(nums))):
                missing = sorted(set(range(max(nums) + 1)) - set(nums))[:10]
                issues.append("frame numbers not continuous from 0 (missing %s)" % missing)
            if orig_frames is not None and len(nums) != orig_frames:
                issues.append("%d frames, original has %d" % (len(nums), orig_frames))
        bad = [f for f in files if not re.match(r"^frame_\d+\.", f, re.I)]
        if bad:
            issues.append("unexpected files: %s" % ", ".join(bad[:5]))
    elif cat == "buildings":
        allowed = {"finished", "damaged"} | {"finished_" + m for m in MAP_TYPES}
        stems = {os.path.splitext(f)[0].lower() for f in files}
        bad = [f for f in files if os.path.splitext(f)[0].lower() not in allowed
               and not re.match(r"^buildup_\d{2}$", os.path.splitext(f)[0], re.I)]
        if bad:
            issues.append("unexpected file names: %s" % ", ".join(bad[:6]))
        if side == "civilian":
            if not any(s.startswith("finished_") for s in stems):
                issues.append("civilian buildings need finished_<tem|sno|urb|des|lun|ubn>.png")
        elif "finished" not in stems:
            issues.append("missing finished.png")
        bu = numbered(files, "buildup", 2)
        if bu and bu != list(range(len(bu))):
            issues.append("buildup numbering not continuous from 00")
        if bu and len(bu) < 2:
            issues.append("build-up needs at least 2 frames")
        seq = ["buildup_%02d" % n for n in bu]
    elif cat == "icons" and "icon" not in {os.path.splitext(f)[0].lower() for f in files}:
        issues.append("missing icon.png")
    elif cat == "loading" and "screen" not in {os.path.splitext(f)[0].lower() for f in files}:
        issues.append("missing screen.png")

    def file_for(stem):
        for f in files:
            if os.path.splitext(f)[0].lower() == stem.lower():
                return os.path.join(path, f)
        return None

    # main image checks
    main_stem = {"icons": "icon", "loading": "screen"}.get(cat)
    if cat == "buildings":
        main_stem = "finished" if file_for("finished") else next((os.path.splitext(f)[0] for f in files if f.lower().startswith("finished_")), None)
    if (not main_stem or is_anim_layer) and seq:
        # the frame with the most content (some original frames are empty on purpose)
        def content(stem):
            arr = load_rgba(file_for(stem))
            return int(subject_mask(arr, background_kind(arr)).sum())
        main_stem = max(seq[:: max(1, len(seq) // 8)], key=content)
    main = file_for(main_stem) if main_stem else None
    main_mask = None
    if main:
        a = load_rgba(main)
        bg = background_kind(a)
        mask = subject_mask(a, bg)
        main_mask = mask
        b = bbox(mask)
        metrics.update(size="%dx%d" % (a.shape[1], a.shape[0]), background=bg,
                       sharpness=round(sharpness(a, mask), 2))
        if bg == "opaque-other":
            issues.append("%s: background is neither transparent nor white" % os.path.basename(main))
        if b:
            bw, bh = b[2] - b[0], b[3] - b[1]
            metrics["subject"] = "%dx%d" % (bw, bh)
            if cat == "buildings" and not is_anim_layer and max(bw, bh) < 1500:
                issues.append("building is %d px across, needs >= 1500" % max(bw, bh))
            if cat == "icons" and a.shape[1] < 600:
                issues.append("icon is %d px wide, needs >= 600" % a.shape[1])
            if cat == "loading" and (a.shape[1] < 2400 or a.shape[0] < 1800):
                issues.append("screen is %dx%d, needs >= 2400x1800" % (a.shape[1], a.shape[0]))
            edge = 2
            if bg != "opaque-other" and (b[0] < edge or b[1] < edge or b[2] > a.shape[1] - edge or b[3] > a.shape[0] - edge) \
                    and cat in ("buildings", "effects", "infantry"):
                issues.append("%s: subject touches the image edge (cropped?)" % os.path.basename(main))
        else:
            issues.append("%s: empty image" % os.path.basename(main))
        if cat == "buildings" and not is_anim_layer:
            rf = red_fraction(a, mask)
            metrics["team_red"] = round(rf, 4)
            if side != "civilian" and rf < 0.005:
                notes.append("almost no pure red: team colour will not show (paint owner-colour parts pure red)")
        if cat in ("effects", "infantry", "interface") and orig_canvas:
            ar, oar = a.shape[1] / float(a.shape[0]), orig_canvas[0] / float(orig_canvas[1])
            if abs(ar - oar) > 0.01 * oar:
                issues.append("canvas %dx%d is not the original %dx%d proportion" % (a.shape[1], a.shape[0], *orig_canvas))
            elif a.shape[1] % orig_canvas[0]:
                notes.append("canvas is not a whole multiple of the original (%.2fx); fine, but whole multiples fit best" % (a.shape[1] / float(orig_canvas[0])))
    # sequence consistency
    if len(seq) >= 2:
        sizes, bottoms, centres, means, sharp, areas = set(), [], [], [], [], []
        masks = []
        orig_frames_px = None
        if rec and seq[0].startswith("frame_"):
            try:
                orig_frames_px = read_shp(os.path.join(originals.exports, rec["file"]))[2]
            except Exception:
                orig_frames_px = None
        empty_bad = []
        for s in seq:
            fa = load_rgba(file_for(s))
            bg = background_kind(fa)
            m = subject_mask(fa, bg)
            sizes.add(fa.shape[:2])
            bx = bbox(m)
            if orig_frames_px is not None:
                n = int(s.split("_")[1])
                if n < len(orig_frames_px):
                    if not m.any() and orig_frames_px[n].any():
                        empty_bad.append(n)
            if bx:
                bottoms.append(bx[3] / float(fa.shape[0]))
                ys, xs = np.where(m)
                centres.append((xs.mean() / fa.shape[1], ys.mean() / fa.shape[0]))
                means.append(fa[..., :3][m].mean(axis=0))
                sharp.append(sharpness(fa, m))
                areas.append(int(m.sum()))
            masks.append(m)
        if len(sizes) > 1:
            issues.append("frames have different sizes: %s" % sorted(sizes)[:3])
        if empty_bad:
            issues.append("frames empty where the original has content: %s" % empty_bad[:10])
        if sharp:
            # judge only frames with a real subject (tiny first/last effect frames are soft by nature)
            big = [s for s, ar in zip(sharp, areas) if ar >= 0.25 * max(areas)]
            metrics["seq_sharpness_min_max"] = "%.1f/%.1f" % (min(big), max(big))
            if min(big) < 0.5 * max(big):
                notes.append("sharpness varies a lot between frames (%.1f .. %.1f): some frames are blurry" % (min(big), max(big)))
        if cat == "buildings" and seq[0].startswith("buildup") and len(bottoms) > 1:
            drift = (max(bottoms) - min(bottoms)) * 100
            metrics["pad_drift_pct"] = round(drift, 2)
            if drift > 2.5:
                issues.append("build-up pad moves between frames (bottom edge drifts %.1f%% of height)" % drift)
            if main_mask is not None:
                last_iou = iou(main_mask, masks[-1])
                metrics["buildup_last_vs_finished_iou"] = round(last_iou, 3)
                if last_iou < 0.9:
                    issues.append("last build-up frame does not match finished.png (outline overlap %.2f, want >= 0.9)" % last_iou)
        if len(means) > 2:
            spread = float(np.max(np.std(np.array(means), axis=0)))
            metrics["colour_spread"] = round(spread, 1)
    # damaged / map types vs finished
    if cat == "buildings" and main_mask is not None:
        for stem in ["damaged"] + ["finished_" + m for m in MAP_TYPES]:
            f = file_for(stem)
            if f and stem != main_stem:
                da = load_rgba(f)
                m = subject_mask(da, background_kind(da))
                v = iou(main_mask, m)
                metrics["%s_iou" % stem] = round(v, 3)
                if v < 0.85:
                    issues.append("%s does not line up with %s (overlap %.2f, want >= 0.85)" % (os.path.basename(f), main_stem, v))
    return dict(category=cat, side=side, id=gid, path=path, files=len(files), original=rec["file"] if rec else None,
                original_frames=orig_frames, issues=issues, notes=notes, metrics=metrics, main=main)


def folder_stamp(path):
    return max([os.path.getmtime(os.path.join(path, f)) for f in os.listdir(path)] + [0])


def comparison_sheet(gid, entries, originals, out):
    """Original (enlarged) next to each model's main image, same height."""
    tiles = []
    first = entries[0]
    if first["original"]:
        try:
            im, _, _ = originals.render(originals.find(gid), first["category"])
            tiles.append(("original", im))
        except Exception:
            pass
    for root, e in entries_by_root(entries):
        if e["main"]:
            tiles.append((os.path.basename(root), Image.open(e["main"]).convert("RGBA")))
    if len(tiles) < 2:
        return None
    H = 520
    scaled = []
    for label, im in tiles:
        k = H / float(im.height)
        scaled.append((label, im.resize((max(1, int(im.width * k)), H), Image.NEAREST if label == "original" else Image.LANCZOS)))
    W = sum(i.width for _, i in scaled) + 20 * (len(scaled) + 1)
    sheet = Image.new("RGBA", (W, H + 50), (56, 60, 52, 255))
    d = ImageDraw.Draw(sheet)
    x = 20
    for label, im in scaled:
        sheet.alpha_composite(im, (x, 40))
        d.text((x, 12), label, fill=(235, 235, 235, 255))
        x += im.width + 20
    os.makedirs(os.path.join(out, "compare"), exist_ok=True)
    p = os.path.join(out, "compare", gid + ".png")
    sheet.convert("RGB").save(p)
    return p


def entries_by_root(entries):
    return [(e["root"], e) for e in entries]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="+", required=True)
    ap.add_argument("--inventory", required=True)
    ap.add_argument("--exports", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    state_path = os.path.join(args.out, "state.json")
    state = json.load(open(state_path)) if os.path.exists(state_path) and not args.all else {}
    originals = Originals(args.inventory, args.exports)
    results, by_id, totals = [], {}, {}
    for root in args.roots:
        totals[root] = 0
        if not os.path.isdir(root):
            continue
        for cat, side, gid, path, rel in asset_folders(root):
            totals[root] += 1
            key = os.path.relpath(path, os.path.dirname(root))
            stamp = folder_stamp(path)
            if state.get(key) == stamp:
                continue
            if time.time() - stamp < 90:
                continue          # still being written: review it next run
            try:
                r = review_asset(cat, side, gid, path, rel, originals)
            except (OSError, ValueError, SyntaxError) as e:
                print("skipped %s (unreadable, retry next run): %s" % (key, e))
                continue
            r["root"] = root
            results.append(r)
            by_id.setdefault(gid, []).append(r)
            state[key] = stamp
    # comparisons: an asset that exists (now) in more than one root
    for gid, entries in by_id.items():
        all_entries = []
        for root in args.roots:
            match = [e for e in entries if e["root"] == root]
            if match:
                all_entries.append(match[0])
            else:
                for cat, side, g, path, rel in (asset_folders(root) if os.path.isdir(root) else []):
                    if g == gid or originals.image_of.get(g) == gid or originals.image_of.get(gid) == g:
                        try:
                            e = review_asset(cat, side, g, path, rel, originals)
                        except (OSError, ValueError, SyntaxError):
                            break
                        e["root"] = root
                        all_entries.append(e)
                        break
        if len(all_entries) > 1:
            p = comparison_sheet(gid, all_entries, originals, args.out)
            for e in all_entries:
                e["compare"] = p
            by_id[gid] = all_entries
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M")
    lines = ["# Art review %s" % stamp, ""]
    for root in args.roots:
        lines.append("- %s: %d asset folders (%s)" % (root, totals.get(root, 0), "exists" if os.path.isdir(root) else "missing"))
    lines.append("- reviewed this run: %d" % len(results))
    lines.append("")
    for gid, entries in sorted(by_id.items()):
        lines.append("## %s" % gid)
        for e in entries:
            lines.append("**%s** (%s/%s, %d files, original %s, %s frames)" % (
                os.path.basename(e["root"]), e["category"], e["side"] or "-", e["files"], e["original"], e["original_frames"]))
            lines.append("- metrics: " + ", ".join("%s=%s" % kv for kv in e["metrics"].items()))
            for i in e["issues"]:
                lines.append("- ISSUE: " + i)
            for n in e["notes"]:
                lines.append("- note: " + n)
        if entries[0].get("compare"):
            lines.append("- comparison: %s" % entries[0]["compare"])
        lines.append("")
    md = os.path.join(args.out, "report_%s.md" % stamp)
    open(md, "w", encoding="utf-8").write("\n".join(lines))
    json.dump([{k: v for k, v in r.items()} for r in results], open(os.path.join(args.out, "report_%s.json" % stamp), "w"), indent=1, default=str)
    json.dump(state, open(state_path, "w"), indent=1)
    issues = sum(len(r["issues"]) for r in results)
    print("roots: %s" % ", ".join("%s=%d" % (os.path.basename(k), v) for k, v in totals.items()))
    print("reviewed %d asset folders, %d issues, report %s" % (len(results), issues, md))


if __name__ == "__main__":
    main()
