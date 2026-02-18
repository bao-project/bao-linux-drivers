// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixoto@osyx.tech>
 *	José Martins <jose@osyx.tech>
 *	David Cerdeira <davidmcerdeira@osyx.tech>
 */

#include <bao.h>
#include <hypercall.h>

/**
 * struct bao_io_dispatcher_work - Work item for I/O dispatching
 * @work: Work struct for scheduling on workqueue
 * @dm: Pointer to the associated Bao device model
 *
 * Represents a single work item that dispatches I/O requests
 * for a specific Bao device model.
 */
struct bao_io_dispatcher_work {
    struct work_struct work;
    struct bao_dm* dm;
};

/* Array of I/O dispatcher work items, one per Bao DM */
static struct bao_io_dispatcher_work io_dispatcher_work[BAO_IO_MAX_DMS];

/* Workqueues dedicated to dispatching I/O requests for each Bao DM */
static struct workqueue_struct* bao_io_dispatcher_wq[BAO_IO_MAX_DMS];

void bao_io_dispatcher_destroy(struct bao_dm* dm)
{
    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    if (bao_io_dispatcher_wq[dm->info.id]) {
        bao_io_dispatcher_pause(dm);

        destroy_workqueue(bao_io_dispatcher_wq[dm->info.id]);
        bao_io_dispatcher_wq[dm->info.id] = NULL;

        bao_intc_remove_handler();
    }
}

int bao_dispatch_io(struct bao_dm* dm)
{
    struct bao_io_client* client;
    struct bao_remio_hypercall_ctx ctx;
    struct bao_virtio_request req;

    if (WARN_ON_ONCE(!dm)) {
        return -EINVAL;
    }

    ctx.dm_id = dm->info.id;
    ctx.op = BAO_IO_ASK;
    ctx.addr = 0;
    ctx.value = 0;
    ctx.request_id = 0;

    if (bao_remio_hypercall(&ctx)) {
        return -EFAULT;
    }

    req.dm_id = ctx.dm_id;
    req.op = ctx.op;
    req.addr = ctx.addr;
    req.value = ctx.value;
    req.access_width = ctx.access_width;
    req.request_id = ctx.request_id;

    down_read(&dm->io_clients_lock);
    client = bao_io_client_find(dm, &req);
    if (!client) {
        up_read(&dm->io_clients_lock);
        return -ENODEV;
    }

    if (!bao_io_client_push_request(client, &req)) {
        up_read(&dm->io_clients_lock);
        return -EINVAL;
    }

    wake_up_interruptible(&client->wq);
    up_read(&dm->io_clients_lock);

    return ctx.npend_req;
}

/**
 * io_dispatcher - Workqueue handler for dispatching I/O
 * @work: Work struct representing this dispatch operation
 *
 * Handles all pending I/O requests for the associated Bao DM.
 * Executed in process context by the workqueue.
 */
static void io_dispatcher(struct work_struct* work)
{
    struct bao_io_dispatcher_work* bao_dm_work;
    struct bao_dm* dm;

    if (WARN_ON_ONCE(!work)) {
        return;
    }

    bao_dm_work = container_of(work, struct bao_io_dispatcher_work, work);
    dm = bao_dm_work->dm;

    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    while (bao_dispatch_io(dm) > 0) {
        cpu_relax();
    }
}

/**
 * io_dispatcher_intc_handler - Interrupt handler for I/O requests
 * @dm: Bao device model that triggered the interrupt
 *
 * Invoked by the interrupt controller when a new I/O request is available.
 * Queues the corresponding work item onto the I/O dispatcher workqueue
 * for processing in process context.
 */
static void io_dispatcher_intc_handler(struct bao_dm* dm)
{
    if (WARN_ON_ONCE(!dm || !bao_io_dispatcher_wq[dm->info.id])) {
        return;
    }

    queue_work(bao_io_dispatcher_wq[dm->info.id], &io_dispatcher_work[dm->info.id].work);
}

void bao_io_dispatcher_pause(struct bao_dm* dm)
{
    if (WARN_ON_ONCE(!dm || !bao_io_dispatcher_wq[dm->info.id])) {
        return;
    }

    bao_intc_remove_handler();

    drain_workqueue(bao_io_dispatcher_wq[dm->info.id]);
}

void bao_io_dispatcher_resume(struct bao_dm* dm)
{
    if (WARN_ON_ONCE(!dm || !bao_io_dispatcher_wq[dm->info.id])) {
        return;
    }

    bao_intc_setup_handler(io_dispatcher_intc_handler);

    queue_work(bao_io_dispatcher_wq[dm->info.id], &io_dispatcher_work[dm->info.id].work);
}

int bao_io_dispatcher_init(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];

    if (WARN_ON_ONCE(!dm)) {
        return -EINVAL;
    }

    snprintf(name, sizeof(name), "bao-iodwq%u", dm->info.id);

    if (bao_io_dispatcher_wq[dm->info.id]) {
        return -EBUSY;
    }
    bao_io_dispatcher_wq[dm->info.id] = alloc_workqueue(name, WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
    if (!bao_io_dispatcher_wq[dm->info.id]) {
        return -ENOMEM;
    }

    io_dispatcher_work[dm->info.id].dm = dm;
    INIT_WORK(&io_dispatcher_work[dm->info.id].work, io_dispatcher);

    bao_intc_setup_handler(io_dispatcher_intc_handler);

    return 0;
}
