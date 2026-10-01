#!/bin/sh
# Renders every channel's preview.png and preview.gif into its own folder
# (firmware/channels/<id>/), through the real platform code. Needs ffmpeg for the GIF.
#
#   tools/hostrender/make-previews.sh            # every station on the board
#   tools/hostrender/make-previews.sh pong       # just one
#
# By default a channel is played through its own phone panel (START, slider) for 6 s at
# 12 fps. A channel can tell its own story with firmware/channels/<id>/preview.conf, a
# shell snippet that may set:
#   ARGS="..."        extra hostrender options (e.g. --tuner, --input, --fps, --frames)
#   GIF="fps colours size"   default "12 32 240"
set -eu
cd "$(dirname "$0")"
make -s
CH=../../firmware/channels
IDS=${*:-$(./hostrender --list)}
for id in $IDS; do
  [ -d "$CH/$id" ] || continue                       # host-only channels (inputprobe) have no folder
  ARGS="--fps 12 --frames 72"; GIF="12 32 240"
  [ -f "$CH/$id/preview.conf" ] && . "$CH/$id/preview.conf"
  set -- $GIF; FPS=$1; COLS=$2; SIZE=$3
  rm -rf "out/preview-$id"
  ./hostrender --quiet --channel "$id" --drive $ARGS --out "out/preview-$id"
  last=$(ls "out/preview-$id"/f*.png | tail -1)
  cp "$last" "$CH/$id/preview.png"
  # Snow is noise and GIF cannot compress noise: channels that show it keep colours/size low
  # in their preview.conf to stay under the repo's 1 MB file limit (tools/hooks).
  ffmpeg -loglevel error -y -framerate "$FPS" -i "out/preview-$id/f%03d.png" \
    -vf "scale=$SIZE:$SIZE:flags=area,split[a][b];[a]palettegen=max_colors=$COLS:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$CH/$id/preview.gif"
  echo "$id: preview.png, preview.gif $(wc -c < "$CH/$id/preview.gif") bytes"
done
