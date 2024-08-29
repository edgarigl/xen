/*
 * virtio-mmio to virtio-msg proxy.
 *
 * Implements an extension to virtio-mmio allowing for indirect and
 * non-blocking accesses.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * TODO:
 * Use xen/perfc for stats.
 */
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/ctype.h>
#include <xen/string.h>
#include <xen/event.h>
#include <asm/mmio.h>

#include <xen/virtio_mmio.h>
/* Indirect access extension */
#include <xen/virtio_mmio_ind_ext.h>
#include <xen/virtio-msg-prot.h>
#include <asm/vmp.h>

/* TODO: Make these configurable.  */
#define VMP_BASE 0x2000000
#define VMP_SIZE 0x1000
#define VMP_EMULATOR_DOMID 0
/* Size of each side of the queue.  */
#define VMP_QUEUE_SIZE 1024

#define VMP_DEBUG 0

#define VIRT_MAGIC 0x74726976 /* 'virt' */
#define VIRT_VERSION 3
#define VIRT_VENDOR 0x58656e20 /* 'Xen ' */

static void vmp_set_status(struct vmp *s);

static inline void vmp_update_interrupts(struct vmp *s)
{
    vgic_inject_irq(s->domain, NULL, s->virq, s->regs.interrupt_status);
}

/* Set or clear the busy bit. Must be called with s->lock held.  */
static inline void vmp_indirect_set_busy(struct vmp *s, bool busy, bool err)
{
    s->regs.access |= err ? VIRTIO_MMIO_ACCESS_ERROR_RETRY : 0;
    s->regs.access &= ~VIRTIO_MMIO_ACCESS_BUSY;
    if (busy) {
        s->regs.access |= VIRTIO_MMIO_ACCESS_BUSY;
        /*
         * Make sure stores to access_data are observed
         * before we clear the busy flag.
         */
        smp_wmb();
    }
}

static bool virtio_msg_bus_send(struct vmp *s, VirtIOMSG *msg_req)
{
    bool sent;

    sent = spsc_send(s->queues.driver, msg_req, sizeof *msg_req);

    s->queues.error = !sent;
    if ( sent )
        notify_via_xen_event_channel(s->domain, s->evtchn);
    return sent;
}

void vmp_print_stats(struct vmp *s)
{
#if VMP_STATS
    int i;

    printk("%s:\n", __func__);
    for (i = 0; i < ARRAY_SIZE(s->stats.state); i++) {
        printk("state[%d]=%d\n", i, s->stats.state[i]);
    }
    for (i = 0; i < ARRAY_SIZE(s->stats.regs); i++) {
        if (s->stats.regs[i])
            printk("dir_reg[0x%x]=%d\n", i * 4, s->stats.regs[i]);
    }
    for (i = 0; i < ARRAY_SIZE(s->stats.ind_regs); i++) {
        if (s->stats.ind_regs[i])
            printk("ind_reg[0x%x]=%d\n", i * 4, s->stats.ind_regs[i]);
    }
    for (i = 0; i < ARRAY_SIZE(s->stats.ind_regs); i++) {
        if (s->stats.ind_regs[i] || s->stats.regs[i])
            printk("reg[0x%x]=%d\n", i * 4,
                   s->stats.regs[i] + s->stats.ind_regs[i]);
    }
#endif
}

/*
 * Validates and moves the state-machine into a new state.
 */
static void vmp_set_state(struct vmp *s, int new_state)
{
    bool bad_state = false;

    /*
     * Changing to the same state is not an error but it's an indication
     * that the code may not expect what just happened. So we print a warning
     * and return OK
     */
    if (s->state == new_state) {
        printk("State change to same state %d -> %d\n", s->state, new_state);
    }

    /* Out of bounds?  */
    if (new_state < VMP_STATE_IDLE || new_state >= VMP_STATE_MAX)
        bad_state = true;
    /* We can only go from STATE_IDLE to something and back.  */
    if (s->state != VMP_STATE_IDLE && new_state != VMP_STATE_IDLE)
        bad_state = true;
    if (bad_state) {
        printk("Bad state change from %d -> %d\n", s->state, new_state);
        ASSERT(0);
    }

    /* OK.  */
    s->state = new_state;
#if VMP_STATS
    s->stats.state[new_state]++;
#endif
}

