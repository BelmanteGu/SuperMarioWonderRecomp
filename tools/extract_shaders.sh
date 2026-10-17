#!/usr/bin/env bash
# Extracts every Maxwell shader container of Wonder's RomFS, plus the shader blocks embedded in `main`
# (docs/nvn/SHADERS.md, "Where the shaders are"; docs/BUILD.md, step 6). Run it from anywhere.
#   usage: tools/extract_shaders.sh [wonder dir]          (default: private/wonder)
#   env:   PY=<python with zstandard>   (default: private/venv/bin/python, else python3)
#          JOBS=<parallel extractions>  (default: half the cores, at least 1)
# Output: <wonder dir>/shaders_extracted/<group>/..., one group per container; the end line should read
# "groups: 141  blocks: ..." for the 1.0.0 dump.
set -u
cd "$(dirname "$0")/.." || exit 1
W=${1:-private/wonder}
PY=${PY:-}
if [ -z "$PY" ]; then if [ -x private/venv/bin/python ]; then PY=private/venv/bin/python; else PY=python3; fi; fi
JOBS=${JOBS:-$(( $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) / 2 ))}
[ "$JOBS" -ge 1 ] || JOBS=1
X=tools/maxwell/containers.py
R=$W/extract/romfs
O=$W/shaders_extracted
[ -d "$R" ] || { echo "no $R: import the game first (docs/BUILD.md, step 2)"; exit 1; }
mkdir -p "$O"
# One line per group: "<group> <container>".
{
  echo "uber $R/Shader/SecredUber.Product.100.product.Nin_NX_NVN.bfsha.zs"
  echo "secred $R/Shader/Secred.Nin_NX_NVN.release.sarc.zs"
  echo "agl $R/Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs"
  echo "gsys $R/Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc.zs"
  # 33 effect files. EffectFileInfo.*.byml.zs has no shaders and does not match this pattern.
  for f in "$R"/Effect/*.Nin_NX_NVN.esetb.byml.zs; do
    b=$(basename "$f" .Nin_NX_NVN.esetb.byml.zs); echo "effect_$b $f"
  done
  # 234 layout archives, 103 of them hold shaders; the others log "no shader container recognised" (harmless).
  for f in "$R"/Layout/*.Nin_NX_NVN.blarc.zs; do
    b=$(basename "$f" .Nin_NX_NVN.blarc.zs); echo "layout_$b $f"
  done
} | xargs -P "$JOBS" -L 1 sh -c \
    '"$1" "$2" "$5" --extract "$3/$4" --unique > "$3/$4.log" 2>&1 || echo "FAIL $4"' sh "$PY" "$X" "$O"
# Layouts without shaders leave an empty folder: drop them.
for d in "$O"/layout_*/; do
  [ -n "$(ls -A "$d" 2>/dev/null | grep -v manifest.tsv)" ] || rm -rf "$d"
done
# The code blocks embedded in `main` that no container holds. After the others: it skips what they already have.
"$PY" tools/maxwell/exefs_shaders.py "$W/analysis/main.bin" --extract "$O/exefs" > "$O/exefs.log" 2>&1 || echo "FAIL exefs"
echo "groups: $(ls -d "$O"/*/ | wc -l)  blocks: $(find "$O" -name '*.code.bin' -o -name 'code_*.bin' | wc -l)"
