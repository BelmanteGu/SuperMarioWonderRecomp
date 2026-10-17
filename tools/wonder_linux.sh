#!/bin/bash
# Run Wonder on Linux with a quality preset. usage: tools/wonder_linux.sh [quality|balanced|performance] [build dir]
#   quality      docked 1080p, plain scaling to the window                                          [default]
#   balanced     the game renders 720p (handheld mode) and FSR 1 upscales to the window
#   performance  as balanced with softer FSR sharpening (NX_FSR_SHARP=1.0)
#   intel        integrated GPU (Intel UHD / Mesa ANV): handheld 720p, pipelined submits, the desktop's performance power
#                profile held while it runs; the game's own 60 fps mode (docs/nvn/studies/intel-60fps-1710.md)
#   lightest     as intel plus FSR 1 and softer sharpening
# The discrete GPU is used unless NX_GPU says otherwise (laptops with Intel + NVIDIA: PRIME render offload).
# Extra environment still applies (NX_FULLSCREEN=1, NX_RESOLUTION=WxH, NX_MUTE=1, NX_MEM_LIMIT_GB, NX_LOG).
cd "$(dirname "$0")/.." || exit 1
MODE=${1:-quality}
BUILD=${2:-private/wonder/build}
case "$MODE" in
  quality) ;;
  balanced) export NX_HANDHELD=1 NX_FSR=1 ;;
  performance) export NX_HANDHELD=1 NX_FSR=1 NX_FSR_SHARP=${NX_FSR_SHARP:-1.0} ;;
  intel) export NX_GPU=${NX_GPU:-integrated} NX_HANDHELD=1 NX_PIPELINED=${NX_PIPELINED:-2} NX_POWER_PROFILE=${NX_POWER_PROFILE:-1} ;;
  lightest) export NX_GPU=${NX_GPU:-integrated} NX_HANDHELD=1 NX_PIPELINED=${NX_PIPELINED:-2} NX_POWER_PROFILE=${NX_POWER_PROFILE:-1} NX_FSR=1 NX_FSR_SHARP=${NX_FSR_SHARP:-1.0} ;;
  *) echo "unknown preset: $MODE"; exit 1 ;;
esac
export NX_GPU=${NX_GPU:-discrete}
export __NV_PRIME_RENDER_OFFLOAD=${__NV_PRIME_RENDER_OFFLOAD:-1}
export NX_LOG=${NX_LOG:-private/wonder/nx_play.log}
exec "$BUILD/wonder"