static void vmp_receive_req(struct vmp *s, VirtIOMSG *msg)
{
    switch (msg->id) {
    case VIRTIO_MSG_EVENT_USED:
        s->regs.interrupt_status |= VIRTIO_MMIO_INT_VRING;
        vmp_update_interrupts(s);
        break;
    case VIRTIO_MSG_EVENT_CONFIG:
        s->regs.interrupt_status |= VIRTIO_MMIO_INT_CONFIG;
        vmp_update_interrupts(s);
        break;
    default:
        printk("Dropped msg. Request %x\n", msg->id);
        /* Dropped.  */
        ASSERT(0);
        break;
    }
}

/*
 * Validate that this message is one we're expecting.
 */
static bool vmp_expected_msg_resp(struct vmp *s, VirtIOMSG *msg)
{
#define VMP_STATE_CHECK_MSGID(X) [VMP_STATE_ ## X] = VIRTIO_MSG_ ## X
    static const int valid[] = {
        VMP_STATE_CHECK_MSGID(GET_DEVICE_STATUS),
        VMP_STATE_CHECK_MSGID(DEVICE_INFO),
        VMP_STATE_CHECK_MSGID(GET_FEATURES),
        VMP_STATE_CHECK_MSGID(SET_FEATURES),
        VMP_STATE_CHECK_MSGID(GET_CONFIG),
        VMP_STATE_CHECK_MSGID(SET_CONFIG),
        VMP_STATE_CHECK_MSGID(GET_CONFIG_GEN),
        VMP_STATE_CHECK_MSGID(GET_VQUEUE),
    };
    uint8_t id = msg->id;
    int state = s->state;

    ASSERT(state < ARRAY_SIZE(valid));
    return id == valid[state];
}

static void vmp_receive(struct vmp *s, VirtIOMSG *msg)
{
    uint32_t features_shift;

    if (VMP_DEBUG)
        virtio_msg_print(msg);

    if (!(msg->type & VIRTIO_MSG_TYPE_RESPONSE)) {
        vmp_receive_req(s, msg);
        return;
    }

    /* Responses.  */
    if (!vmp_expected_msg_resp(s, msg)) {
        printk("%s: Dropping unexpected response! state %d\n",
               __func__, s->state);
        virtio_msg_print(msg);
        return;
    }

    switch (msg->id) {
    case VIRTIO_MSG_GET_DEVICE_STATUS:
        s->regs.access_data = msg->get_device_status_resp.status;
        break;
    case VIRTIO_MSG_DEVICE_INFO:
        s->regs.access_data = msg->get_device_info_resp.device_id;
        break;
    case VIRTIO_MSG_GET_FEATURES:
        /* FIXME: Handle features beyond 64bits. */
        features_shift = s->regs.device_features_sel & 1 ? 32 : 0;
        s->regs.access_data = msg->get_features_resp.features >> features_shift;
        break;
    case VIRTIO_MSG_SET_FEATURES:
        /* NOP.  */
        break;
    case VIRTIO_MSG_GET_CONFIG:
        s->regs.access_data = msg->get_config_resp.data;
        break;
    case VIRTIO_MSG_SET_CONFIG:
        break;
    case VIRTIO_MSG_GET_CONFIG_GEN:
        s->regs.access_data = msg->get_config_gen_resp.generation;
        break;
    case VIRTIO_MSG_GET_VQUEUE:
        s->regs.queue_max_size = msg->get_vqueue_resp.max_size;
        break;
    default:
        /*  Already dropped unexpected resonses, this should never happen. */
        virtio_msg_print(msg);
        ASSERT_UNREACHABLE();
        return;
    }

    vmp_indirect_set_busy(s, false, false);
    vmp_set_state(s, VMP_STATE_IDLE);
}

