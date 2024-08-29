/*
 * virtio-mmio indirect access extension.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __VIRTIO_MMIO_IND_EXT_H__
#define __VIRTIO_MMIO_IND_EXT_H__

/*
 * Writing to the VIRTIO_MMIO_ACCESS register will trigger an indirect MMIO
 * access to the virtio-mmio registers.
 *
 * Data layout:
 * [31]    - rw: READ=0, WRITE=1.
 * [29:28] - size:   8BIT=0, 16BIT=1, 32BIT=2
 * [16:0]  - offset: Virtio-mmio offset.
 *
 * When reading from this register, you can poll for status. No new transaction
 * will be triggered.
 *
 * Data layout:
 * [31]    - rw:     READ=0, WRITE=1.
 * [30]    - status: IDLE=0, BUSY=1
 * [29:28] - size:   8BIT=0, 16BIT=1, 32BIT=2
 * [27]    - error:  RETRY=1
 *                   If the device signals error retry, it means some of it's
 *                   internal resources to handle the are termporarily fully
 *                   used and it needs to push back (backpressure). The driver
 *                   should retry the access.
 *
 * [16:0]  - offset: Virtio-mmio offset.
  */
#define VIRTIO_MMIO_ACCESS 0x18
#define VIRTIO_MMIO_ACCESS_WRITE       (1UL << 31)
#define VIRTIO_MMIO_ACCESS_BUSY        (1UL << 30)
#define VIRTIO_MMIO_ACCESS_SIZE_SHIFT  28
#define VIRTIO_MMIO_ACCESS_ERROR_RETRY (1UL << 27)

/*
 * Transaction data.
 *
 * For reads, only valid after ACCESS.BUSY = 0.
 * For writes, must be written to before writing to ACCESS to initiate
 * the write.
 */
#define VIRTIO_MMIO_ACCESS_DATA 0x1c
#endif
