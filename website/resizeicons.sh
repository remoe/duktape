#!/bin/sh
#
#  Resize touch icon starting from touch_icon_1024x1024.png.
#

for sz in 16 32 48 57 60 64 72 114 120 128 144 152 256; do
	convert touch_icon_1024x1024.png -resize ${sz}x${sz} touch_icon_${sz}x${sz}.png
	file touch_icon_${sz}x${sz}.png
done