/*
 * Rx budget of messages to process back-to-back before returning.
 */
#define VMP_RX_BUDGET 2
static void vmp_rx_process(struct vmp *s)
{
    spsc_queue *q;
    VirtIOMSG msg;
    int i = 0;
    bool r;

    /*
     * We process the opposite queue, i.e, a driver will want to receive
     * messages on the backend queue (and send messages on the driver queue).
     */
    q = s->queues.device;
    do {
        r = spsc_recv(q, &msg, sizeof msg);
        if (r) {
            virtio_msg_unpack(&msg);
            vmp_receive(s, &msg);
        }
    } while (r && i++ < VMP_RX_BUDGET);
}

static void vmp_get_status(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_device_status(&msg);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_set_status(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_set_device_status(&msg, s->regs.status);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_get_device_info(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_device_info(&msg);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_get_features(struct vmp *s)
{
    VirtIOMSG msg;
    uint32_t features_sel = s->regs.device_features_sel;

    /*
     * Virtio-mmio uses a 32-bit feature word array.
     * Virtio-msg has a 256-bit feature word array.
     */
    features_sel /= 8;

    virtio_msg_pack_get_features(&msg, features_sel);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_get_config(struct vmp *s, uint8_t size, uint32_t offset)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_config(&msg, size, offset);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_set_config(struct vmp *s, uint8_t size,
                           uint32_t offset, uint32_t data)
{
    VirtIOMSG msg;

    virtio_msg_pack_set_config(&msg, size, offset, data);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_get_config_gen(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_config_gen(&msg);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_get_vqueue(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_vqueue(&msg, s->regs.queue_sel);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_set_vqueue(struct vmp *s)
{
    VirtIOMSG msg;
    int i = s->regs.queue_sel;

    virtio_msg_pack_set_vqueue(&msg, s->regs.queue_sel,
                               s->vq[i].size,
                               s->vq[i].descriptor_addr,
                               s->vq[i].driver_addr,
                               s->vq[i].device_addr);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_set_features(struct vmp *s)
{
    uint64_t features;
    VirtIOMSG msg;

    features = s->regs.driver_features[1];
    features <<= 32;
    features |= s->regs.driver_features[0];

    virtio_msg_pack_set_features(&msg, 0, features);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_event_avail(struct vmp *s, uint32_t data)
{
    VirtIOMSG msg;
    uint32_t vq_index;
    uint64_t next_offset;
    uint64_t next_wrap;

    vq_index = data & 0xffff;
    next_offset = (data >> 16) & GENMASK(15, 0);
    next_wrap = data >> 31;

    virtio_msg_pack_event_avail(&msg, vq_index, next_offset, next_wrap);
    virtio_msg_bus_send(s, &msg);
}

static void vmp_notification(struct vcpu *v, unsigned int port)
{
    struct domain *d = v->domain;
    struct vmp *s = &d->arch.vmp;

    ASSERT(local_irq_is_enabled());
    spin_lock(&s->lock);
    vmp_rx_process(s);
    spin_unlock(&s->lock);
}

static void vmp_indirect_read(struct vmp *s, int size, uint16_t offset)
{
    uint32_t data = 0xDEADDEAD;

#if VMP_STATS
    if (offset < 0x100)
        s->stats.ind_regs[offset / 4]++;
#endif

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        data = VIRT_MAGIC;
        break;
    case VIRTIO_MMIO_VERSION:
        data = VIRT_VERSION;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        data = VIRT_VENDOR;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        vmp_indirect_set_busy(s, true, false);
        vmp_get_device_info(s);
        vmp_set_state(s, VMP_STATE_DEVICE_INFO);
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        data = s->regs.interrupt_status;
        break;
    case VIRTIO_MMIO_STATUS:
        vmp_indirect_set_busy(s, true, false);
        vmp_get_status(s);
        vmp_set_state(s, VMP_STATE_GET_DEVICE_STATUS);
        break;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        vmp_indirect_set_busy(s, true, false);
        vmp_get_features(s);
        vmp_set_state(s, VMP_STATE_GET_FEATURES);
        break;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        vmp_indirect_set_busy(s, true, false);
        vmp_get_config_gen(s);
        vmp_set_state(s, VMP_STATE_GET_CONFIG_GEN);
        break;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        data = s->regs.queue_max_size;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        data = s->vq[s->regs.queue_sel].enabled;
        break;
    case VIRTIO_MMIO_CONFIG ... (VIRTIO_MMIO_CONFIG + 0x100):
        vmp_indirect_set_busy(s, true, false);
        vmp_get_config(s, size, offset - VIRTIO_MMIO_CONFIG);
        vmp_set_state(s, VMP_STATE_GET_CONFIG);
        break;
    default:
        printk("#### %s: unhandled 0x%x = 0x%x\n", __func__, offset, data);
        ASSERT(0);
        break;
    }

    s->regs.access_data = data;
}

static void vmp_write64_low(uint64_t *reg, uint32_t val)
{
    uint64_t v = *reg;

    v &= GENMASK_ULL(63, 32);
    v |= val;
    *reg = v;
}

static void vmp_write64_high(uint64_t *reg, uint32_t val)
{
    uint64_t v = *reg;

    v &= GENMASK_ULL(31, 0);
    v |= ((uint64_t) val) << 32;
    *reg = v;
}

static void vmp_indirect_write(struct vmp *s, int size, uint16_t offset,
                               uint32_t data)
{
#if VMP_STATS
    if (offset < 0x100)
        s->stats.ind_regs[offset / 4]++;
#endif

    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        s->regs.device_features_sel = data;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        s->regs.driver_features_sel = data;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        s->regs.driver_features[s->regs.driver_features_sel & 1] = data;
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        s->regs.interrupt_status &= ~data;
        vmp_update_interrupts(s);
        break;
    case VIRTIO_MMIO_STATUS:
        s->regs.status = data;

        if (data & VIRTIO_CONFIG_S_FEATURES_OK) {
            /* Set features.  */
            vmp_indirect_set_busy(s, true, false);
            /* Send both messages.  */
            vmp_set_features(s);
            vmp_set_status(s);
            vmp_set_state(s, VMP_STATE_SET_FEATURES);
        } else {
            /* Setting status is non-blocking.  */
            vmp_set_status(s);
        }
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        if (data > VIRTIO_MSG_MAX_QUEUES) {
            printk("VIRTIO_MMIO_QUEUE_SEL out of bounds.\n");
            return;
        }

        ASSERT(s->regs.queue_sel < ARRAY_SIZE(s->vq));

        s->regs.queue_sel = data;
        vmp_indirect_set_busy(s, true, false);
        vmp_get_vqueue(s);
        vmp_set_state(s, VMP_STATE_GET_VQUEUE);
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        s->vq[s->regs.queue_sel].size = data;
        break;

    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        vmp_write64_low(&s->vq[s->regs.queue_sel].descriptor_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        vmp_write64_high(&s->vq[s->regs.queue_sel].descriptor_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        vmp_write64_low(&s->vq[s->regs.queue_sel].driver_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        vmp_write64_high(&s->vq[s->regs.queue_sel].driver_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
        vmp_write64_low(&s->vq[s->regs.queue_sel].device_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        vmp_write64_high(&s->vq[s->regs.queue_sel].device_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        /* This is posted.  */
        s->vq[s->regs.queue_sel].enabled = data;
        vmp_set_vqueue(s);
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        vmp_event_avail(s, data);
        break;
    case VIRTIO_MMIO_CONFIG ... (VIRTIO_MMIO_CONFIG + 0x100):
        vmp_indirect_set_busy(s, true, false);
        vmp_set_config(s, size, offset - VIRTIO_MMIO_CONFIG, data);
        vmp_set_state(s, VMP_STATE_SET_CONFIG);
        break;
    default:
        printk("##### %s: unhandled 0x%x = 0x%x\n", __func__, offset, data);
        ASSERT(0);
        break;
    }
}

static int vmp_mmio_read(struct vcpu *v, mmio_info_t *info,
                         register_t *r, void *priv)
{
    struct vmp *s = priv;
    paddr_t offset = info->gpa - s->base_addr;

    /* We never expect to start transactios with pending errors.  */
    BUG_ON(s->queues.error);
    ASSERT(local_irq_is_enabled());

#if VMP_STATS
    spin_lock(&s->lock);
    if (offset < 0x100)
        s->stats.regs[offset / 4]++;
    spin_unlock(&s->lock);
#endif

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        *r = VIRT_MAGIC;
        break;
    case VIRTIO_MMIO_VERSION:
        *r = VIRT_VERSION;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        *r = VIRT_VENDOR;
        break;
    case VIRTIO_MMIO_ACCESS:
        *r = s->regs.access;
        break;
    case VIRTIO_MMIO_ACCESS_DATA:
        /*
         * Make sure loads from regs.access don't get reorderd beyond here.
         * We need to read access before access_data if guest issued them in
         * that order.
         */
        smp_rmb();
        *r = s->regs.access_data;
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        *r = s->regs.interrupt_status;
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        /* DFT. We allow devemem2 to dummy read this.  */
        *r = 0;
        break;
    default:
        printk("%s: addr=%lx\n", __func__, offset);
        return 0;
    }
    return 1;
}

static int vmp_mmio_write(struct vcpu *v, mmio_info_t *info,
                          register_t r, void *priv)
{
    paddr_t offset = info->gpa - VMP_BASE;
    struct vmp *s = priv;
    uint32_t data = r;
    int size = 1 << info->dabt.size;
    struct {
        unsigned int offset;
        unsigned int size;
        bool is_write;
    } ind;

    /* We never expect to start transactios with pending errors.  */
    BUG_ON(s->queues.error);
    ASSERT(local_irq_is_enabled());

    spin_lock(&s->lock);
#if VMP_STATS
    if (offset < 0x100)
        s->stats.regs[offset / 4]++;
#endif

    /* Only 32bit direct writes are supported */
    if (size != 4)
        goto io_abort;

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        vmp_print_stats(s);
        break;
    case VIRTIO_MMIO_ACCESS:
        /* TODO: Do we need a way to abort transactions for dev restart? */
        if (s->state != VMP_STATE_IDLE) {
            printk("Indirect write while one is pending!\n");
            goto io_abort;
        }

        ind.offset = data & 0xffff;
        ind.size = 1 << ((data >> VIRTIO_MMIO_ACCESS_SIZE_SHIFT) & 3);
        ind.is_write = data & VIRTIO_MMIO_ACCESS_WRITE;

        /*
         * Check that size is 1, 2 or 4. Nothing else is supported.
         *
         * Size cannot be 0 since we're starting by 1 and shifting left.
         * The shift value is 2 bits, can only be between 0 - 3.
         * ind.size can only be a power of 2.
         */
        if (ind.size > 4) {
            printk("%s Bad indirect access size %d!\n", __func__, ind.size);
            goto io_abort;
        }

        /* Validate alignment.  */
        if (ind.offset & (ind.size - 1)) {
            printk("%s: Bad indirect unalinged access! size %d offset %x\n",
                    __func__, ind.size, ind.offset);
            goto io_abort;
        }

        /* 
         * Good access, default regs.access to not busy and no error.
         * Let indirect handlers override it when needed.
         */
        s->regs.access = data;
        vmp_indirect_set_busy(s, false, false);
        if (ind.is_write)
            vmp_indirect_write(s, ind.size, ind.offset, s->regs.access_data);
        else
            vmp_indirect_read(s, ind.size, ind.offset);

        if ( s->queues.error ) {
            /* Access failed. Abort it and signal error.  */
            printk("ERROR RETRY offset %x data %x\n",
                   ind.offset, s->regs.access_data);
            vmp_indirect_set_busy(s, false, true);
            vmp_set_state(s, VMP_STATE_IDLE);
        }
        break;
    case VIRTIO_MMIO_ACCESS_DATA:
        if (s->state != VMP_STATE_IDLE) {
            printk("Clobbering access_data while busy\n");
            goto io_abort;
        }
        s->regs.access_data = data;
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        s->regs.interrupt_status &= ~data;
        vmp_update_interrupts(s);
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        vmp_event_avail(s, data);
        if (s->queues.error) {
            /* To see errors, indirect writes must be used.  */
            /* FIXME: Should we allow this?  */
            printk("NOTIFY FAILED! Silently dropped.\n");
            s->queues.error = false;
        }
        break;
    default:
        printk("%s: Write to unkown register! %lx = %x\n", __func__,
               offset, data);
        goto io_abort;
    }

    spin_unlock(&s->lock);
    return 1;
io_abort:
    printk("%s: FATAL: Bad write! state %d size=%d offset %lx data=%x\n",
            __func__, s->state, size, offset, data);
    spin_unlock(&s->lock);
    return 0;
}

static const struct mmio_handler_ops vmp_mmio_handler = {
    .read  = vmp_mmio_read,
    .write = vmp_mmio_write,
};

int domain_vmp_init(struct domain *d)
{
    struct vmp *s = &d->arch.vmp;
    int capacity = spsc_capacity(VMP_QUEUE_SIZE);
    int rc;

    /* We need regs.access to be at least 32bit.  */
    BUILD_BUG_ON(sizeof s->regs.access < sizeof(uint32_t));

    memset(s, 0, sizeof *s);

    spin_lock_init(&s->lock);
    s->domain = d;

    rc = prepare_ring_for_helper(d, GUEST_VIRTIO_PROXY_BASE >> PAGE_SHIFT,
                                 &s->shm.page, &s->shm.ptr);
    if ( rc < 0 )
        goto out;

    memset(s->shm.ptr, 0, 0x1000);
    s->queues.driver = spsc_open_mem("driver", capacity, s->shm.ptr);
    s->queues.device = spsc_open_mem("device", capacity,
                                     s->shm.ptr + VMP_QUEUE_SIZE);

    rc = alloc_unbound_xen_event_channel(d, 0, VMP_EMULATOR_DOMID,
                                         vmp_notification);
    if ( rc < 0 )
        goto out1;

    s->evtchn = rc;

    rc = vgic_reserve_virq(d, GUEST_VIRTIO_MMIO_SPI_FIRST);
    if ( !rc )
    {
        rc = -EINVAL;
        goto out1;
    }

    s->virq = GUEST_VIRTIO_MMIO_SPI_FIRST;
    s->base_addr = VMP_BASE;
    register_mmio_handler(d, &vmp_mmio_handler, s->base_addr, VMP_SIZE, s);
    return 0;
out1:
    domain_vmp_deinit(d);
out:
    return rc;
}

void domain_vmp_deinit(struct domain *d)
{
    struct vmp *s = &d->arch.vmp;

    if (s->virq)
        vgic_free_virq(d, s->virq);

    if ( s->shm.ptr )
        destroy_ring_for_helper(&s->shm.ptr, s->shm.page);

    if ( s->evtchn ) {
        free_xen_event_channel(d, s->evtchn);
    }
}
