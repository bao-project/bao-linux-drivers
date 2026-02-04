// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor Backend Device Model (DM)
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixoto@osyx.tech>
 *	José Martins <jose@osyx.tech>
 *	David Cerdeira <davidmcerdeira@osyx.tech>
 */

#include "bao.h"
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include "hypercall.h"

/*
 * List of all backend device models (DMs)
 */
LIST_HEAD(bao_dm_list);

/*
 * Lock to protect bao_dm_list
 */
DEFINE_RWLOCK(bao_dm_list_lock);

static void bao_dm_get(struct bao_dm* dm)
{
    refcount_inc(&dm->refcount);
}

static void bao_dm_put(struct bao_dm* dm)
{
    if (refcount_dec_and_test(&dm->refcount)) {
        kfree(dm);
    }
}

static int bao_dm_open(struct inode* inode, struct file* filp)
{
    return 0;
}

static int bao_dm_release(struct inode* inode, struct file* filp)
{
    struct bao_dm* dm = filp->private_data;

    if (WARN_ON_ONCE(!dm)) {
        return -ENODEV;
    }

    filp->private_data = NULL;
    bao_dm_put(dm);

    return 0;
}

static long bao_dm_ioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
    struct bao_dm* dm = filp->private_data;
    int rc;

    if (WARN_ON_ONCE(!dm)) {
        return -ENODEV;
    }

    switch (cmd) {
        case BAO_IOCTL_IO_CLIENT_ATTACH: {
            struct bao_virtio_request* req;

            req = memdup_user((void __user*)arg, sizeof(*req));
            if (IS_ERR(req)) {
                rc = PTR_ERR(req);
                break;
            }

            if (!dm->control_client) {
                rc = -ENOENT;
                goto out_free;
            }

            rc = bao_io_client_attach(dm->control_client);
            if (rc) {
                goto out_free;
            }

            rc = bao_io_client_request(dm->control_client, req);
            if (rc) {
                goto out_free;
            }

            if (copy_to_user((void __user*)arg, req, sizeof(*req))) {
                rc = -EFAULT;
                goto out_free;
            }

            rc = 0;

out_free:
            kfree(req);
            break;
        }
        case BAO_IOCTL_IO_REQUEST_COMPLETE: {
            struct bao_virtio_request* req;
            struct bao_remio_hypercall_ctx ctx;

            req = memdup_user((void __user*)arg, sizeof(*req));
            if (IS_ERR(req)) {
                rc = PTR_ERR(req);
                break;
            }

            ctx.dm_id = req->dm_id;
            ctx.addr = req->addr;
            ctx.op = req->op;
            ctx.value = req->value;
            ctx.access_width = req->access_width;
            ctx.request_id = req->request_id;

            rc = bao_remio_hypercall(&ctx);
            kfree(req);

            break;
        }
        case BAO_IOCTL_IOEVENTFD: {
            struct bao_ioeventfd ioeventfd;

            if (copy_from_user(&ioeventfd, (void __user*)arg, sizeof(struct bao_ioeventfd))) {
                return -EFAULT;
            }

            rc = bao_ioeventfd_client_config(dm, &ioeventfd);
            break;
        }
        case BAO_IOCTL_IRQFD: {
            struct bao_irqfd irqfd;

            if (copy_from_user(&irqfd, (void __user*)arg, sizeof(struct bao_irqfd))) {
                return -EFAULT;
            }

            rc = bao_irqfd_server_config(dm, &irqfd);
            break;
        }
        default:
            rc = -ENOTTY;
            break;
    }

    return rc;
}

/**
 * bao_dm_mmap - mmap backend DM shared memory to userspace
 * @filp: File pointer for the DM device
 * @vma: Virtual memory area for mapping
 *
 * Return: 0 on success, negative errno on failure
 */
static int bao_dm_mmap(struct file* filp, struct vm_area_struct* vma)
{
    struct bao_dm* dm = filp->private_data;
    unsigned long vsize;
    unsigned long offset;
    phys_addr_t phys;

    if (WARN_ON_ONCE(!dm)) {
        return -ENODEV;
    }

    vsize = vma->vm_end - vma->vm_start;
    offset = vma->vm_pgoff << PAGE_SHIFT;

    if (!vsize || offset) {
        return -EINVAL;
    }

    if (vsize > dm->info.shmem_size) {
        return -EINVAL;
    }

    phys = dm->info.shmem_addr;
    if (!PAGE_ALIGNED(phys)) {
        return -EINVAL;
    }

    if (remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT, vsize, vma->vm_page_prot)) {
        return -EFAULT;
    }

    return 0;
}

/**
 * bao_dm_llseek - Adjust file offset for backend DM device
 * @file: File pointer for the DM device
 * @offset: Offset to seek
 * @whence: Reference point (SEEK_SET, SEEK_CUR, SEEK_END)
 *
 * Return: New file position on success, negative errno on failure
 */
static loff_t bao_dm_llseek(struct file* file, loff_t offset, int whence)
{
    struct bao_dm* bao = file->private_data;
    loff_t new_pos;

    if (WARN_ON_ONCE(!bao)) {
        return -ENODEV;
    }

    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = file->f_pos + offset;
            break;
        case SEEK_END:
            new_pos = bao->info.shmem_size + offset;
            break;
        default:
            return -EINVAL;
    }

    if (new_pos < 0 || new_pos > bao->info.shmem_size) {
        return -EINVAL;
    }

    file->f_pos = new_pos;
    return new_pos;
}

