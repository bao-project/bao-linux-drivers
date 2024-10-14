// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor Irqfd Server
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include "hypercall.h"

#include "bao.h"

/**
 * struct irqfd - Properties of irqfd
 * @dm:	Associated DM pointer
 * @wait: Entry of wait-queue
 * @shutdown: Async shutdown work
 * @eventfd: Associated eventfd to poll
 * @list: Entry within &bao_dm.irqfds of irqfds of a DM
 * @pt:	Structure for select/poll on the associated eventfd
 */
struct irqfd {
	struct bao_dm *dm;
	wait_queue_entry_t wait;
	struct work_struct shutdown;
	struct eventfd_ctx *eventfd;
	struct list_head list;
	poll_table pt;
};

/**
 * Shutdown a irqfd
 * @irqfd: The irqfd to shutdown
 */
static void bao_irqfd_shutdown(struct irqfd *irqfd)
{
	u64 cnt;
	lockdep_assert_held(&irqfd->dm->irqfds_lock);

	// delete the irqfd from the list of irqfds
	list_del_init(&irqfd->list);

	// remove the irqfd from the wait queue
	eventfd_ctx_remove_wait_queue(irqfd->eventfd, &irqfd->wait, &cnt);

	// release the eventfd
	eventfd_ctx_put(irqfd->eventfd);

	// free the irqfd
	kfree(irqfd);
}

/**
 * Inject a notify hypercall into the Bao Hypervisor
 * @id: The DM ID
 */
static int bao_irqfd_inject(int id)
{
	struct bao_virtio_request request = {
		.dm_id = id,
		.addr = 0,
		.op = BAO_IO_NOTIFY,
		.value = 0,
		.access_width = 0,
		.request_id = 0,
	};

	// notify the Hypervisor about the event
	struct remio_hypercall_ret ret = bao_hypercall_remio(&request);

	if (ret.hyp_ret != 0 || ret.remio_hyp_ret != 0) {
		return -EFAULT;
	}
	return 0;
}

/**
 * Custom wake-up handling to be notified whenever underlying eventfd is signaled.
 * @note: This function will be called by Linux kernel poll table (irqfd->pt) whenever the eventfd is signaled.
 * @wait: Entry of wait-queue
 * @mode: Mode
 * @sync: Sync
 * @key: Poll bits
 * @return int
 */
static int bao_irqfd_wakeup(wait_queue_entry_t *wait, unsigned int mode,
			    int sync, void *key)
{
	unsigned long poll_bits = (unsigned long)key;
	struct irqfd *irqfd;
	struct bao_dm *dm;

	// get the irqfd object from the wait queue
	irqfd = container_of(wait, struct irqfd, wait);

	// get the DM from the irqfd
	dm = irqfd->dm;

	// check if the event is signaled
	if (poll_bits & POLLIN)
		// an event has been signaled, inject a irqfd
		bao_irqfd_inject(dm->info.id);

	if (poll_bits & POLLHUP)
		// do shutdown work in thread to hold wqh->lock
		queue_work(dm->irqfd_server, &irqfd->shutdown);

	return 0;
}

/**
 * Register the file descriptor with the poll table and associate it with a wait queue
 * that the kernel will monitor for events
 * @file: The file to poll
 * @wqh: The wait queue head
 * @pt: The poll table
 */
static void bao_irqfd_poll_func(struct file *file, wait_queue_head_t *wqh,
				poll_table *pt)
{
	struct irqfd *irqfd;

	// get the irqfd from the file
	irqfd = container_of(pt, struct irqfd, pt);
	// add the irqfd wait queue entry to the wait queue
	add_wait_queue(wqh, &irqfd->wait);
}

/**
 * Shutdown a irqfd
 * @work: The work to shutdown the irqfd
 */
static void irqfd_shutdown_work(struct work_struct *work)
{
	struct irqfd *irqfd;
	struct bao_dm *dm;

	// get the irqfd from the work
	irqfd = container_of(work, struct irqfd, shutdown);

	// get the DM from the irqfd
	dm = irqfd->dm;

	// shutdown the irqfd
	mutex_lock(&dm->irqfds_lock);
	if (!list_empty(&irqfd->list))
		bao_irqfd_shutdown(irqfd);
	mutex_unlock(&dm->irqfds_lock);
}

/**
 * Assign an eventfd to a DM and create the associated irqfd.
 * @dm: The DM to assign the eventfd
 * @args: The &struct bao_irqfd to assign
 */
