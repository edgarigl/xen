/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2006, Keir Fraser
 */

#ifndef __XEN_PUBLIC_HVM_E820_H__
#define __XEN_PUBLIC_HVM_E820_H__

#include "../xen.h"

/* E820 location in HVM virtual address space. */
#define HVM_E820_PAGE        0x00090000
#define HVM_E820_NR_OFFSET   0x000001E8
#define HVM_E820_OFFSET      0x000002D0

#define HVM_BELOW_4G_RAM_END        0xF0000000
#define HVM_BELOW_4G_MMIO_START     HVM_BELOW_4G_RAM_END
#define HVM_BELOW_4G_MMIO_LENGTH    ((xen_mk_ullong(1) << 32) - \
                                     HVM_BELOW_4G_MMIO_START)

#define PCIE_VIRTIO_SEG       (0U)
#define PCIE_VIRTIO_NR_BUS    (256U)
#define PCIE_VIRTIO_ECAM_BASE xen_mk_ullong(0xc000000000)
#define PCIE_VIRTIO_ECAM_SIZE (PCIE_VIRTIO_NR_BUS * 0x100000ULL)
#define PCIE_VIRTIO_64BIT_MMIO_BASE \
    (PCIE_VIRTIO_ECAM_BASE +  PCIE_VIRTIO_ECAM_SIZE)
#define PCIE_VIRTIO_64BIT_MMIO_SIZE xen_mk_ullong(0x1000000000)
#define PCIE_VIRTIO_MMIO_BASE xen_mk_ulong(HVM_BELOW_4G_MMIO_START)
#define PCIE_VIRTIO_MMIO_SIZE (0x2000000UL)
#define PCIE_VIRTIO_GSI_START (16U)

#endif /* __XEN_PUBLIC_HVM_E820_H__ */
