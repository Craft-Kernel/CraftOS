#!/bin/bash
# builds solitaire's card sheets from the design's pngs in ../../../Craft Assets/solitaire:
#
#   assets/sol-cards.cimg   the 52 faces, 13 across (A to K) by 4 down
#                           (clubs, diamonds, hearts, spades), 71 x 99 each
#   assets/sol-extras.cimg  8 across: the green and blue backs, an empty
#                           slot, the empty stock, and the four foundation
#                           slots (clubs, diamonds, hearts, spades)
#
# and the app icon at the four sizes the desktop uses. needs imagemagick.
cd "$(dirname "$0")/.."
SRC="../../../Craft Assets/solitaire"
T=$(mktemp -d)
W=71
H=99

i=0
for suit in clubs diamonds hearts spades; do
  for rank in A 2 3 4 5 6 7 8 9 10 J Q K; do
    convert "$SRC/cards/$suit-$rank.png" -filter Lanczos -resize ${W}x${H}! $T/c$(printf %02d $i).png
    i=$((i+1))
  done
done
montage $T/c*.png -tile 13x4 -geometry ${W}x${H}+0+0 -background none $T/cards.png

j=0
for f in backs/back-green backs/back-blue slots/empty slots/stock-empty slots/foundation-clubs \
         slots/foundation-diamonds slots/foundation-hearts slots/foundation-spades; do
  convert "$SRC/$f.png" -filter Lanczos -resize ${W}x${H}! $T/e$j.png
  j=$((j+1))
done
montage $T/e0.png $T/e1.png $T/e2.png $T/e3.png $T/e4.png $T/e5.png $T/e6.png $T/e7.png \
  -tile 8x1 -geometry ${W}x${H}+0+0 -background none $T/extras.png

python3 tools/img2cimg.py $T/cards.png assets/sol-cards.cimg --size $((W * 13))x$((H * 4))
python3 tools/img2cimg.py $T/extras.png assets/sol-extras.cimg --size $((W * 8))x$H

for s in 16 32 48; do
  python3 tools/img2cimg.py "$SRC/icon/solitaire-$s.png" assets/app-solitaire-$s.cimg --size ${s}x$s
done
convert "$SRC/icon/solitaire-256.png" -filter Lanczos -resize 28x28 $T/icon28.png
python3 tools/img2cimg.py $T/icon28.png assets/app-solitaire-28.cimg --size 28x28
rm -rf $T
ls -la assets/sol-*.cimg assets/app-solitaire-*.cimg
