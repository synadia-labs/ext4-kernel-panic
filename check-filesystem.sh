#!/bin/bash
#
# Check if an ext4 filesystem has inline_data enabled
#

set -e

die() { echo "ERROR: $*" >&2; exit 1; }

# Check root for tune2fs
[[ $EUID -eq 0 ]] || die "Must run as root"

TARGET="${1:-/mnt/nats-data}"

echo "=== ext4 inline_data Check ==="
echo

# Find the device
if [[ -b "$TARGET" ]]; then
  DEVICE="$TARGET"
else
  DEVICE=$(df "$TARGET" 2>/dev/null | tail -1 | awk '{print $1}')
fi

[[ -n "$DEVICE" ]] || die "Cannot determine device for: $TARGET"

echo "Target: $TARGET"
echo "Device: $DEVICE"
echo

# Check filesystem type
FSTYPE=$(lsblk -no FSTYPE "$DEVICE" 2>/dev/null || true)
if [[ "$FSTYPE" != "ext4" ]]; then
  echo "Filesystem type: $FSTYPE"
  echo "This check only applies to ext4 filesystems."
  exit 0
fi
echo "Filesystem type: ext4"
echo

# Check features
if ! command -v tune2fs &>/dev/null; then
  die "tune2fs not found"
fi

echo "Filesystem features:"
tune2fs -l "$DEVICE" 2>/dev/null | grep -i "features" || true
echo

# Check inline_data specifically
FEATURES=$(tune2fs -l "$DEVICE" 2>/dev/null | grep -i "features" || true)
if echo "$FEATURES" | grep -q inline_data; then
  echo "STATUS: VULNERABLE"
  echo
  echo "This filesystem has inline_data enabled and is susceptible to the"
  echo "ext4 race condition that causes kernel panics."
  echo
  echo "Mitigation: Reformat with 'mkfs.ext4 -O ^inline_data'"
  exit 1
else
  echo "STATUS: MITIGATED"
  echo
  echo "This filesystem does not have inline_data enabled."
  echo "The race condition cannot be triggered."
  exit 0
fi
