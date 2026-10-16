# SPDX-License-Identifier: GPL-2.0-only

# Prepend our include paths to ensure bundled headers override system ones
# This is critical because 6.14 kernels may ship with older amdxdna_accel.h
LINUXINCLUDE := -I$(src)/include/uapi -I$(src) $(LINUXINCLUDE)

amdxdna-y := \
	aie2_ctx.o \
	aie2_error.o \
	aie2_message.o \
	aie2_pci.o \
	aie2_pm.o \
	aie2_psp.o \
	aie2_smu.o \
	aie2_solver.o \
	amdxdna_ctx.o \
	amdxdna_gem.o \
	amdxdna_mailbox.o \
	amdxdna_mailbox_helper.o \
	amdxdna_pci_drv.o \
	amdxdna_pm.o \
	amdxdna_sysfs.o \
	amdxdna_ubuf.o \
	npu1_regs.o \
	npu4_regs.o \
	npu5_regs.o \
	npu6_regs.o

obj-m := amdxdna.o
