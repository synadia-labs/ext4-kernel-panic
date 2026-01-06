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
sudo ./burst-trigger -d /mnt/nats-data/test-inline -a
```

**WARNING**: When successful, this WILL crash the machine. Run on a non-production
node or be prepared for an immediate reboot.

## Command Options

```
Usage: ./burst-trigger [-d dir] [-f files] [-c converters] [-a]

Options:
  -d dir         Test directory (default: /mnt/ext4-test/burst)
  -f files       Files per burst (default: 1000)
  -c converters  Converter threads (default: 16)
  -a             Apply aggressive writeback sysctl settings (recommended)
```

## Expected Behavior

When running, the tool outputs:

```
[10s] Bursts: 50, Conversions: 50000, Rate: 5000/s
[20s] Bursts: 100, Conversions: 100000, Rate: 5000/s
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
sudo ./burst-trigger -d /mnt/nats-data/test-inline -a
```

With `inline_data` disabled, the tool will run indefinitely without crashing.

## How It Works

The tool uses a coordinated 3-phase approach to maximize race probability:

1. **ACCUMULATE**: Create 1000 files at 140 bytes (90% of inline threshold)
2. **TRIGGER**: Call `sync_file_range()` to start non-blocking writeback
3. **CONVERT**: Immediately expand files to 200 bytes (forces inline-to-extent)

The race occurs when writeback (checking inline status) happens simultaneously
with the conversion (clearing the inline flag).

## Files

- `burst-trigger.c` - The reproduction tool
- `README.md` - This file

## Notes

- The tool does not require NATS to be running
- It must run on the same filesystem where NATS stores JetStream data
- The bug is timing-dependent; multiple runs may be needed
- Crash detection: the tool saves state to `/var/tmp/ext4-burst-state`

## Support

This reproduction package was prepared as part of the investigation into
kernel panics affecting your NATS cluster. The recommended mitigation is
to reformat the JetStream storage volume with `mkfs.ext4 -O ^inline_data`.
