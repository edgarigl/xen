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

/* PCI Root Bridges attached to PVH guests */
#define PCIE_GSI_BASE               (16U)

/* Resources allocated to the vpci root bridge */
#define PCIE_VPCI_SEG               (0U)
#define PCIE_VPCI_NR_BUS            (256U)
#define PCIE_VPCI_ECAM_BASE         xen_mk_ullong(0xe000000000)
#define PCIE_VPCI_ECAM_SIZE         (PCIE_VPCI_NR_BUS * 0x100000UL)
#define PCIE_VPCI_MMIO_BASE         xen_mk_ulong(0xf0000000)
#define PCIE_VPCI_MMIO_SIZE         xen_mk_ulong(0x02000000)
#define PCIE_VPCI_64BIT_MMIO_BASE   xen_mk_ullong(0xc000000000)
#define PCIE_VPCI_64BIT_MMIO_SIZE   xen_mk_ullong(0x1000000000)
#define PCIE_VPCI_INTX_BASE         PCIE_GSI_BASE

/* Resources allocated to the virtio pci root bridge */
#define PCIE_VIRTIO_SEG             (1U)
#define PCIE_VIRTIO_NR_BUS          (256U)
#define PCIE_VIRTIO_ECAM_BASE       xen_mk_ullong(0xe010000000)
#define PCIE_VIRTIO_ECAM_SIZE       (PCIE_VIRTIO_NR_BUS * 0x100000UL)
#define PCIE_VIRTIO_MMIO_BASE       xen_mk_ulong(0xf2000000)
#define PCIE_VIRTIO_MMIO_SIZE       xen_mk_ulong(0x02000000)
#define PCIE_VIRTIO_64BIT_MMIO_BASE xen_mk_ullong(0xd000000000)
#define PCIE_VIRTIO_64BIT_MMIO_SIZE xen_mk_ullong(0x1000000000)
#define PCIE_VIRTIO_INTX_BASE       PCIE_GSI_BASE

#endif /* __XEN_PUBLIC_HVM_E820_H__ */
