// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor Ioeventfd Client
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include "bao.h"
#include <linux/eventfd.h>

/**
 * Properties of a ioeventfd
 * @list: List node of the ioeventfd
 * @eventfd: Eventfd of the ioeventfd
 * @addr: Address of I/O range
 * @data: Data for matching
 * @length:	Length of I/O range
 * @wildcard: Data matching or not
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
 * Shutdown the ioeventfd
 * @dm:	The DM that the ioeventfd belongs to
 * @p: The ioeventfd to shutdown
 */
static void bao_ioeventfd_shutdown(struct bao_dm* dm, struct ioeventfd* p)
{
    lockdep_assert_held(&dm->ioeventfds_lock);

    // unregister the ioeventfd
    eventfd_ctx_put(p->eventfd);
    // remove the ioeventfd from the list
    list_del(&p->list);
    // free the ioeventfd
    kfree(p);
}

/**
 * Check if the configuration of ioeventfd is valid
 * @config: The configuration of ioeventfd
 * @return: bool
 */
static bool bao_ioeventfd_config_valid(struct bao_ioeventfd* config)
{
    // check if the configuration is valid
    if (!config) {
        return false;
    }

    // check for overflow
    if (config->addr + config->len < config->addr) {
        return false;
    }

    // vhost supports 1, 2, 4 and 8 bytes access
    if (!(config->len == 1 || config->len == 2 || config->len == 4 || config->len == 8)) {
        return false;
    }

    return true;
}

/**
 * Check if the ioeventfd is conflict with other ioeventfds
 * @dm: The DM that the ioeventfd belongs to
 * @ioeventfd: The ioeventfd to check
 * @return: bool
 */
static bool bao_ioeventfd_is_conflict(struct bao_dm* dm, struct ioeventfd* ioeventfd)
{
    struct ioeventfd* p;

    lockdep_assert_held(&dm->ioeventfds_lock);

    // either one is wildcard, the data matching will be skipped
    list_for_each_entry(p, &dm->ioeventfds, list) if (p->eventfd == ioeventfd->eventfd &&
        p->addr == ioeventfd->addr &&
        (p->wildcard || ioeventfd->wildcard || p->data == ioeventfd->data)) return true;

    return false;
}

/**
 * Return the matched ioeventfd
 * @dm: The DM to check
 * @addr: The address of I/O request
 * @data: The data of I/O request
 * @len: The length of I/O request
 * @return: The matched ioeventfd or NULL
 */
static struct ioeventfd* bao_ioeventfd_match(struct bao_dm* dm, u64 addr, u64 data, int len)
{
    struct ioeventfd* p = NULL;

    lockdep_assert_held(&dm->ioeventfds_lock);

    list_for_each_entry(p, &dm->ioeventfds, list)
    {
        if (p->addr == addr && p->length >= len && (p->wildcard || p->data == data)) {
            return p;
        }
    }

    return NULL;
}

/**
 * Assign an eventfd to a DM and create a ioeventfd associated with the eventfd
 * @dm:	The DM to assign the eventfd to
 * @config:	The configuration of the eventfd
 */
static int bao_ioeventfd_assign(struct bao_dm* dm, struct bao_ioeventfd* config)
{
    struct eventfd_ctx* eventfd;
    struct ioeventfd* new;
    int rc = 0;

    // check if the configuration is valid
    if (!bao_ioeventfd_config_valid(config)) {
        return -EINVAL;
    }

    // get the eventfd from the file descriptor
    eventfd = eventfd_ctx_fdget(config->fd);
    if (IS_ERR(eventfd)) {
        return PTR_ERR(eventfd);
    }

    // allocate a new ioeventfd object
    new = kzalloc(sizeof(*new), GFP_KERNEL);
    if (!new) {
        rc = -ENOMEM;
        goto err;
    }

    // initialize the ioeventfd
    INIT_LIST_HEAD(&new->list);
    new->addr = config->addr;
    new->length = config->len;
    new->eventfd = eventfd;

    /*
     * BAO_IOEVENTFD_FLAG_DATAMATCH flag is set in virtio 1.0 support, the
     * writing of notification register of each virtqueue may trigger the
     * notification. There is no data matching requirement.
     */
    if (config->flags & BAO_IOEVENTFD_FLAG_DATAMATCH) {
        new->data = config->data;
    } else {
        new->wildcard = true;
    }

    mutex_lock(&dm->ioeventfds_lock);

    // check if the ioeventfd is conflict with other ioeventfds
    if (bao_ioeventfd_is_conflict(dm, new)) {
        rc = -EEXIST;
        goto err_unlock;
    }

    // register the I/O range monitor into the Ioeventfd client
    rc = bao_io_client_range_add(dm->ioeventfd_client, new->addr, new->addr + new->length - 1);
    if (rc < 0) {
        goto err_unlock;
    }

    // add the ioeventfd to the list
    list_add_tail(&new->list, &dm->ioeventfds);
    mutex_unlock(&dm->ioeventfds_lock);

    return rc;

err_unlock:
    mutex_unlock(&dm->ioeventfds_lock);
    kfree(new);
err:
    eventfd_ctx_put(eventfd);
    return rc;
}

