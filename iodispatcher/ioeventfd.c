// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor Ioeventfd Client
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixoto@osyx.tech>
 *	José Martins <jose@osyx.tech>
 *	David Cerdeira <davidmcerdeira@osyx.tech>
 */

#include <linux/eventfd.h>
#include "bao.h"

/**
 * struct ioeventfd - Properties of an I/O eventfd
 * @list: List node linking this ioeventfd
 * @eventfd: Associated eventfd context
 * @addr: Start address of the I/O range
 * @data: Data used for matching (if not wildcard)
 * @length: Length of the I/O range
 * @wildcard: True if data matching is not required
 *
 * Represents an I/O eventfd registered for a Bao device model.
 */
struct ioeventfd {
    struct list_head list;
    struct eventfd_ctx* eventfd;
    u64 addr;
    u64 data;
    int length;
    bool wildcard;
};

/**
 * bao_ioeventfd_shutdown - Release and remove an ioeventfd
 * @dm: Bao device model owning the ioeventfd
 * @p: Ioeventfd to shut down
 */
static void bao_ioeventfd_shutdown(struct bao_dm* dm, struct ioeventfd* p)
{
    lockdep_assert_held(&dm->ioeventfds_lock);

    if (WARN_ON_ONCE(!p)) {
        return;
    }

    eventfd_ctx_put(p->eventfd);
    list_del(&p->list);
    kfree(p);
}

/**
 * bao_ioeventfd_config_valid - Validate ioeventfd configuration
 * @config: Ioeventfd configuration
 *
 * Return: True if config is non-NULL, address+length does not wrap,
 * and length is 1, 2, 4, or 8 bytes.
 */
static bool bao_ioeventfd_config_valid(struct bao_ioeventfd* config)
{
    if (WARN_ON_ONCE(!config)) {
        return false;
    }

    if (config->addr + config->len < config->addr) {
        return false;
    }

    if (!(config->len == 1 || config->len == 2 || config->len == 4 || config->len == 8)) {
        return false;
    }

    return true;
}

/**
 * bao_ioeventfd_is_conflict - Check if an ioeventfd conflicts with existing ones
 * @dm: Bao device model
 * @ioeventfd: Ioeventfd to check
 *
 * Return: True if an existing ioeventfd matches address, eventfd,
 * and optionally data.
 */
static bool bao_ioeventfd_is_conflict(struct bao_dm* dm, struct ioeventfd* ioeventfd)
{
    struct ioeventfd* p;

    lockdep_assert_held(&dm->ioeventfds_lock);

    if (WARN_ON_ONCE(!dm || !ioeventfd)) {
        return true;
    }

    list_for_each_entry(p, &dm->ioeventfds, list)
    {
        if (p->eventfd == ioeventfd->eventfd && p->addr == ioeventfd->addr &&
            (p->wildcard || ioeventfd->wildcard || p->data == ioeventfd->data)) {
            return true;
        }
    }

    return false;
}

/**
 * bao_ioeventfd_match - Find ioeventfd matching an I/O request
 * @dm: Bao device model
 * @addr: I/O request address
 * @data: I/O request data
 * @len: I/O request length
 *
 * Return: The matching ioeventfd, NULL if none matches.
 */
static struct ioeventfd* bao_ioeventfd_match(struct bao_dm* dm, u64 addr, u64 data, int len)
{
    struct ioeventfd* p;

    lockdep_assert_held(&dm->ioeventfds_lock);

    if (WARN_ON_ONCE(!dm)) {
        return NULL;
    }

    list_for_each_entry(p, &dm->ioeventfds, list)
    {
        if (p->addr == addr && p->length >= len && (p->wildcard || p->data == data)) {
            return p;
        }
    }

    return NULL;
}

/**
 * bao_ioeventfd_assign - Assign and create an eventfd for a DM
 * @dm: Bao device model to assign the eventfd to
 * @config: Configuration of the eventfd to create
 *
 * Creates a new ioeventfd associated with the given eventfd and
 * adds it to the Bao DM. Validates the configuration, checks for
 * conflicts with existing ioeventfds, and registers the corresponding
 * I/O client address range. Supports optional data matching for
 * virtio 1.0 notifications; if not set, wildcard matching is used.
 *
 * Return: 0 on success, a negative error code on failure
 */
