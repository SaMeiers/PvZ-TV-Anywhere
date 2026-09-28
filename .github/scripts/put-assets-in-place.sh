#!/usr/bin/env bash
#
# Lays the game data out for one language build.
#
#   put-assets-in-place.sh <assets-checkout> <destination> <zh|en>
#
# The assets repository stores the game once, as it ships -- in Chinese -- plus
# a small English override: the two string files and the bitmap fonts.
#
# Copying the override over the base is almost enough, but not quite, because a
# replaced font does not always keep the file name it had. The English atlases
# came out of another build as `BrianneTod12_.png` where the Chinese one is
# `BrianneTod12.png`, so a plain copy leaves both in place -- and the engine,
# asked for `BrianneTod12` with no extension, resolves the Chinese file and
# draws our English text through its glyph rectangles. That is a menu of boxes
# and cut-up characters, and it is why the first English build was wrong.
#
# So before the override goes in, every image it brings takes out the base
# images of the same font, whatever those are named. Descriptors are left
# alone, and a font the override does not touch keeps the image it always had:
# three of them are byte-identical to the base and have no English atlas of
# their own.

set -euo pipefail

src=${1:?assets checkout}
dest=${2:?destination}
language=${3:?zh or en}

base=$src/assets
override=$src/overrides/english

# A fork has no access to the assets repository, so nothing was checked out
# and the build simply comes out with no game data.
if [ ! -d "$base" ]; then
  echo "No assets available; building without game data."
  exit 0
fi

mkdir -p "$dest"
cp -r "$base/." "$dest/"

if [ "$language" = zh ]; then
  echo "$(find "$dest" -type f | wc -l) asset files in place (zh)"
  exit 0
fi

# The font an image belongs to: drop the extension, then the trailing
# underscore that marks an atlas exported on its own.
font_of() {
  local stem=${1%.*}
  printf '%s' "${stem%_}"
}

while IFS= read -r -d '' image; do
  rel=${image#"$override"/}
  dir=$(dirname "$rel")
  font=$(font_of "$(basename "$rel")")

  for ext in png gif jpg; do
    for stale in "$dest/$dir/$font.$ext" "$dest/$dir/${font}_.$ext"; do
      if [ -f "$stale" ]; then
        rm -f "$stale"
        echo "dropped $dir/$(basename "$stale"), replaced by $(basename "$rel")"
      fi
    done
  done
done < <(find "$override" -type f \( -name '*.png' -o -name '*.gif' -o -name '*.jpg' \) -print0)

cp -r "$override/." "$dest/"
echo "$(find "$dest" -type f | wc -l) asset files in place (en)"
