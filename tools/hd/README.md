# HD sprites (4K art for one building at a time)

`Runtime HdScale=S` (2..4) renders the battle map S times larger on the GPU.
Every existing draw keeps working on game pixels (each becomes an S x S block);
sprites that have an **HD pack** are sampled at full resolution instead, so one
game pixel shows S x S real pixels of the source art. The 1x SHP still decides
silhouette, depth sorting, selection and shadows, so gameplay is unchanged.

Code: `src/render/hd/RuntimeHdSprites.h` (packs, art override, capture),
`MapRaster.hlsl` (`spriteHd`), `MapRasterD3D11.inl` / `D3D11MapPresentWin32.inl`
(S-times back buffer).

## Files (all under `local_data/hd/`, not in Git: they are game art)

| File | Purpose |
| --- | --- |
| `art/<name>.shp` | Replacement 1x SHP; wins over the copy in the game archives |
| `<name>.hdspr` | HD pack for the SHP `<name>`: one true-colour image per frame |

## Making the Power Plant example

1x art first (palette-quantised from the source image, same frame layout as
stock), from the Ra2Mod tools: `modtools/enhance_building.py GAPOWR source.png`
gives `gapowr.shp`, `gapowr_a.shp` (glow) and `gapowrmk.shp` (build-up). In
ConcordRTS the stock names are `ggpowr`, `ggpowr_a`, `gtpowrmk`; copy them to
`local_data/hd/art/` under those names. Then the HD pack:

```
python tools/hd/make_hd_building.py --source source.png ^
  --stock-body <stock ggpowr.shp> --body local_data/hd/art/ggpowr.shp ^
  --anim local_data/hd/art/ggpowr_a.shp ^
  --palette local_data/resources/mix_exports/palettes/unittem.pal --scale 3 --out local_data/hd
```

`--stock-body` is the original SHP the 1x conversion was fitted to (export it
from `ra2/generic.mix`), so the HD art lines up with the 1x frames exactly.
Damage states copy the 1x damage (scorch ratio and holes); the glow loop takes
the HD art's glowing pixels with the same pulse as the 1x loop.

## Checking it

`tests/autoplay/hd_power_plant.ini` builds two Power Plants at `HdScale=3`
and saves `power_plants_hd.bmp`, the full 3840x2160 frame.

## Playing in 4K

`[Runtime] HdScale=3` in `local_data/runtime_config.ini` (1280x720 x 3 =
3840x2160): the battle window becomes borderless full screen; menus keep their
size. Team colour: the pack's mask plane marks house-colour pixels, which take
the owner's remap colour (palette slots 16-31) at the art's brightness.

## Performance notes

The GPU map path rebuilds a GDI "overlay" (UI, text, bars) for the GPU every
frame. It used to cost 50-350 ms: an exact 256-pass reconstruction plus big
sidebar/radar blits and per-pixel `SetPixelV` cameos embedded in the overlay
metafile. Now: 2-pass reconstruction when HD is on (`ExactOverlay=yes` restores
the exact one), and opaque/translucent images are sent as GPU commands. A 4K
battle runs ~35 FPS (vsync at 60 Hz needs < 16.7 ms; today ~26 ms: map prep 6,
overlay 5, upload 3, present 2). `Profile N` in an autoplay script measures it;
`CONCORD_NATIVE_PROFILE=1` prints overlay compile details.

## Known gaps

- Build-up frames stay 1x.
- 60 FPS needs sprite materials and GPU resources cached across frames.
