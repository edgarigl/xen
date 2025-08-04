/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef XEN_FDT_VIRTIO_H
#define XEN_FDT_VIRTIO_H

#include <asm/kernel.h>
#include <xen/types.h>

int dom_construct_virtio(struct kernel_info *ki);
int parse_virtio(const struct dt_device_node *node, struct kernel_info *ki);

#endif /* XEN_FDT_VIRTIO_H */