static int bao_irqfd_assign(struct bao_dm *dm, struct bao_irqfd *args)
{
	struct eventfd_ctx *eventfd = NULL;
	struct irqfd *irqfd, *tmp;
	__poll_t events;
	struct fd f;
	int ret = 0;

	// allocate a new irqfd object
	irqfd = kzalloc(sizeof(*irqfd), GFP_KERNEL);
	if (!irqfd)
		return -ENOMEM;

	// initialize the irqfd
	irqfd->dm = dm;
	INIT_LIST_HEAD(&irqfd->list);
	INIT_WORK(&irqfd->shutdown, irqfd_shutdown_work);

	// get a reference to the file descriptor
	f = fdget(args->fd);
	if (!f.file) {
		ret = -EBADF;
		goto out;
	}

	// get the eventfd from the file descriptor
	eventfd = eventfd_ctx_fileget(f.file);
	if (IS_ERR(eventfd)) {
		ret = PTR_ERR(eventfd);
		goto fail;
	}

	// assign the eventfd to the irqfd
	irqfd->eventfd = eventfd;

	// define the custom callback for the wait queue to be notified whenever underlying eventfd is signaled
	// (in this case we don't need to wake-up any task, just to be notified when the eventfd is signaled)
	init_waitqueue_func_entry(&irqfd->wait, bao_irqfd_wakeup);

	// define the custom poll function behavior
	init_poll_funcptr(&irqfd->pt, bao_irqfd_poll_func);

	// add the irqfd to the list of irqfds of the DM
	mutex_lock(&dm->irqfds_lock);
	list_for_each_entry(tmp, &dm->irqfds, list) {
		if (irqfd->eventfd != tmp->eventfd)
			continue;
		ret = -EBUSY;
		mutex_unlock(&dm->irqfds_lock);
		goto fail;
	}
	list_add_tail(&irqfd->list, &dm->irqfds);
	mutex_unlock(&dm->irqfds_lock);

	// check the pending event in this stage by calling vfs_poll function
	// (this function will internally call the custom poll function already defined)
	// any event signaled upon this stage will be handled by the custom poll function
	events = vfs_poll(f.file, &irqfd->pt);

	// if the event is signaled, signal Bao Hypervisor
	if (events & EPOLLIN)
		bao_irqfd_inject(dm->info.id);

	// release the file descriptor reference
	fdput(f);
	return 0;
fail:
	if (eventfd && !IS_ERR(eventfd))
		eventfd_ctx_put(eventfd);

	fdput(f);
out:
	kfree(irqfd);
	return ret;
}

/**
 * Deassign an eventfd from a DM and destroy the associated irqfd.
 * @dm: The DM to deassign the eventfd
 * @args: The &struct bao_irqfd to deassign
 */
static int bao_irqfd_deassign(struct bao_dm *dm, struct bao_irqfd *args)
{
	struct irqfd *irqfd, *tmp;
	struct eventfd_ctx *eventfd;

	// get the eventfd from the file descriptor
	eventfd = eventfd_ctx_fdget(args->fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	// find the irqfd associated with the eventfd and shutdown it
	mutex_lock(&dm->irqfds_lock);
	list_for_each_entry_safe(irqfd, tmp, &dm->irqfds, list) {
		if (irqfd->eventfd == eventfd) {
			bao_irqfd_shutdown(irqfd);
			break;
		}
	}
	mutex_unlock(&dm->irqfds_lock);

	// release the eventfd
	eventfd_ctx_put(eventfd);

	return 0;
}

int bao_irqfd_server_config(struct bao_dm *dm, struct bao_irqfd *config)
{
	// check if the DM and configuration are valid
	if (WARN_ON(!dm || !config))
		return -EINVAL;

	// deassign the eventfd
	if (config->flags & BAO_IRQFD_FLAG_DEASSIGN)
		return bao_irqfd_deassign(dm, config);

	// assign the eventfd
	return bao_irqfd_assign(dm, config);
}

int bao_irqfd_server_init(struct bao_dm *dm)
{
	char name[BAO_NAME_MAX_LEN];

	mutex_init(&dm->irqfds_lock);
	INIT_LIST_HEAD(&dm->irqfds);

	// create a new name for the irqfd server based on type and DM ID
	snprintf(name, sizeof(name), "bao-ioirqfds%u", dm->info.id);

	// allocate a new workqueue for the irqfd
	dm->irqfd_server = alloc_workqueue(name, 0, 0);
	if (!dm->irqfd_server)
		return -ENOMEM;

	return 0;
}

void bao_irqfd_server_destroy(struct bao_dm *dm)
{
	struct irqfd *irqfd, *next;

	// destroy the workqueue
	destroy_workqueue(dm->irqfd_server);
	mutex_lock(&dm->irqfds_lock);
	// shutdown all the irqfds
	list_for_each_entry_safe(irqfd, next, &dm->irqfds, list)
		bao_irqfd_shutdown(irqfd);
	mutex_unlock(&dm->irqfds_lock);
}