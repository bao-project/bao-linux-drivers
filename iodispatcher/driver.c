// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher Kernel Driver
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include "bao.h"
#include <asm/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define DEV_NAME "bao-io-dispatcher"

static dev_t bao_iodispatcher_devt;
struct class* bao_iodispatcher_cl;

/**
 * Bao I/O Dispatcher driver structure
 * @cdev: The character device
 * @dev: The device
 */
struct bao_iodispatcher_drv {
    struct cdev cdev;
    struct device* dev;
};

/**
 * Open the I/O Dispatcher device
 * @inode: The inode of the I/O Dispatcher
 * @filp: The file pointer of the I/O Dispatcher
 */
static int bao_io_dispatcher_driver_open_fops(struct inode* inode, struct file* filp)
{
    struct bao_iodispatcher_drv* bao_iodispatcher_drv =
        container_of(inode->i_cdev, struct bao_iodispatcher_drv, cdev);
    filp->private_data = bao_iodispatcher_drv;

    kobject_get(&bao_iodispatcher_drv->dev->kobj);

    return 0;
}

/**
 * Release the I/O Dispatcher device
 * @inode: The inode of the I/O Dispatcher
 * @filp: The file pointer of the I/O Dispatcher
 */
static int bao_io_dispatcher_driver_release_fops(struct inode* inode, struct file* filp)
{
    struct bao_iodispatcher_drv* bao_iodispatcher_drv =
        container_of(inode->i_cdev, struct bao_iodispatcher_drv, cdev);
    filp->private_data = NULL;

    kobject_put(&bao_iodispatcher_drv->dev->kobj);

    return 0;
}

static long bao_io_dispatcher_driver_ioctl_fops(struct file* filp, unsigned int cmd,
    unsigned long ioctl_param)
{
    return bao_io_dispatcher_driver_ioctl(filp, cmd, ioctl_param);
}

static struct file_operations bao_io_dispatcher_driver_fops = {
    .owner = THIS_MODULE,
    .open = bao_io_dispatcher_driver_open_fops,
    .release = bao_io_dispatcher_driver_release_fops,
    .unlocked_ioctl = bao_io_dispatcher_driver_ioctl_fops,
};

/**
 * Register the driver with the kernel
 * @pdev: Platform device pointer
 */