/**
 * Deassign an eventfd from a DM and destroy the ioeventfd associated with
 * the eventfd.
 * @dm:	The DM to deassign the eventfd from
 * @config:	The configuration of the eventfd
 */
static int bao_ioeventfd_deassign(struct bao_dm* dm, struct bao_ioeventfd* config)
{
    struct ioeventfd* p;
    struct eventfd_ctx* eventfd;

    // get the eventfd from the file descriptor
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
        // delete the I/O range monitor from the Ioeventfd client
        bao_io_client_range_del(dm->ioeventfd_client, p->addr, p->addr + p->length - 1);
        // shutdown the ioeventfd
        bao_ioeventfd_shutdown(dm, p);
        break;
    }
    mutex_unlock(&dm->ioeventfds_lock);

    // unregister the eventfd
    eventfd_ctx_put(eventfd);
    return 0;
}

/**
 * Handle the Ioeventfd client I/O request
 * This function is called by the I/O client kernel thread
 * (bao_io_client_kernel_thread)
 * @client: The Ioeventfd client that the I/O request belongs to
 * @req: The I/O request to be handled
 */
static int bao_ioeventfd_handler(struct bao_io_client* client, struct bao_virtio_request* req)
{
    struct ioeventfd* p;

    /*
     * I/O requests are dispatched by range check only, so a
     * bao_io_client need process both READ and WRITE accesses
     * of same range. READ accesses are safe to be ignored here
     * because virtio MMIO drivers only write into the notify
     * register (`QueueNotify` field) for notification.
     * In fact, the read request won't exist since
     * the `QueueNotify` field is WRITE ONLY from the driver
     * and read only from the device.
     */
    if (req->op == BAO_IO_READ) {
        req->value = 0;
        return 0;
    }

    mutex_lock(&client->dm->ioeventfds_lock);

    // find the matched ioeventfd
    p = bao_ioeventfd_match(client->dm, req->addr, req->value, req->access_width);

    // if matched, signal the eventfd
    if (p) {
        eventfd_signal(p->eventfd, 1);
    }
    mutex_unlock(&client->dm->ioeventfds_lock);

    return 0;
}

int bao_ioeventfd_client_config(struct bao_dm* dm, struct bao_ioeventfd* config)
{
    // check if the DM and configuration are valid
    if (WARN_ON(!dm || !config)) {
        return -EINVAL;
    }

    // deassign the eventfd from the DM
    if (config->flags & BAO_IOEVENTFD_FLAG_DEASSIGN) {
        bao_ioeventfd_deassign(dm, config);
    }

    // assign the eventfd to the DM
    return bao_ioeventfd_assign(dm, config);
}

int bao_ioeventfd_client_init(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];

    mutex_init(&dm->ioeventfds_lock);
    INIT_LIST_HEAD(&dm->ioeventfds);

    // create a new name for the Ioeventfd client based on type and DM ID
    snprintf(name, sizeof(name), "bao-ioevfdc%u", dm->info.id);

    // create a new I/O client (Ioeventfd client)
    dm->ioeventfd_client = bao_io_client_create(dm, bao_ioeventfd_handler, NULL, false, name);
    if (!dm->ioeventfd_client) {
        return -EINVAL;
    }

    return 0;
}

void bao_ioeventfd_client_destroy(struct bao_dm* dm)
{
    struct ioeventfd *p, *next;

    mutex_lock(&dm->ioeventfds_lock);
    // shutdown all the ioeventfds
    list_for_each_entry_safe(p, next, &dm->ioeventfds, list) bao_ioeventfd_shutdown(dm, p);
    mutex_unlock(&dm->ioeventfds_lock);
}
