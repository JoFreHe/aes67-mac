#!/bin/bash
set -euo pipefail

RESOURCE_DIR="$(cd "$(dirname "$0")/Resources" && pwd)"
SOURCE="$RESOURCE_DIR/AppIcon.png"
OUTPUT="${1:-$RESOURCE_DIR/AppIcon.icns}"
ICONSET_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/aes67-iconset.XXXXXX")"
ICONSET="$ICONSET_ROOT/AppIcon.iconset"
trap 'rm -rf "$ICONSET_ROOT"' EXIT

if [ ! -f "$SOURCE" ]; then
  echo "Missing icon source: $SOURCE" >&2
  exit 1
fi

mkdir -p "$ICONSET" "$(dirname "$OUTPUT")"

make_icon() {
  local name="$1"
  local size="$2"
  sips -z "$size" "$size" "$SOURCE" --out "$ICONSET/$name" >/dev/null
}

make_icon icon_16x16.png 16
make_icon icon_16x16@2x.png 32
make_icon icon_32x32.png 32
make_icon icon_32x32@2x.png 64
make_icon icon_128x128.png 128
make_icon icon_128x128@2x.png 256
make_icon icon_256x256.png 256
make_icon icon_256x256@2x.png 512
make_icon icon_512x512.png 512
make_icon icon_512x512@2x.png 1024

iconutil -c icns "$ICONSET" -o "$OUTPUT"
if [ ! -s "$OUTPUT" ] || [ "$(dd if="$OUTPUT" bs=1 count=4 2>/dev/null)" != "icns" ]; then
  echo "iconutil did not produce a valid ICNS file: $OUTPUT" >&2
  exit 1
fi
echo "Built $OUTPUT from $SOURCE"
