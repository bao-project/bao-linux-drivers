// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher Kernel Driver
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <bao.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>
#include <linux/miscdevice.h>

struct bao_iodispatcher_drv {
    struct miscdevice miscdev;
};

static int bao_io_dispatcher_driver_open(struct inode* inode, struct file* filp)
{
    struct miscdevice* misc = filp->private_data;
    struct bao_iodispatcher_drv* drv;

    drv = container_of(misc, struct bao_iodispatcher_drv, miscdev);
    filp->private_data = drv;

    return 0;
}

static int bao_io_dispatcher_driver_release(struct inode* inode, struct file* filp)
{
    filp->private_data = NULL;
    return 0;
}

static long bao_io_dispatcher_driver_ioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
    struct bao_dm_info* info;

    switch (cmd) {
        case BAO_IOCTL_DM_GET_INFO:
            info = memdup_user((void __user*)arg, sizeof(*info));
            if (IS_ERR(info)) {
                return PTR_ERR(info);
            }

            if (!bao_dm_get_info(info)) {
                kfree(info);
                return -ENOENT;
            }

            if (copy_to_user((void __user*)arg, info, sizeof(*info))) {
                kfree(info);
                return -EFAULT;
            }

            kfree(info);
            return 0;

        default:
            return -ENOTTY;
    }
}

static const struct file_operations bao_io_dispatcher_driver_fops = {
    .owner = THIS_MODULE,
    .open = bao_io_dispatcher_driver_open,
    .release = bao_io_dispatcher_driver_release,
    .unlocked_ioctl = bao_io_dispatcher_driver_ioctl,
};

static int bao_io_dispatcher_driver_probe(struct platform_device* pdev)
{
    struct device* dev = &pdev->dev;
    struct bao_iodispatcher_drv* drv;
    struct bao_dm* dm;
    struct bao_dm_info dm_info;
    struct resource* r;
    int ret;
    int irq;
    int i;
    resource_size_t reg_size;

    drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
    if (!drv) {
        return -ENOMEM;
    }

    for (i = 0; i < BAO_IO_MAX_DMS; i++) {
        r = platform_get_resource(pdev, IORESOURCE_MEM, i);
        if (!r) {
            break;
        }

        irq = platform_get_irq(pdev, i);
        if (irq < 0) {
            dev_err(dev, "failed to get IRQ at index %d\n", i);
            ret = irq;
            goto err_unregister_dms;
        }

        reg_size = resource_size(r);

        dm_info.id = i;
        dm_info.shmem_addr = (unsigned long)r->start;
        dm_info.shmem_size = (unsigned long)reg_size;
        dm_info.irq = irq;
        dm_info.fd = 0;

        dm = bao_dm_create(&dm_info);
        if (!dm) {
            dev_err(dev, "failed to create Bao DM %d\n", i);
            ret = -EINVAL;
            goto err_unregister_dms;
        }

        ret = bao_intc_init(dm);
        if (ret) {
            dev_err(dev, "failed to register interrupt %d\n", irq);
            goto err_unregister_dms;
        }
    }

    drv->miscdev.minor = MISC_DYNAMIC_MINOR;
    drv->miscdev.name = "bao-io-dispatcher";
    drv->miscdev.fops = &bao_io_dispatcher_driver_fops;
    drv->miscdev.parent = dev;

    ret = misc_register(&drv->miscdev);
    if (ret) {
        dev_err(dev, "failed to register misc device: %d\n", ret);
        goto err_unregister_irqs;
    }

    platform_set_drvdata(pdev, drv);

    dev_info(dev, "Bao I/O dispatcher device registered\n");
    return 0;

err_unregister_irqs:
    list_for_each_entry(dm, &bao_dm_list, list) bao_intc_destroy(dm);

err_unregister_dms:
    list_for_each_entry(dm, &bao_dm_list, list) bao_dm_destroy(dm);

    return ret;
}

static int bao_io_dispatcher_driver_remove(struct platform_device* pdev)
{
    struct bao_iodispatcher_drv* drv = platform_get_drvdata(pdev);
    struct bao_dm* dm;
    struct bao_dm* tmp;

    if (drv) {
        misc_deregister(&drv->miscdev);
    }

    list_for_each_entry_safe(dm, tmp, &bao_dm_list, list)
    {
        bao_intc_destroy(dm);
        bao_dm_destroy(dm);
    }

    return 0;
}

static const struct of_device_id bao_io_dispatcher_driver_dt_ids[] = {
    { .compatible = "bao,io-dispatcher" }, { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bao_io_dispatcher_driver_dt_ids);

static struct platform_driver bao_io_dispatcher_driver = {
	.probe = bao_io_dispatcher_driver_probe,
	.remove = bao_io_dispatcher_driver_remove,
	.driver = {
		.name = "bao-io-dispatcher",
		.of_match_table = bao_io_dispatcher_driver_dt_ids,
	},
};

module_platform_driver(bao_io_dispatcher_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("João Peixoto <joaopeixoto@osyx.tech>");
MODULE_AUTHOR("David Cerdeira <davidmcerdeira@osyx.tech>");
MODULE_AUTHOR("José Martins <jose@osyx.tech>");
MODULE_DESCRIPTION("Bao Hypervisor I/O Dispatcher Kernel Driver");
