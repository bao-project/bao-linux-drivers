// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/eventfd.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "bao.h"
#include "hypercall.h"

// Define a wrapper structure that contains both work_struct and the private data (bao_dm)
struct bao_io_dispatcher_work {
    struct work_struct work;
    struct bao_dm *dm;
};

static struct bao_io_dispatcher_work io_dispatcher_work[BAO_IO_MAX_DMS];

/**
 * Responsible for dispatching I/O requests for all I/O DMs
 * This function is called by the workqueue
 * @work: The work struct
 */
static void io_dispatcher(struct work_struct *work);
// Workqueue for the I/O requests
static struct workqueue_struct *bao_io_dispatcher_wq[BAO_IO_MAX_DMS];

void bao_io_dispatcher_destroy(struct bao_dm *dm)
{
	// if the workqueue exists
	if (bao_io_dispatcher_wq[dm->info.id]) {
		// pause the I/O Dispatcher
		bao_io_dispatcher_pause(dm);
		// destroy the I/O Dispatcher workqueue
		destroy_workqueue(bao_io_dispatcher_wq[dm->info.id]);
		// remove the interrupt handler
		bao_intc_remove_handler();
	}
}

int bao_dispatch_io(struct bao_dm *dm)
{
	struct bao_io_client *client;
	struct bao_virtio_request req;
	struct remio_hypercall_ret ret;

	// update the request
	// the dm_id is the Virtual Remote I/O ID
	req.dm_id = dm->info.id;
	// BAO_IO_ASK will extract the I/O request from the Remote I/O system
	req.op = BAO_IO_ASK;
	// clear the other fields (convention)
	req.addr = 0;
	req.value = 0;
	req.request_id = 0;

	// perform a Hypercall to get the I/O request from the Remote I/O system
	// the ret.pending_requests value holds the number of requests that still need to be processed
	ret = bao_hypercall_remio(&req);

	if (ret.hyp_ret != 0 || ret.remio_hyp_ret != 0) {
		return -EFAULT;
	}

	// find the I/O client that the I/O request belongs to
	down_read(&dm->io_clients_lock);
	client = bao_io_client_find(dm, &req);
	if (!client) {
		up_read(&dm->io_clients_lock);
		return -EEXIST;
	}

	// add the request to the end of the virtio_request list
	bao_io_client_push_request(client, &req);

	// wake up the handler thread which is waiting for requests on the wait queue
	wake_up_interruptible(&client->wq);
	up_read(&dm->io_clients_lock);

	// return the number of request that still need to be processed
	return ret.pending_requests;
}

static void io_dispatcher(struct work_struct *work)
{
	struct bao_io_dispatcher_work *bao_dm_work = container_of(work, struct bao_io_dispatcher_work, work);
    struct bao_dm *dm = bao_dm_work->dm;

	// dispatch the I/O request for the device model
	while (bao_dispatch_io(dm) > 0)
		; // while there are requests to be processed
}

/**
 * Interrupt Controller handler for the I/O requests
 * @note: This function is called by the interrupt controller
 * when an interrupt is triggered (when a new I/O request is available)
 * @dm: The DM that triggered the interrupt
 */
static void io_dispatcher_intc_handler(struct bao_dm *dm)
{
	// add the work to the workqueue
	queue_work(bao_io_dispatcher_wq[dm->info.id], &io_dispatcher_work[dm->info.id].work);
}

void bao_io_dispatcher_pause(struct bao_dm *dm)
{
	// remove the interrupt handler
	bao_intc_remove_handler();
	// drain the workqueue (wait for all the work to finish)
	drain_workqueue(bao_io_dispatcher_wq[dm->info.id]);
}

void bao_io_dispatcher_resume(struct bao_dm *dm)
{
	// setup the interrupt handler
	bao_intc_setup_handler(io_dispatcher_intc_handler);
	// add the work to the workqueue
	queue_work(bao_io_dispatcher_wq[dm->info.id], &io_dispatcher_work[dm->info.id].work);
}

int bao_io_dispatcher_init(struct bao_dm *dm)
{
	char name[BAO_NAME_MAX_LEN];
    snprintf(name, BAO_NAME_MAX_LEN, "bao-iodwq%u", dm->info.id);

	// Create the I/O Dispatcher workqueue with high priority
	bao_io_dispatcher_wq[dm->info.id] = alloc_workqueue(name, WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (!bao_io_dispatcher_wq[dm->info.id]) {
		return -ENOMEM;
	}

	// Assign the custom data to the work
	io_dispatcher_work[dm->info.id].dm = dm;

	// Initialize the work_struct
	INIT_WORK(&io_dispatcher_work[dm->info.id].work, io_dispatcher);

	// setup the interrupt handler
	bao_intc_setup_handler(io_dispatcher_intc_handler);

	return 0;
}

int bao_io_dispatcher_setup(void)
{
	// Do nothing
	return 0;
}

void bao_io_dispatcher_remove(void)
{
	// Do nothing
}