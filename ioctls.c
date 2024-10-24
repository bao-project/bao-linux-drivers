// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor IOCTLs Handler for the I/O Dispatcher kernel module
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include "bao.h"
#include "hypercall.h"
#include <linux/anon_inodes.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

long bao_io_dispatcher_driver_ioctl(struct file* filp, unsigned int cmd, unsigned long ioctl_param)
{
    int rc = -EINVAL;
    struct bao_dm_info* info;

    switch (cmd) {
        case BAO_IOCTL_DM_GET_INFO:
            info = memdup_user((void __user*)ioctl_param, sizeof(struct bao_dm_info));
            if (IS_ERR(info)) {
                pr_err("%s: memdup_user failed\n", __FUNCTION__);
                return PTR_ERR(info);
            }
            rc = bao_dm_get_info(info);
            if (!rc) {
                pr_err("%s: DM with id [%d] not found\n", __FUNCTION__, info->id);
                kfree(info);
                return -EINVAL;
            }
            if (copy_to_user((void __user*)ioctl_param, info, sizeof(struct bao_dm_info))) {
                pr_err("%s: copy_to_user failed\n", __FUNCTION__);
                kfree(info);
                return -EFAULT;
            }
            break;
        default:
            pr_err("%s: unknown ioctl cmd [%d]\n", __FUNCTION__, cmd);
            return -ENOTTY;
    }
    return rc;
}

long bao_dm_ioctl(struct file* filp, unsigned int cmd, unsigned long ioctl_param)
{
    struct bao_virtio_request* req;
    struct remio_hypercall_ret hret;
    int rc = -EINVAL;

    // get the backend DM pointer from the file pointer private data
    struct bao_dm* dm = filp->private_data;

    switch (cmd) {
        case BAO_IOCTL_IO_CLIENT_ATTACH:
            req = memdup_user((void __user*)ioctl_param, sizeof(struct bao_virtio_request));
            if (IS_ERR(req)) {
                pr_err("%s: memdup_user failed\n", __FUNCTION__);
                return PTR_ERR(req);
            }
            if (!dm->control_client) {
                pr_err("%s: control client does not exist\n", __FUNCTION__);
                return -EINVAL;
            }
            rc = bao_io_client_attach(dm->control_client);
            if (rc == 0) {
                rc = bao_io_client_request(dm->control_client, req);
                if (copy_to_user((void __user*)ioctl_param, req,
                        sizeof(struct bao_virtio_request))) {
                    pr_err("%s: copy_to_user failed\n", __FUNCTION__);
                    return -EFAULT;
                }
            }
            kfree(req);
            break;
        case BAO_IOCTL_IO_REQUEST_COMPLETE:
            req = memdup_user((void __user*)ioctl_param, sizeof(struct bao_virtio_request));
            if (IS_ERR(req)) {
                pr_err("%s: memdup_user failed\n", __FUNCTION__);
                return PTR_ERR(req);
            }
            hret = bao_hypercall_remio(req);
            rc = hret.hyp_ret | hret.remio_hyp_ret;
            break;
        case BAO_IOCTL_IOEVENTFD:
            struct bao_ioeventfd ioeventfd;
            if (copy_from_user(&ioeventfd, (void __user*)ioctl_param,
                    sizeof(struct bao_ioeventfd))) {
                pr_err("%s: copy_from_user failed\n", __FUNCTION__);
                return -EFAULT;
            }
            rc = bao_ioeventfd_client_config(dm, &ioeventfd);
            break;
        case BAO_IOCTL_IRQFD:
            struct bao_irqfd irqfd;
            if (copy_from_user(&irqfd, (void __user*)ioctl_param, sizeof(struct bao_irqfd))) {
                pr_err("%s: copy_from_user failed\n", __FUNCTION__);
                return -EFAULT;
            }
            rc = bao_irqfd_server_config(dm, &irqfd);
            break;
        default:
            pr_err("%s: unknown ioctl cmd [%d]\n", __FUNCTION__, cmd);
            rc = -ENOTTY;
            break;
    }
    return rc;
}
