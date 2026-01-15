# ext4 Inline Data Race Reproduction

This tool reproduces a Linux kernel bug in the ext4 filesystem that causes kernel
panics. The bug is a race condition in the `inline_data` feature that has been
present since kernel 3.8 (2013) and affects all current kernels including 6.8.x.

**Syzkaller Bug Report**: https://syzkaller.appspot.com/bug?extid=d1da16f03614058fdc48

## Overview

The bug occurs when:
1. A small file (<156 bytes) is stored inline in inode metadata
2. The file grows past the threshold, triggering conversion to block storage
3. Kernel writeback occurs during the conversion
4. A race between these operations triggers a BUG_ON assertion

## Prerequisites

- Linux kernel 5.4+ with ext4 filesystem
- ext4 volume with `inline_data` feature enabled (default on most systems)
- Root access to run the reproduction tool
- GCC to compile the tool

## Check Your Filesystem

Before running, verify your NATS data volume has `inline_data` enabled:

```bash
# Check mounted filesystem options
tune2fs -l /dev/<your-device> | grep inline
```

If output includes `inline_data`, the filesystem is vulnerable.

## Quick Start

```bash
# 1. Copy files to one of your NATS nodes
scp burst-trigger.c user@nats-node:/tmp/

# 2. SSH to the node
ssh user@nats-node

# 3. Compile
gcc -O2 -pthread -o burst-trigger burst-trigger.c

# 4. Create test directory on same filesystem as NATS data
# (Assumes NATS data is on /mnt/nats-data)
sudo mkdir -p /mnt/nats-data/test-inline

# 5. Run the reproducer
sudo ./burst-trigger -d /mnt/nats-data/test-inline
```

**WARNING**: When successful, this WILL crash the machine. Run on a non-production
node or be prepared for an immediate reboot.

## Command Options

```
Usage: ./burst-trigger [-d dir] [-w writers] [-s syncers] [-f files]

Options:
  -d dir      Test directory (default: /mnt/ext4-test/trigger)
  -w writers  Number of writer threads (default: 16)
  -s syncers  Number of syncer threads (default: 4)
  -f files    Files per writer (default: 50)
```

## Expected Behavior

When running, the tool outputs progress every 5 seconds:

```
[5s] ops=2500 syncs=450 rate=500/s dirty=128KB wb=64KB
[10s] ops=5200 syncs=920 rate=520/s dirty=256KB wb=128KB
```

If the bug triggers, the machine will crash with a kernel panic similar to:

```
kernel BUG at fs/ext4/inode.c:2721!
```

Typical time to crash: 30 seconds to 5 minutes, depending on system load.

## Mitigation Verification

To verify the mitigation works, reformat the test filesystem without `inline_data`:

```bash
# Unmount the volume
sudo umount /mnt/nats-data

# Reformat WITHOUT inline_data
sudo mkfs.ext4 -O ^inline_data /dev/<your-device>

# Remount
sudo mount /dev/<your-device> /mnt/nats-data

# Verify inline_data is disabled
tune2fs -l /dev/<your-device> | grep inline
# (should show nothing or "inline_data" absent from feature list)

# Run reproducer again - it should NOT crash
sudo mkdir -p /mnt/nats-data/test-inline
sudo ./burst-trigger -d /mnt/nats-data/test-inline
```

With `inline_data` disabled, the tool will run indefinitely without crashing.

## How It Works

The tool uses concurrent writer and syncer threads to maximize race probability:

1. **Writer threads** create small files (100 bytes, stored inline in inode)
2. **Writers** immediately expand files to 200 bytes (forces inline-to-extent conversion)
3. **Syncer threads** continuously call `sync()` to trigger writeback
4. Half the writers use an aggressive pattern (batch create, sync, expand)

The race occurs when writeback (checking inline status) happens simultaneously
with the conversion (clearing the inline flag).

## Files

- `burst-trigger.c` - The reproduction tool
- `run-repro.sh` - Helper script that compiles and runs the tool
- `check-filesystem.sh` - Check if a filesystem has inline_data enabled
- `README.md` - This file

## Notes

- The tool does not require NATS to be running
- It must run on the same filesystem where NATS stores JetStream data
- The bug is timing-dependent; multiple runs may be needed
- Crash detection: the tool saves state to `/var/tmp/ext4-repro-state`

## Support

This reproduction package was prepared as part of the investigation into
kernel panics affecting your NATS cluster. The recommended mitigation is
to reformat the JetStream storage volume with `mkfs.ext4 -O ^inline_data`.