static const struct file_operations bao_dm_fops = {
    .owner = THIS_MODULE,
    .open = bao_dm_open,
    .release = bao_dm_release,
    .unlocked_ioctl = bao_dm_ioctl,
    .llseek = bao_dm_llseek,
    .mmap = bao_dm_mmap,
};

struct bao_dm* bao_dm_create(struct bao_dm_info* info)
{
    struct bao_dm* dm;
    struct bao_dm* tmp;
    char name[BAO_NAME_MAX_LEN];

    if (WARN_ON(!info)) {
        return NULL;
    }

    dm = kzalloc(sizeof(*dm), GFP_KERNEL);
    if (!dm) {
        return NULL;
    }

    INIT_LIST_HEAD(&dm->list);
    INIT_LIST_HEAD(&dm->io_clients);
    init_rwsem(&dm->io_clients_lock);

    refcount_set(&dm->refcount, 1);
    dm->info = *info;

    bao_io_dispatcher_init(dm);

    snprintf(name, sizeof(name), "bao-ioctlc%u", dm->info.id);
    dm->control_client = bao_io_client_create(dm, NULL, NULL, true, name);
    if (!dm->control_client) {
        pr_err("%s: failed to create control client for DM %u\n", __func__, dm->info.id);
        goto err_remove_dm;
    }

    if (bao_ioeventfd_client_init(dm)) {
        pr_err("%s: failed to initialize ioeventfd for DM %u\n", __func__, dm->info.id);
        goto err_destroy_io_clients;
    }

    if (bao_irqfd_server_init(dm)) {
        pr_err("%s: failed to initialize irqfd for DM %u\n", __func__, dm->info.id);
        goto err_destroy_io_clients;
    }

    dm->shmem_base_addr = memremap(dm->info.shmem_addr, dm->info.shmem_size, MEMREMAP_WB);
    if (!dm->shmem_base_addr) {
        pr_err("%s: failed to map memory region for DM %u\n", __func__, dm->info.id);
        goto err_destroy_irqfd;
    }

    write_lock(&bao_dm_list_lock);
    list_for_each_entry(tmp, &bao_dm_list, list)
    {
        if (tmp->info.id == info->id) {
            write_unlock(&bao_dm_list_lock);
            goto err_unmap;
        }
    }
    list_add(&dm->list, &bao_dm_list);
    write_unlock(&bao_dm_list_lock);

    return dm;

err_unmap:
    memunmap(dm->shmem_base_addr);

err_destroy_irqfd:
    bao_irqfd_server_destroy(dm);

err_destroy_io_clients:
    bao_io_clients_destroy(dm);

err_remove_dm:
    kfree(dm);

    return NULL;
}

void bao_dm_destroy(struct bao_dm* dm)
{
    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    write_lock(&bao_dm_list_lock);
    list_del_init(&dm->list);
    write_unlock(&bao_dm_list_lock);

    dm->info.id = 0;
    dm->info.shmem_addr = 0;
    dm->info.shmem_size = 0;
    dm->info.irq = 0;

    if (dm->shmem_base_addr) {
        memunmap(dm->shmem_base_addr);
    }

    if (dm->info.fd >= 0) {
        put_unused_fd(dm->info.fd);
    }

    bao_irqfd_server_destroy(dm);
    bao_io_clients_destroy(dm);
    bao_io_dispatcher_destroy(dm);

    bao_dm_put(dm);
}

/**
 * bao_dm_create_anonymous_inode - Create an anonymous inode for a backend DM
 * @dm: The backend device model (DM)
 *
 * Creates an anonymous inode that exposes the backend DM to userspace.
 * The frontend DM can use the returned file descriptor to request
 * services from the backend DM directly.
 *
 * Return: File descriptor on success, negative errno on failure
 */
static int bao_dm_create_anonymous_inode(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];
    struct file* file;
    int fd;

    if (WARN_ON_ONCE(!dm)) {
        return -EINVAL;
    }

    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) {
        return fd;
    }

    snprintf(name, sizeof(name), "bao-dm%u", dm->info.id);
    bao_dm_get(dm);
    file = anon_inode_getfile(name, &bao_dm_fops, dm, O_RDWR);
    if (IS_ERR(file)) {
        bao_dm_put(dm);
        put_unused_fd(fd);
        return PTR_ERR(file);
    }

    fd_install(fd, file);
    dm->info.fd = fd;

    return fd;
}

bool bao_dm_get_info(struct bao_dm_info* info)
{
    struct bao_dm* dm;
    bool found = false;

    if (WARN_ON_ONCE(!info)) {
        return false;
    }

    read_lock(&bao_dm_list_lock);
    list_for_each_entry(dm, &bao_dm_list, list)
    {
        if (dm->info.id == info->id) {
            bao_dm_get(dm);
            found = true;
            break;
        }
    }
    read_unlock(&bao_dm_list_lock);

    if (!found) {
        return false;
    }

    info->shmem_addr = dm->info.shmem_addr;
    info->shmem_size = dm->info.shmem_size;
    info->irq = dm->info.irq;
    info->fd = bao_dm_create_anonymous_inode(dm);

    bao_dm_put(dm);

    return true;
}
