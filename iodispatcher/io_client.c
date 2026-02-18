// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Client
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
#include <linux/kthread.h>

/**
 * struct bao_io_request - Bao I/O request structure
 * @list: List node linking all requests
 * @virtio_request: The VirtIO request payload
 *
 * Represents a single I/O request for a Bao I/O client.
 */
struct bao_io_request {
    struct list_head list;
    struct bao_virtio_request virtio_request;
};

/**
 * bao_io_client_has_pending_requests - Check if an I/O client has pending requests
 * @client: The bao_io_client to check
 *
 * Return: True if has pending I/O requests, false otherwise.
 */
static inline bool bao_io_client_has_pending_requests(struct bao_io_client* client)
{
    if (WARN_ON_ONCE(!client)) {
        return false;
    }

    return !list_empty(&client->virtio_requests);
}

/**
 * bao_io_client_is_destroying - Check if an I/O client is being destroyed
 * @client: The bao_io_client to check
 *
 * Return: True if the client is being destroyed, false otherwise.
 */
static inline bool bao_io_client_is_destroying(struct bao_io_client* client)
{
    if (WARN_ON_ONCE(!client)) {
        return true;
    }

    return test_bit(BAO_IO_CLIENT_DESTROYING, &client->flags);
}

bool bao_io_client_push_request(struct bao_io_client* client, struct bao_virtio_request* req)
{
    struct bao_io_request* io_req;

    if (WARN_ON_ONCE(!client || !req)) {
        return false;
    }

    io_req = kzalloc(sizeof(*io_req), GFP_KERNEL);
    if (!io_req) {
        return false;
    }

    io_req->virtio_request = *req;

    mutex_lock(&client->virtio_requests_lock);
    list_add_tail(&io_req->list, &client->virtio_requests);
    mutex_unlock(&client->virtio_requests_lock);

    return true;
}

bool bao_io_client_pop_request(struct bao_io_client* client, struct bao_virtio_request* ret)
{
    struct bao_io_request* req;

    if (WARN_ON_ONCE(!client || !ret)) {
        return false;
    }

    mutex_lock(&client->virtio_requests_lock);

    req = list_first_entry_or_null(&client->virtio_requests, struct bao_io_request, list);
    if (!req) {
        mutex_unlock(&client->virtio_requests_lock);
        return false;
    }

    list_del(&req->list);
    *ret = req->virtio_request;

    mutex_unlock(&client->virtio_requests_lock);

    kfree(req);

    return true;
}

/**
 * bao_io_client_destroy - Destroy an I/O client
 * @client: The bao_io_client to destroy
 */
static void bao_io_client_destroy(struct bao_io_client* client)
{
    struct bao_io_client* range;
    struct bao_io_client* next;
    struct bao_dm* dm;

    if (WARN_ON_ONCE(!client)) {
        return;
    }

    dm = client->dm;

    bao_io_dispatcher_pause(dm);

    set_bit(BAO_IO_CLIENT_DESTROYING, &client->flags);

    if (client->is_control) {
        wake_up_interruptible(&client->wq);
    } else {
        bao_ioeventfd_client_destroy(dm);
        if (client->thread) {
            kthread_stop(client->thread);
        }
    }

    down_write(&client->range_lock);
    list_for_each_entry_safe(range, next, &client->range_list, list)
    {
        list_del(&range->list);
        kfree(range);
    }
    up_write(&client->range_lock);

    down_write(&dm->io_clients_lock);
    if (client->is_control) {
        dm->control_client = NULL;
    } else {
        dm->ioeventfd_client = NULL;
    }

    list_del(&client->list);
    up_write(&dm->io_clients_lock);

    bao_io_dispatcher_resume(dm);

    kfree(client);
}

void bao_io_clients_destroy(struct bao_dm* dm)
{
    struct bao_io_client *client, *next;

    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    list_for_each_entry_safe(client, next, &dm->io_clients, list)
    {
        bao_io_client_destroy(client);
    }
}

int bao_io_client_attach(struct bao_io_client* client)
{
    if (WARN_ON_ONCE(!client)) {
        return -EINVAL;
    }

    if (client->is_control) {
        wait_event_interruptible(client->wq,
            bao_io_client_has_pending_requests(client) || bao_io_client_is_destroying(client));
        if (bao_io_client_is_destroying(client)) {
            return -EPERM;
        }
    } else {
        wait_event_interruptible(client->wq,
            bao_io_client_has_pending_requests(client) || bao_io_client_is_destroying(client) ||
                kthread_should_stop());
        if (bao_io_client_is_destroying(client) || kthread_should_stop()) {
            if (kthread_should_stop()) {
                bao_io_client_destroy(client);
            }
            return -EPERM;
        }
    }

    return 0;
}

/**
 * bao_io_client_kernel_thread - Thread for processing a kernel I/O client
 * @data: Pointer to the bao_io_client structure
 *
 * Return: 0 on completion
 */
