# Driver-Side Firmware Patching — 40 Column Unlock

## Strategy

Rather than modifying the NPU firmware file itself (which causes PSP validation
to fail with error 0x63), we patch the firmware **in-memory** in the kernel
driver — after PSP validates the original firmware but before PSP_START copies
it to NPU execution memory.

## How It Works

1. `aie2_psp_start()` sends `PSP_VALIDATE` with the **original** firmware
   → PSP confirms integrity of the stock firmware
2. `aie2_apply_fw_patches()` modifies the firmware in the DMA buffer at
   several offsets (see below)
3. `PSP_START` with `PSP_START_COPY_FW` re-reads from the DMA buffer
   (now patched) and copies it to NPU SRAM → the patched firmware runs

The PSP does **NOT** re-validate during PSP_START.

## Patches Applied

### 1. Multi-Application Gate Bypass (offsets 0x2DD8–0x2DF7)

Zeroes 4 function-pointer table entries in the VE2 IPU scheduler that
implement the "last scheduled application" serialization check.

After patching, the VE2 IPU scheduler always allows context creation
regardless of which application was previously scheduled. Without this,
only one process can use the NPU at a time.

### 2. Column Limit Unlock (offsets 0x17B04, 0x17B10)

Modifies the VE2 IPU firmware's `column_config` structure:

| Offset | Field | Before | After | Meaning |
|--------|-------|--------|-------|---------|
| 0x17B04 | `max_columns` | 0x00000008 (8) | 0x00000028 (40) | AIE columns exposed |
| 0x17B08 | reserved | 0xFFFFFFFF | 0xFFFFFFFF | Unchanged |
| 0x17B10 | `flags` | 0x08000010 | 0x28000010 | Encodes max_col=40 |

Changes `max_columns` from 8 to 40, unlocking all physically available
columns on Strix Halo (NPU5 / VE2). The TOPS scales linearly with column
count: ~4096 × total_col × clock_freq / 1,000,000.

### 3. Driver Column Override (`aie2_pci.c`)

Bypasses the firmware's `metadata.cols` clamp in `aie2_mgmt_fw_query()`:
```c
// Before: clamped to firmware metadata
ndev->total_col = min(aie2_max_col, ndev->metadata.cols);

// After: module parameter only (default XRS_MAX_COL=128)
ndev->total_col = aie2_max_col;
```

The `aie2_max_col` module parameter controls the column count:
```bash
sudo modprobe -r amdxdna
sudo modprobe amdxdna aie2_max_col=40
```

## Building & Testing

```bash
cd ~/amdxdna-dkns
make -C /lib/modules/$(uname -r)/build M=$(pwd)/src modules

# Unload old module
sudo rmmod amdxdna

# Load patched module with 40 columns
sudo insmod src/amdxdna/amdxdna.ko aie2_max_col=40

# Verify
dmesg | grep "total_col\|firmware patches\|OVERRIDE"
```

## Cold Boot Required

If the NPU PSP has been left in a bad state (e.g., from a previous failed
firmware load), a cold reboot (shutdown → 30s wait → power on) is required
before the patched module will work. Warm reboots may not clear the PSP
state.

## Firmware Versions

| Firmware | Size | Notes |
|----------|------|-------|
| `npu.sbin.1.0.0.166` | 376,768 B | Older, no column_config at 0x17B04 |
| `npu.sbin.1.1.2.65` (npu_7.sbin) | 429,680 B | Has column_config, patches apply |
