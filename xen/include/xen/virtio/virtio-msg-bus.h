/*
 * virtio-msg-bus interface.
 *
 * Very basic, supporting only a single bus implementation for now.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __XEN_VIRTIO_MSG_BUS_H__
#define __XEN_VIRTIO_MSG_BUS_H__

#include <xen/dm.h>
#include <xen/virtio/std/virtio-msg-prot.h>

struct virtio_msg_bus;

struct virtio_msg_bus_client {
    /* rx_msg, called by lower layers to deliver a virtio msg.  */
    void (*rx_msg)(void *opaque, struct VirtIOMSG *msg);

    /* deinit, called by lower layers when the bus gets destroyed.  */
    void (*deinit)(void *opaque);
};

struct virtio_msg_bus_channel {
    const char *name;
    /*
     * Ask the bus to receive and process messages that
     * are readily available. The bus will call the registered
     * client->rx_msg() function for each message.
     *
     * Will return immediately if no messages are available.
     */
    void (*process)(struct virtio_msg_bus *bd);

    /*
     * Called by the transport to see if the bus is applying back-pressure.
     */
    bool (*back_pressure)(struct virtio_msg_bus *bd);

    /*
     * Called by the transport to send a message.
     */
    bool (*send)(struct virtio_msg_bus *bd, VirtIOMSG *msg_req);

    /* Called to destroy the client. */
    void (*deinit)(struct virtio_msg_bus *bd);
};

enum virtio_msg_bus_state {
    STATE_DISABLED = 0, /* Bus is not in use.  */
    STATE_ENABLED,      /* Bus created, no device backend connected.  */
    STATE_CONNECTED     /* Bus with a connected device backend.  */
};

/* Abstract bus.  */
struct virtio_msg_bus {
    /* Specialized bus channel implementation.  */
    const struct virtio_msg_bus_channel *channel;
    /* Bus client, e.g virtio-msg-bus transport or proxy layer.  */
    const struct virtio_msg_bus_client *client;
    void *client_opaque;

    /* Domain that drives the virtio-msg-bus.  */
    struct domain *domain;

    atomic_t state;
};

static inline enum virtio_msg_bus_state
virtio_msg_bus_get_state(struct virtio_msg_bus *bd)
{
    enum virtio_msg_bus_state state;
    state = atomic_read(&bd->state);
    /*
     * Ensure that the state value is read before any subsequent loads
     * from the bus descriptor. Prevents reordering so that if the state
     * indicates the bus is ready, later reads will observe consistent data.
     */
    smp_rmb();

    return state;
}

static inline void virtio_msg_bus_set_state(struct virtio_msg_bus *bd,
                                            enum virtio_msg_bus_state state)
{
    /* Ensure prior writes become visible before publishing the state. */
    smp_wmb();
    atomic_set(&bd->state, state);
}

static inline void
virtio_msg_bus_attach_client(struct virtio_msg_bus *bd,
                             const struct virtio_msg_bus_client *client,
                             void *client_opaque)
{
    ASSERT(bd);
    ASSERT(!bd->client);
    ASSERT(client);
    ASSERT(client->rx_msg);

    bd->client = client;
    bd->client_opaque = client_opaque;
}

static inline void
virtio_msg_bus_init(struct virtio_msg_bus *bd,
                    const struct virtio_msg_bus_channel *channel,
                    struct domain *d)
{
    bd->channel = channel;
    bd->domain = d;

    virtio_msg_bus_set_state(bd, STATE_ENABLED);
}

int virtio_msg_bus_dm_op(struct xen_dm_op *op, struct domain *d,
                         bool *const_op);

static inline bool virtio_msg_bus_back_pressure(struct virtio_msg_bus *bd)
{
    bool bp = false;

    ASSERT(bd && bd->channel);
    if ( bd->channel->back_pressure(bd) )
    {
        bp = bd->channel->back_pressure(bd);
    }
    return bp;
}

/* Is the bus ready to send? */
static inline bool virtio_msg_bus_ready(struct virtio_msg_bus *bd)
{
    ASSERT(bd);

    /* The bus is considered ready when connected and not in back-pressure.  */
    return virtio_msg_bus_get_state(bd) == STATE_CONNECTED &&
           !virtio_msg_bus_back_pressure(bd);
}

static inline bool virtio_msg_bus_send(struct virtio_msg_bus *bd,
                                       VirtIOMSG *msg_req)
{
    /* Mandatory method.  */
    ASSERT(bd && bd->channel && bd->channel->send);
    return bd->channel->send(bd, msg_req);
}

static inline void virtio_msg_bus_rx_process(struct virtio_msg_bus *bd)
{
    ASSERT(bd && bd->channel);
    if ( bd->channel->process )
    {
        bd->channel->process(bd);
    }
}

static inline void virtio_msg_bus_deinit(struct virtio_msg_bus *bd)
{
    ASSERT(bd);

    /* Stop new processing paths first. */
    virtio_msg_bus_set_state(bd, STATE_DISABLED);

    /* Quiesce the backend/channel so no more process() can run. */
    if (bd->channel && bd->channel->deinit)
        bd->channel->deinit(bd);

    /* Now it’s safe to tear down the client. */
    if (bd->client && bd->client->deinit)
        bd->client->deinit(bd->client_opaque);

    bd->client = NULL;
    bd->client_opaque = NULL;
    bd->channel = NULL;
    bd->domain = NULL;
    xfree(bd);
}

static inline void virtio_msg_bus_domain_disable(struct domain *d)
{
#ifdef CONFIG_VIRTIO_MSG_BUS_CORE
    unsigned int i;

    for ( i = 0; i < ARRAY_SIZE(d->virtio_msg_bus); i++ )
    {
        struct virtio_msg_bus *bd = rcu_dereference(d->virtio_msg_bus[i]);

        /* Disable all buses. Wait for RCU call to destroy the objects. */
        if ( bd )
            virtio_msg_bus_set_state(bd, STATE_DISABLED);
    }
#endif
}

static inline void virtio_msg_bus_domain_destroy(struct domain *d)
{
#ifdef CONFIG_VIRTIO_MSG_BUS_CORE
    unsigned int i;

    for ( i = 0; i < ARRAY_SIZE(d->virtio_msg_bus); i++ )
    {
        struct virtio_msg_bus *bd = rcu_dereference(d->virtio_msg_bus[i]);

        rcu_assign_pointer(d->virtio_msg_bus[i], NULL);
        if ( bd )
            virtio_msg_bus_deinit(bd);
    }
#endif
}
#endif /* __XEN_VIRTIO_MSG_BUS_H__ */
