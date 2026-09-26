#!/bin/bash
# Stage the built firmware + Stripe key onto the Xteink SD card, then eject.
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
FW="${FW:-$REPO/.pio/build/default/firmware.bin}"
KEY="$REPO/fs_/.crosspoint/stripe.json"
VOL="${1:-/Volumes/NO NAME}"

[[ -f "$FW" ]] || { echo "No firmware at $FW — build first (pio run -e default)"; exit 1; }
[[ -d "$VOL" ]] || { echo "SD card not mounted at $VOL"; exit 1; }
[[ "$(xxd -p -l1 "$FW")" == "e9" ]] || { echo "firmware.bin is not an ESP32 app image"; exit 1; }

cp "$FW" "$VOL/update.bin"
mkdir -p "$VOL/.crosspoint"
[[ -f "$KEY" ]] && cp "$KEY" "$VOL/.crosspoint/stripe.json"
# Extra SD content (e.g. sleep/ wallpapers) mirrored onto the card root.
[[ -d "$REPO/sd-extra" ]] && rsync -r "$REPO/sd-extra/" "$VOL/"
# The card's wallpapers mirror sd-extra/sleep exactly, so removed ones disappear too.
[[ -d "$REPO/sd-extra/sleep" ]] && rsync -r --delete "$REPO/sd-extra/sleep/" "$VOL/sleep/"
sync

[[ "$(shasum "$FW" | cut -d' ' -f1)" == "$(shasum "$VOL/update.bin" | cut -d' ' -f1)" ]] \
  || { echo "Checksum mismatch on update.bin"; exit 1; }

dot_clean -m "$VOL" || true
find "$VOL" -name '._*' -delete 2>/dev/null || true

DISK="$(diskutil info "$VOL" | awk -F': *' '/Part of Whole/{print $2}')"
echo "Staged $(du -h "$VOL/update.bin" | cut -f1) update.bin on $VOL (disk: $DISK)"
diskutil eject "$DISK"