static int bao_ioeventfd_assign(struct bao_dm* dm, struct bao_ioeventfd* config)
{
    struct eventfd_ctx* eventfd;
    struct ioeventfd* new;
    int rc = 0;

    if (WARN_ON_ONCE(!dm || !config)) {
        return -EINVAL;
    }

    if (!bao_ioeventfd_config_valid(config)) {
        return -EINVAL;
    }

    eventfd = eventfd_ctx_fdget(config->fd);
    if (IS_ERR(eventfd)) {
        return PTR_ERR(eventfd);
    }

    new = kzalloc(sizeof(*new), GFP_KERNEL);
    if (!new) {
        rc = -ENOMEM;
        goto err_put_eventfd;
    }

    INIT_LIST_HEAD(&new->list);
    new->addr = config->addr;
    new->length = config->len;
    new->eventfd = eventfd;
    new->wildcard = !(config->flags & BAO_IOEVENTFD_FLAG_DATAMATCH);
    if (!new->wildcard) {
        new->data = config->data;
    }

    mutex_lock(&dm->ioeventfds_lock);

    if (bao_ioeventfd_is_conflict(dm, new)) {
        rc = -EEXIST;
        goto err_unlock_free;
    }

    rc = bao_io_client_range_add(dm->ioeventfd_client, new->addr, new->addr + new->length - 1);
    if (rc < 0) {
        goto err_unlock_free;
    }

    list_add_tail(&new->list, &dm->ioeventfds);
    mutex_unlock(&dm->ioeventfds_lock);

    return 0;

err_unlock_free:
    mutex_unlock(&dm->ioeventfds_lock);
    kfree(new);
err_put_eventfd:
    eventfd_ctx_put(eventfd);
    return rc;
}

/**
 * bao_ioeventfd_deassign - Deassign and destroy an eventfd from a DM
 * @dm: Bao device model to deassign the eventfd from
 * @config: Configuration of the eventfd to remove
 *
 * Return: 0 on success, a negative error code on failure
 */
static int bao_ioeventfd_deassign(struct bao_dm* dm, struct bao_ioeventfd* config)
{
    struct ioeventfd* p;
    struct eventfd_ctx* eventfd;

    if (WARN_ON_ONCE(!dm || !config)) {
        return -EINVAL;
    }

    eventfd = eventfd_ctx_fdget(config->fd);
    if (IS_ERR(eventfd)) {
        return PTR_ERR(eventfd);
    }

    mutex_lock(&dm->ioeventfds_lock);

    list_for_each_entry(p, &dm->ioeventfds, list)
    {
        if (p->eventfd != eventfd) {
            continue;
        }

        bao_io_client_range_del(dm->ioeventfd_client, p->addr, p->addr + p->length - 1);

        bao_ioeventfd_shutdown(dm, p);
        break;
    }

    mutex_unlock(&dm->ioeventfds_lock);
    eventfd_ctx_put(eventfd);

    return 0;
}

/**
 * bao_ioeventfd_handler - Handle an Ioeventfd client I/O request
 * @client: Ioeventfd client associated with the request
 * @req: I/O request to process
 *
 * Processes I/O requests from the Bao I/O client kernel thread
 * (bao_io_client_kernel_thread). For READ operations, the value is
 * ignored and set to 0 since virtio MMIO drivers only write to the
 * `QueueNotify` field. WRITE operations are checked against the
 * registered ioeventfds, and the corresponding eventfd is signaled
 * if a match is found.
 *
 * Return: 0 on success, a negative error code on failure
 */
static int bao_ioeventfd_handler(struct bao_io_client* client, struct bao_virtio_request* req)
{
    struct ioeventfd* p;

    if (WARN_ON_ONCE(!client || !req)) {
        return -EINVAL;
    }

    if (req->op == BAO_IO_READ) {
        req->value = 0;
        return 0;
    }

    mutex_lock(&client->dm->ioeventfds_lock);

    p = bao_ioeventfd_match(client->dm, req->addr, req->value, req->access_width);
    if (p) {
        eventfd_signal(p->eventfd);
    }

    mutex_unlock(&client->dm->ioeventfds_lock);

    return 0;
}

int bao_ioeventfd_client_config(struct bao_dm* dm, struct bao_ioeventfd* config)
{
    if (WARN_ON_ONCE(!dm || !config)) {
        return -EINVAL;
    }

    if (config->flags & BAO_IOEVENTFD_FLAG_DEASSIGN) {
        bao_ioeventfd_deassign(dm, config);
    }

    return bao_ioeventfd_assign(dm, config);
}

int bao_ioeventfd_client_init(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];

    if (WARN_ON_ONCE(!dm)) {
        return -EINVAL;
    }

    mutex_init(&dm->ioeventfds_lock);
    INIT_LIST_HEAD(&dm->ioeventfds);

    snprintf(name, sizeof(name), "bao-ioevfdc%u", dm->info.id);

    dm->ioeventfd_client = bao_io_client_create(dm, bao_ioeventfd_handler, NULL, false, name);
    if (!dm->ioeventfd_client) {
        return -ENOMEM;
    }

    return 0;
}

void bao_ioeventfd_client_destroy(struct bao_dm* dm)
{
    struct ioeventfd* p;
    struct ioeventfd* next;

    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    mutex_lock(&dm->ioeventfds_lock);
    list_for_each_entry_safe(p, next, &dm->ioeventfds, list) bao_ioeventfd_shutdown(dm, p);
    mutex_unlock(&dm->ioeventfds_lock);
}
