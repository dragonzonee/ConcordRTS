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

## Known gaps

- The live window still shows the HD frame scaled down to the window size;
  a full-screen 4K window needs input coordinates scaled too.
- HD art ignores team colour (the 1x sprite remaps the house colour range).
- Build-up frames stay 1x; the GPU map path is slow at 4K (a few FPS).