static int bao_io_client_kernel_thread(void* data)
{
    struct bao_io_client* client = data;
    struct bao_virtio_request req;
    struct bao_remio_hypercall_ctx ctx;
    bool stop = false;
    int ret;

    if (WARN_ON_ONCE(!client)) {
        return -EINVAL;
    }

    while (!stop && !kthread_should_stop()) {
        ret = bao_io_client_attach(client);
        if (ret < 0) {
            stop = true;
            break;
        }

        while (bao_io_client_has_pending_requests(client) && !stop) {
            if (!bao_io_client_pop_request(client, &req)) {
                pr_err("%s: failed to pop I/O request\n", __func__);
                stop = true;
                break;
            }

            ret = client->handler(client, &req);
            if (ret < 0) {
                pr_warn("%s: client handler returned %d\n", __func__, ret);
                break;
            }

            ctx.dm_id = req.dm_id;
            ctx.op = req.op;
            ctx.addr = req.addr;
            ctx.value = req.value;
            ctx.access_width = req.access_width;
            ctx.request_id = req.request_id;

            if (bao_remio_hypercall(&ctx)) {
                stop = true;
                break;
            }
        }
    }

    return 0;
}

struct bao_io_client* bao_io_client_create(struct bao_dm* dm, bao_io_client_handler_t handler,
    void* data, bool is_control, const char* name)
{
    struct bao_io_client* client;

    if (WARN_ON_ONCE(!dm || !name)) {
        return NULL;
    }

    if (!handler && !is_control) {
        return NULL;
    }

    client = kzalloc(sizeof(*client), GFP_KERNEL);
    if (!client) {
        return NULL;
    }

    client->handler = handler;
    client->dm = dm;
    client->priv = data;
    client->is_control = is_control;
    if (name) {
        strscpy(client->name, name, sizeof(client->name));
    }

    INIT_LIST_HEAD(&client->virtio_requests);
    mutex_init(&client->virtio_requests_lock);
    init_rwsem(&client->range_lock);
    INIT_LIST_HEAD(&client->range_list);
    init_waitqueue_head(&client->wq);

    if (client->handler) {
        client->thread =
            kthread_run(bao_io_client_kernel_thread, client, "%s-kthread", client->name);
        if (IS_ERR(client->thread)) {
            kfree(client);
            return NULL;
        }
    }

    down_write(&dm->io_clients_lock);
    if (is_control) {
        dm->control_client = client;
    } else {
        dm->ioeventfd_client = client;
    }

    list_add(&client->list, &dm->io_clients);
    up_write(&dm->io_clients_lock);

    return client;
}

int bao_io_client_request(struct bao_io_client* client, struct bao_virtio_request* req)
{
    if (WARN_ON_ONCE(!client)) {
        return -EINVAL;
    }

    if (!bao_io_client_pop_request(client, req)) {
        return -EFAULT;
    }

    return 0;
}

int bao_io_client_range_add(struct bao_io_client* client, u64 start, u64 end)
{
    struct bao_io_range* range;

    if (WARN_ON_ONCE(!client)) {
        return -EINVAL;
    }

    if (end < start) {
        return -EINVAL;
    }

    range = kzalloc(sizeof(*range), GFP_KERNEL);
    if (!range) {
        return -ENOMEM;
    }

    range->start = start;
    range->end = end;

    down_write(&client->range_lock);
    list_add(&range->list, &client->range_list);
    up_write(&client->range_lock);

    return 0;
}

void bao_io_client_range_del(struct bao_io_client* client, u64 start, u64 end)
{
    struct bao_io_range* range;
    struct bao_io_range* tmp;

    if (WARN_ON_ONCE(!client)) {
        return;
    }

    down_write(&client->range_lock);
    list_for_each_entry_safe(range, tmp, &client->range_list, list)
    {
        if (range->start == start && range->end == end) {
            list_del(&range->list);
            kfree(range);
            break;
        }
    }
    up_write(&client->range_lock);
}

/**
 * bao_io_request_in_range - Check if the I/O request is in the range
 * @range: The I/O request range
 * @req: The I/O request to be checked
 *
 * Return: True if the I/O request is in the range, false otherwise
 */
static bool bao_io_request_in_range(struct bao_io_range* range, struct bao_virtio_request* req)
{
    if (WARN_ON_ONCE(!range || !req)) {
        return false;
    }

    if (req->addr >= range->start && (req->addr + req->access_width - 1) <= range->end) {
        return true;
    }

    return false;
}

struct bao_io_client* bao_io_client_find(struct bao_dm* dm, struct bao_virtio_request* req)
{
    struct bao_io_client* client;
    struct bao_io_client* found = NULL;
    struct bao_io_range* range;

    if (WARN_ON_ONCE(!dm || !req)) {
        return NULL;
    }

    list_for_each_entry(client, &dm->io_clients, list)
    {
        down_read(&client->range_lock);
        list_for_each_entry(range, &client->range_list, list)
        {
            if (bao_io_request_in_range(range, req)) {
                found = client;
                break;
            }
        }
        up_read(&client->range_lock);

        if (found) {
            break;
        }
    }

    return found ? found : dm->control_client;
}
