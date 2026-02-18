// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor Irqfd Server
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
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/poll.h>

/**
 * struct irqfd - Properties of an IRQ eventfd
 * @dm: Associated Bao device model
 * @wait: Wait queue entry for blocking/waking
 * @shutdown: Work struct for async shutdown
 * @eventfd: Eventfd used to signal interrupts
 * @list: List node within &bao_dm.irqfds
 * @pt: Poll table for select/poll on the eventfd
 *
 * Represents an IRQ eventfd registered to a Bao device model.
 */
struct irqfd {
    struct bao_dm* dm;
    wait_queue_entry_t wait;
    struct work_struct shutdown;
    struct eventfd_ctx* eventfd;
    struct list_head list;
    poll_table pt;
};

/**
 * bao_irqfd_shutdown - Release and remove an irqfd
 * @irqfd: IRQ eventfd to shut down (lock must be held)
 */
static void bao_irqfd_shutdown(struct irqfd* irqfd)
{
    u64 cnt;

    if (WARN_ON_ONCE(!irqfd || !irqfd->dm)) {
        return;
    }

    lockdep_assert_held(&irqfd->dm->irqfds_lock);

    list_del_init(&irqfd->list);

    eventfd_ctx_remove_wait_queue(irqfd->eventfd, &irqfd->wait, &cnt);

    eventfd_ctx_put(irqfd->eventfd);

    kfree(irqfd);
}

/**
 * bao_irqfd_inject - Inject a notify hypercall into the Bao hypervisor
 * @id: Bao DM ID
 *
 * Return: 0 on success, -EFAULT if the hypercall fails.
 */
static int bao_irqfd_inject(int id)
{
    struct bao_remio_hypercall_ctx ctx = {
        .dm_id = id,
        .addr = 0,
        .op = BAO_IO_NOTIFY,
        .value = 0,
        .access_width = 0,
        .request_id = 0,
    };

    if (bao_remio_hypercall(&ctx)) {
        return -EFAULT;
    }

    return 0;
}

/**
 * bao_irqfd_wakeup - Custom wake-up handler for eventfd signaling
 * @wait: Wait queue entry
 * @mode: Mode flags
 * @sync: Sync indicator
 * @key: Poll bits (cast from void *)
 *
 * Called by the Linux kernel poll table when the underlying eventfd is signaled.
 * Injects a Bao notify hypercall on POLLIN or schedules shutdown on POLLHUP.
 *
 * Return: 0 on success, a negative error code on failure
 */
static int bao_irqfd_wakeup(wait_queue_entry_t* wait, unsigned int mode, int sync, void* key)
{
    struct irqfd* irqfd;
    struct bao_dm* dm;
    unsigned long poll_bits;

    if (WARN_ON_ONCE(!wait || !key)) {
        return -EINVAL;
    }

    irqfd = container_of(wait, struct irqfd, wait);
    dm = irqfd->dm;
    poll_bits = (unsigned long)key;

    if (poll_bits & POLLIN) {
        bao_irqfd_inject(dm->info.id);
    }

    if (poll_bits & POLLHUP) {
        queue_work(dm->irqfd_server, &irqfd->shutdown);
    }

    return 0;
}

/**
 * bao_irqfd_poll_func - Register an IRQFD with a poll table
 * @file: File to poll
 * @wqh: Wait queue head
 * @pt: Poll table
 *
 * Adds the irqfd's wait queue entry to the kernel wait queue for event monitoring.
 */
static void bao_irqfd_poll_func(struct file* file, wait_queue_head_t* wqh, poll_table* pt)
{
    struct irqfd* irqfd;

    if (WARN_ON_ONCE(!pt || !wqh)) {
        return;
    }

    irqfd = container_of(pt, struct irqfd, pt);
    add_wait_queue(wqh, &irqfd->wait);
}

/**
 * irqfd_shutdown_work - Workqueue handler to shutdown an irqfd
 * @work: Work struct for the shutdown operation
 *
 * Removes and frees the irqfd from the DM under lock if it is still linked.
 */
static void irqfd_shutdown_work(struct work_struct* work)
{
    struct irqfd* irqfd;
    struct bao_dm* dm;

    if (WARN_ON_ONCE(!work)) {
        return;
    }

    irqfd = container_of(work, struct irqfd, shutdown);
    dm = irqfd->dm;

    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    mutex_lock(&dm->irqfds_lock);
    if (!list_empty(&irqfd->list)) {
        bao_irqfd_shutdown(irqfd);
    }
    mutex_unlock(&dm->irqfds_lock);
}

/**
 * bao_irqfd_assign - Assign an eventfd to a DM and create an irqfd
 * @dm: Bao device model to assign the eventfd
 * @args: Configuration of the irqfd to assign
 *
 * Return: 0 on success, a negative error code on failure
 */
