/*
 * virtio-mmio to virtio-msg proxy.
 */
#ifndef _VMP_H_
#define _VMP_H_

#include <xen/mm.h>
#include <xen/spsc_queue.h>

#define VMP_STATS 0
#define VIRTIO_MSG_MAX_QUEUES 32

enum vmp_state {
        VMP_STATE_IDLE = 0,
        VMP_STATE_GET_DEVICE_STATUS,
        VMP_STATE_DEVICE_INFO,
        VMP_STATE_GET_FEATURES,
        VMP_STATE_SET_FEATURES,
        VMP_STATE_GET_CONFIG,
        VMP_STATE_SET_CONFIG,
        VMP_STATE_GET_CONFIG_GEN,
        VMP_STATE_GET_VQUEUE,
        VMP_STATE_MAX,
};

struct vmp {
    struct domain *domain;

    /*
     * VMP runs into two contexts.
     *
     * 1. MMIO traps into the virtio-mmio space.
     * 2. Event-channel notifications from the domain with the virtio-dev.
     *
     * These contexts may run on different guest vcpus, so we need locking.
     * We don't use interrupts so we don't need to disable irq's when
     * taking our spinlock.
     */
    spinlock_t lock;

    enum vmp_state state;

    struct {
        uint32_t access;
        uint32_t access_data;
        uint32_t device_features_sel;
        uint32_t driver_features_sel;
        uint32_t driver_features[2];
        uint32_t queue_sel;
        uint32_t queue_max_size;
        uint32_t interrupt_status;
        uint32_t status;
    } regs;

    /* FIXME: Do we need dynamic allocation of these?  */
    struct {
        uint32_t size;
        uint64_t descriptor_addr;
        uint64_t driver_addr;
        uint64_t device_addr;
        bool enabled;
    } vq[VIRTIO_MSG_MAX_QUEUES];

#if VMP_STATS
    struct {
        uint16_t state[VMP_STATE_MAX];
        uint16_t regs[0x100 / 4];
        uint16_t ind_regs[0x100 / 4];
    } stats;
#endif

    paddr_t     base_addr;
    unsigned int virq;

    evtchn_port_t evtchn;

    struct {
        spsc_queue *driver;
        spsc_queue *device;
        bool error;
    } queues;

    struct {
        struct page_info *page;
        void *ptr;
    } shm;
};

int domain_vmp_init(struct domain *d);
void domain_vmp_deinit(struct domain *d);
#endif  /* _VMP_H_ */
