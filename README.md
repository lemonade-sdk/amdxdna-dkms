# amdxdna-dkms

DKMS package for AMD XDNA NPU driver and firmware.

## Overview

This package provides the AMD XDNA NPU (Neural Processing Unit) driver as a DKMS module, along with the required firmware files. It enables support for AMD NPU devices on Ubuntu 24.04 and compatible distributions.

## Supported Kernels

- Linux 6.17 (Ubuntu OEM and HWE kernels)
- Linux 6.18, 6.19
- Linux 7.0 – 7.2

The package is designed to work with Ubuntu 24.04 LTS kernel variants:
- linux-oem-24.04 (6.17)
- linux-generic-hwe-24.04 (6.17)

## Installation

### From PPA (Recommended)

```bash
sudo add-apt-repository ppa:lemonade-team/stable
sudo apt update
sudo apt install amdxdna-dkms
```

### From .deb Package

```bash
sudo apt install ./amdxdna-dkms_*.deb
```

The package will automatically:
1. Install driver sources to `/usr/src/amdxdna-VERSION/`
2. Install firmware to `/lib/firmware/updates/amdnpu/`
3. Build the kernel module for your current kernel via DKMS
4. Update initramfs to include the new firmware

## Building from Source

### Prerequisites

```bash
sudo apt install debhelper dkms devscripts
```

### Build Steps

```bash
# Clone the repository
git clone https://github.com/your-username/amdxdna-dkms.git
cd amdxdna-dkms

# Build the package
dpkg-buildpackage -us -uc -b

# Install the package
sudo dpkg -i ../amdxdna-dkms_*.deb
```

## Updating from Upstream

To sync with the latest upstream kernel and firmware:

```bash
./scripts/update-from-upstream.sh
```

This will:
- Fetch the latest driver sources from the kernel tree
- Fetch the latest firmware from linux-firmware
- Generate appropriate DKMS Makefile
- Update version in debian/changelog

## Verification

After installation, verify the module is built:

```bash
dkms status amdxdna
```

Check firmware installation:

```bash
ls -lR /lib/firmware/updates/amdnpu/
```

Load the module (requires compatible hardware):

```bash
sudo modprobe amdxdna
lsmod | grep amdxdna
```

Check for NPU devices:

```bash
ls -l /dev/accel/accel*
```

## Supported Devices

- AMD Ryzen AI (Phoenix, Hawk Point): PCI ID 1502:00
- AMD Ryzen AI (Strix Point): PCI ID 17f0:10
- AMD Ryzen AI (Strix Halo): PCI ID 17f0:11

## License

The driver source code is licensed under GPL-2.0.
The firmware binaries are proprietary and licensed by AMD.

## Contributing

This is a packaging repository. For driver or firmware issues, please report to:
- Driver: https://gitlab.freedesktop.org/drm/misc/kernel
- Firmware: https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git

For packaging issues, please open an issue in this repository.