static int bao_irqfd_assign(struct bao_dm* dm, struct bao_irqfd* args)
{
    struct eventfd_ctx* eventfd = NULL;
    struct irqfd* irqfd;
    struct irqfd* tmp;
    __poll_t events;
    struct fd f;
    int ret = 0;

    if (WARN_ON_ONCE(!dm || !args)) {
        return -EINVAL;
    }

    irqfd = kzalloc(sizeof(*irqfd), GFP_KERNEL);
    if (!irqfd) {
        return -ENOMEM;
    }

    irqfd->dm = dm;
    INIT_LIST_HEAD(&irqfd->list);
    INIT_WORK(&irqfd->shutdown, irqfd_shutdown_work);

    f = fdget(args->fd);
    if (!fd_file(f)) {
        ret = -EBADF;
        goto out_free_irqfd;
    }

    eventfd = eventfd_ctx_fileget(fd_file(f));
    if (IS_ERR(eventfd)) {
        ret = PTR_ERR(eventfd);
        goto out_fdput;
    }
    irqfd->eventfd = eventfd;

    init_waitqueue_func_entry(&irqfd->wait, bao_irqfd_wakeup);
    init_poll_funcptr(&irqfd->pt, bao_irqfd_poll_func);

    mutex_lock(&dm->irqfds_lock);
    list_for_each_entry(tmp, &dm->irqfds, list)
    {
        if (irqfd->eventfd == tmp->eventfd) {
            ret = -EBUSY;
            mutex_unlock(&dm->irqfds_lock);
            goto out_put_eventfd;
        }
    }
    list_add_tail(&irqfd->list, &dm->irqfds);
    mutex_unlock(&dm->irqfds_lock);

    events = vfs_poll(fd_file(f), &irqfd->pt);
    if (events & EPOLLIN) {
        bao_irqfd_inject(dm->info.id);
    }

    fdput(f);
    return 0;

out_put_eventfd:
    eventfd_ctx_put(eventfd);
out_fdput:
    fdput(f);
out_free_irqfd:
    kfree(irqfd);
    return ret;
}

/**
 * bao_irqfd_deassign - Deassign an eventfd and destroy the associated irqfd
 * @dm: Bao device model to remove the irqfd from
 * @args: Configuration of the irqfd to deassign
 *
 * Return: 0 on success, a negative error code on failure
 */
static int bao_irqfd_deassign(struct bao_dm* dm, struct bao_irqfd* args)
{
    struct irqfd* irqfd;
    struct irqfd* tmp;
    struct eventfd_ctx* eventfd;

    if (WARN_ON_ONCE(!dm || !args)) {
        return -EINVAL;
    }

    eventfd = eventfd_ctx_fdget(args->fd);
    if (IS_ERR(eventfd)) {
        return PTR_ERR(eventfd);
    }

    mutex_lock(&dm->irqfds_lock);
    list_for_each_entry_safe(irqfd, tmp, &dm->irqfds, list)
    {
        if (irqfd->eventfd == eventfd) {
            bao_irqfd_shutdown(irqfd);
            break;
        }
    }
    mutex_unlock(&dm->irqfds_lock);

    eventfd_ctx_put(eventfd);

    return 0;
}

int bao_irqfd_server_config(struct bao_dm* dm, struct bao_irqfd* config)
{
    if (WARN_ON_ONCE(!dm || !config)) {
        return -EINVAL;
    }

    if (config->flags & BAO_IRQFD_FLAG_DEASSIGN) {
        return bao_irqfd_deassign(dm, config);
    }

    return bao_irqfd_assign(dm, config);
}

int bao_irqfd_server_init(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];

    if (WARN_ON_ONCE(!dm)) {
        return -EINVAL;
    }

    mutex_init(&dm->irqfds_lock);
    INIT_LIST_HEAD(&dm->irqfds);

    snprintf(name, sizeof(name), "bao-ioirqfds%u", dm->info.id);

    dm->irqfd_server = alloc_workqueue(name, WQ_UNBOUND | WQ_HIGHPRI, 0);
    if (!dm->irqfd_server) {
        return -ENOMEM;
    }

    return 0;
}

void bao_irqfd_server_destroy(struct bao_dm* dm)
{
    struct irqfd* irqfd;
    struct irqfd* next;

    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    if (dm->irqfd_server) {
        destroy_workqueue(dm->irqfd_server);
    }

    mutex_lock(&dm->irqfds_lock);
    list_for_each_entry_safe(irqfd, next, &dm->irqfds, list) bao_irqfd_shutdown(irqfd);
    mutex_unlock(&dm->irqfds_lock);
}
