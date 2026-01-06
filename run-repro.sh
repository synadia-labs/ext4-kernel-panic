#!/bin/bash
#
# ext4 inline_data race condition reproducer
# Run as root on the same filesystem as your NATS JetStream data
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_TEST_DIR="/mnt/nats-data/test-inline"

die() { echo "ERROR: $*" >&2; exit 1; }

# Check root
[[ $EUID -eq 0 ]] || die "Must run as root"

# Parse arguments
TEST_DIR="${1:-$DEFAULT_TEST_DIR}"

echo "=== ext4 Inline Data Race Reproducer ==="
echo

# Check if burst-trigger exists or needs compilation
if [[ ! -x "$SCRIPT_DIR/burst-trigger" ]]; then
  echo "Compiling burst-trigger..."
  gcc -O2 -pthread -o "$SCRIPT_DIR/burst-trigger" "$SCRIPT_DIR/burst-trigger.c" \
    || die "Compilation failed. Is gcc installed?"
  echo "Compiled successfully."
  echo
fi

# Determine filesystem device
FS_DEVICE=$(df "$TEST_DIR" 2>/dev/null | tail -1 | awk '{print $1}' || true)
if [[ -z "$FS_DEVICE" ]]; then
  # Directory doesn't exist yet, check parent
  PARENT_DIR=$(dirname "$TEST_DIR")
  FS_DEVICE=$(df "$PARENT_DIR" 2>/dev/null | tail -1 | awk '{print $1}' || true)
fi

[[ -n "$FS_DEVICE" ]] || die "Cannot determine filesystem device for $TEST_DIR"

echo "Target directory: $TEST_DIR"
echo "Filesystem device: $FS_DEVICE"
echo

# Check inline_data feature
echo "Checking filesystem features..."
if command -v tune2fs &>/dev/null; then
  FEATURES=$(tune2fs -l "$FS_DEVICE" 2>/dev/null | grep -i features || true)
  if echo "$FEATURES" | grep -q inline_data; then
    echo "inline_data: ENABLED (vulnerable)"
  else
    echo "inline_data: DISABLED (mitigated)"
    echo
    echo "This filesystem does not have inline_data enabled."
    echo "The bug cannot be triggered on this filesystem."
    exit 0
  fi
else
  echo "tune2fs not found, cannot verify inline_data status"
  echo "Proceeding anyway..."
fi
echo

# Create test directory
mkdir -p "$TEST_DIR"

echo "WARNING: This tool WILL crash the machine when the bug triggers!"
echo "Press Ctrl+C within 5 seconds to abort..."
sleep 5

echo
echo "Starting reproducer..."
exec "$SCRIPT_DIR/burst-trigger" -d "$TEST_DIR" -a