static int bao_io_dispatcher_driver_register(struct platform_device* pdev)
{
    int ret, irq;
    struct module* owner = THIS_MODULE;
    struct resource* r;
    dev_t devt;
    resource_size_t reg_size;
    struct bao_iodispatcher_drv* bao_io_dispatcher_drv;
    struct bao_dm* dm;
    struct bao_dm_info dm_info;

    // setup the I/O Dispatcher system
    ret = bao_io_dispatcher_setup();
    if (ret) {
        dev_err(&pdev->dev, "setup I/O Dispatcher failed!\n");
        return ret;
    }

    // allocate memory for the Bao I/O Dispatcher structure
    bao_io_dispatcher_drv =
        devm_kzalloc(&pdev->dev, sizeof(struct bao_iodispatcher_drv), GFP_KERNEL);

    if (bao_io_dispatcher_drv == NULL) {
        ret = -ENOMEM;
        goto err_io_dispatcher;
    }

    for (int i = 0; i < BAO_IO_MAX_DMS; i++) {
        // get the memory region from the device tree
        r = platform_get_resource(pdev, IORESOURCE_MEM, i);
        if (!r) {
            break;
        }

        // get the interrupt number from the device tree
        irq = platform_get_irq(pdev, i);
        if (irq < 0) {
            dev_err(&pdev->dev, "Failed to read interrupt number at index %d\n", i);
            ret = irq;
            goto err_io_dispatcher;
        }

        // get the memory region size
        reg_size = resource_size(r);

        // set the device model information
        dm_info.id = i;
        dm_info.shmem_addr = (unsigned long)r->start;
        dm_info.shmem_size = (unsigned long)reg_size;
        dm_info.irq = irq;
        dm_info.fd = 0;

        // create the device model
        dm = bao_dm_create(&dm_info);
        if (dm == NULL) {
            dev_err(&pdev->dev, "failed to create Bao I/O Dispatcher device model %d\n", i);
            ret = -ENOMEM;
            goto err_io_dispatcher;
        }

        // register the interrupt
        ret = bao_intc_register(dm);
        if (ret) {
            dev_err(&pdev->dev, "failed to register interrupt %d\n", irq);
            goto err_unregister_dms;
        }
    }

    cdev_init(&bao_io_dispatcher_drv->cdev, &bao_io_dispatcher_driver_fops);
    bao_io_dispatcher_drv->cdev.owner = owner;

    devt = MKDEV(MAJOR(bao_iodispatcher_devt), 0);
    ret = cdev_add(&bao_io_dispatcher_drv->cdev, devt, 1);
    if (ret) {
        goto err_unregister_irqs;
    }

    bao_io_dispatcher_drv->dev =
        device_create(bao_iodispatcher_cl, &pdev->dev, devt, bao_io_dispatcher_drv, DEV_NAME);
    if (IS_ERR(bao_io_dispatcher_drv->dev)) {
        ret = PTR_ERR(bao_io_dispatcher_drv->dev);
        goto err_cdev;
    }
    dev_set_drvdata(bao_io_dispatcher_drv->dev, bao_io_dispatcher_drv);

    return 0;

err_cdev:
    cdev_del(&bao_io_dispatcher_drv->cdev);
err_unregister_irqs: {
    list_for_each_entry(dm, &bao_dm_list, list)
    {
        bao_intc_unregister(dm);
    }
}
err_unregister_dms: {
    list_for_each_entry(dm, &bao_dm_list, list)
    {
        bao_dm_destroy(dm);
    }
}
err_io_dispatcher:
    bao_io_dispatcher_remove();

    dev_err(&pdev->dev, "failed initialization\n");
    return ret;
}

/**
 * Unregister the driver from the kernel
 * @pdev: Platform device pointer
 */
static int bao_io_dispatcher_driver_unregister(struct platform_device* pdev)
{
    struct bao_dm* dm;

    // remove the I/O Dispatcher system
    bao_io_dispatcher_remove();

    list_for_each_entry(dm, &bao_dm_list, list)
    {
        // destroy the device model
        bao_dm_destroy(dm);
        // unregister the interrupt
        bao_intc_unregister(dm);
    }

    return 0;
}

static const struct of_device_id bao_io_dispatcher_driver_dt_ids[] = {
    { .compatible = "bao,io-dispatcher" }, { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bao_io_dispatcher_driver_dt_ids);

static struct platform_driver bao_io_dispatcher_driver = {
    .probe = bao_io_dispatcher_driver_register,
    .remove = bao_io_dispatcher_driver_unregister,
    .driver =
        {
            .name = "bao-io-dispatcher",
            .of_match_table = of_match_ptr(bao_io_dispatcher_driver_dt_ids),
            .owner = THIS_MODULE,
        },
};

static int __init bao_io_dispatcher_driver_init(void)
{
    int ret;

    if ((bao_iodispatcher_cl = class_create(THIS_MODULE, DEV_NAME)) == NULL) {
        ret = -1;
        pr_err("unable to class_create " DEV_NAME " device\n");
        return ret;
    }

    ret = alloc_chrdev_region(&bao_iodispatcher_devt, 0, BAO_IO_MAX_DMS, DEV_NAME);
    if (ret < 0) {
        pr_err("unable to alloc_chrdev_region " DEV_NAME " device\n");
        return ret;
    }

    return platform_driver_register(&bao_io_dispatcher_driver);
}

static void __exit bao_io_dispatcher_driver_exit(void)
{
    platform_driver_unregister(&bao_io_dispatcher_driver);
    unregister_chrdev(bao_iodispatcher_devt, DEV_NAME);
    class_destroy(bao_iodispatcher_cl);
}

module_init(bao_io_dispatcher_driver_init);
module_exit(bao_io_dispatcher_driver_exit);

MODULE_AUTHOR("João Peixoto <joaopeixotooficial@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bao Hypervisor I/O Dispatcher Kernel Driver");
