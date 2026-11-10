#!/bin/bash
# Import art_4k structures into local_data/hd (GAPOWR kept: user's own art already there; walls skipped)
cd /f/Dev/ConcordRTS
SRC=/f/Ra2Mod/art_4k/buildings/allied
EXP=local_data/resources/mix_exports/ra2
PAL=local_data/resources/mix_exports/palettes/unittem.pal
OUT=local_data/hd
# folder:sprite
for pair in GACNST:cnst GACSPH:csph GADEPT:dept GAGAP:gap GAOREP:orep GAPILE:pile GAPILL:pill GAREFN:refn GAROBO:robo GATECH:tech GAWEAP:weap GAYARD:yard GTGCAN:gcan GAAIRC:airc ATESLA:pris GASPYSAT:spst GAWEAT:weth; do
  f=${pair%%:*}; b=${pair##*:}
  stock=$(find $EXP local_data/resources/mix_exports/ra2md/genermd -iname "gg${b}.shp" | head -1)
  [ -z "$stock" ] && { echo "$f: no stock body"; continue; }
  cp "$stock" $OUT/art/gg${b}.shp
  if python tools/hd/make_hd_building.py --source $SRC/$f/finished.png --stock-body "$stock" --body $OUT/art/gg${b}.shp --palette $PAL --scale 3 --out $OUT --rebuild-1x > /tmp/imp_$f.log 2>&1; then r="body ok"; else r="body FAIL: $(tail -1 /tmp/imp_$f.log)"; fi
  if ls $SRC/$f/buildup_*.png >/dev/null 2>&1; then
    mk=$(find $EXP/isotemp local_data/resources/mix_exports/ra2md/genermd -iname "g[gt]${b}mk.shp" | head -1)
    name=$(basename "$mk" | tr A-Z a-z)
    cp "$mk" $OUT/art/$name
    if python tools/hd/make_hd_buildup.py --frames $SRC/$f/buildup_*.png --stock-body "$stock" --buildup $OUT/art/$name --palette $PAL --scale 3 --out $OUT >> /tmp/imp_$f.log 2>&1; then r="$r; buildup $name ok"; else r="$r; buildup FAIL: $(tail -1 /tmp/imp_$f.log)"; fi
  fi
  echo "$f -> gg$b: $r"
done
