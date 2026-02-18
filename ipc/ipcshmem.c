// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor IPC Through Shared-memory Driver
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <bao.h>
#include <hypercall.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/mm.h>
#include <linux/io.h>

#define BAO_IPCSHMEM_NAME_LEN 16

struct bao_ipcshmem {
    struct miscdevice miscdev;
    int id;
    char label[BAO_IPCSHMEM_NAME_LEN];
    void* read_base;
    size_t read_size;
    void* write_base;
    size_t write_size;
    phys_addr_t physical_base;
    size_t shmem_size;
    void* shmem_base_addr;
};

static int bao_ipcshmem_mmap(struct file* filp, struct vm_area_struct* vma)
{
    struct bao_ipcshmem* bao = filp->private_data;
    unsigned long vsize = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    phys_addr_t paddr;

    if (!vsize) {
        return -EINVAL;
    }

    if (offset >= bao->shmem_size || vsize > bao->shmem_size - offset) {
        return -EINVAL;
    }

    paddr = bao->physical_base + offset;

    if (!PAGE_ALIGNED(paddr)) {
        return -EINVAL;
    }

    return remap_pfn_range(vma, vma->vm_start, paddr >> PAGE_SHIFT, vsize, vma->vm_page_prot);
}

static ssize_t bao_ipcshmem_read(struct file* filp, char __user* buf, size_t count, loff_t* ppos)
{
    struct bao_ipcshmem* bao = filp->private_data;
    size_t available;

    if (*ppos >= bao->read_size) {
        return 0;
    }

    available = bao->read_size - *ppos;
    count = min(count, available);

    if (copy_to_user(buf, bao->read_base + *ppos, count)) {
        return -EFAULT;
    }

    *ppos += count;
    return count;
}

static ssize_t bao_ipcshmem_write(struct file* filp, const char __user* buf, size_t count,
    loff_t* ppos)
{
    struct bao_ipcshmem* bao = filp->private_data;
    size_t available;

    if (*ppos >= bao->write_size) {
        return 0;
    }

    available = bao->write_size - *ppos;
    count = min(count, available);

    if (copy_from_user(bao->write_base + *ppos, buf, count)) {
        return -EFAULT;
    }

    *ppos += count;

    /* Notify Bao hypervisor */
    bao_ipcshmem_hypercall(bao->id);

    return count;
}

static int bao_ipcshmem_open(struct inode* inode, struct file* filp)
{
    struct bao_ipcshmem* bao;

    bao = container_of(filp->private_data, struct bao_ipcshmem, miscdev);
    filp->private_data = bao;

    return 0;
}

static int bao_ipcshmem_release(struct inode* inode, struct file* filp)
{
    filp->private_data = NULL;
    return 0;
}

static const struct file_operations bao_ipcshmem_fops = {
    .owner = THIS_MODULE,
    .read = bao_ipcshmem_read,
    .write = bao_ipcshmem_write,
    .mmap = bao_ipcshmem_mmap,
    .open = bao_ipcshmem_open,
    .release = bao_ipcshmem_release,
};

static int bao_ipcshmem_probe(struct platform_device* pdev)
{
    struct device* dev = &pdev->dev;
    struct device_node* np = dev->of_node;
    struct resource* r;
    struct bao_ipcshmem* bao;
    resource_size_t shmem_size;
    u32 write_offset;
    u32 read_offset;
    u32 write_size;
    u32 read_size;
    u32 id;
    bool rd_in_range;
    bool wr_in_range;
    bool disjoint;
    int ret;

    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!r) {
        dev_err(dev, "missing shared memory resource\n");
        return -ENODEV;
    }

    ret = of_property_read_u32(np, "id", &id);
    if (ret) {
        dev_err(dev, "missing or invalid 'id' property\n");
        return ret;
    }

    ret = of_property_read_u32_index(np, "read-channel", 0, &read_offset);
    if (ret) {
        dev_err(dev, "failed to read 'read-channel' offset: %d\n", ret);
        return ret;
    }

    ret = of_property_read_u32_index(np, "read-channel", 1, &read_size);
    if (ret) {
        dev_err(dev, "failed to read 'read-channel' size: %d\n", ret);
        return ret;
    }

    ret = of_property_read_u32_index(np, "write-channel", 0, &write_offset);
    if (ret) {
        dev_err(dev, "failed to read 'write-channel' offset: %d\n", ret);
        return ret;
    }

    ret = of_property_read_u32_index(np, "write-channel", 1, &write_size);
    if (ret) {
        dev_err(dev, "failed to read 'write-channel' size: %d\n", ret);
        return ret;
    }

    shmem_size = resource_size(r);

    rd_in_range = (read_offset + read_size) <= shmem_size;
    wr_in_range = (write_offset + write_size) <= shmem_size;
    disjoint =
        ((read_offset + read_size) <= write_offset) || ((write_offset + write_size) <= read_offset);

    if (!rd_in_range || !wr_in_range || !disjoint) {
        dev_err(dev, "invalid read/write channel ranges\n");
        return -EINVAL;
    }

    bao = devm_kzalloc(dev, sizeof(*bao), GFP_KERNEL);
    if (!bao) {
        return -ENOMEM;
    }

    bao->shmem_base_addr = devm_memremap(dev, r->start, shmem_size, MEMREMAP_WB);
    if (!bao->shmem_base_addr) {
        dev_err(dev, "failed to remap shared memory\n");
        return -ENOMEM;
    }

    bao->id = id;
    bao->read_size = read_size;
    bao->write_size = write_size;
    bao->read_base = (u8*)bao->shmem_base_addr + read_offset;
    bao->write_base = (u8*)bao->shmem_base_addr + write_offset;
    bao->physical_base = r->start;
    bao->shmem_size = shmem_size;

    scnprintf(bao->label, BAO_IPCSHMEM_NAME_LEN, "baoipc%d", id);

    bao->miscdev.minor = MISC_DYNAMIC_MINOR;
    bao->miscdev.name = bao->label;
    bao->miscdev.fops = &bao_ipcshmem_fops;
    bao->miscdev.parent = dev;

    ret = misc_register(&bao->miscdev);
    if (ret) {
        dev_err(dev, "failed to register misc device: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, bao);

    dev_info(dev, "Bao IPC shared memory device '%s' registered\n", bao->label);
    return 0;
}

static void bao_ipcshmem_remove(struct platform_device* pdev)
{
    struct bao_ipcshmem* bao = platform_get_drvdata(pdev);

    if (bao) {
        misc_deregister(&bao->miscdev);
    }
}

static const struct of_device_id of_bao_ipcshmem_match[] = { { .compatible = "bao,ipcshmem" },
    { /* sentinel */ } };
MODULE_DEVICE_TABLE(of, of_bao_ipcshmem_match);

static struct platform_driver bao_ipcshmem_driver = {
	.probe = bao_ipcshmem_probe,
	.remove = bao_ipcshmem_remove,
	.driver = {
		.name = "baoipc",
		.of_match_table = of_bao_ipcshmem_match,
	},
};

module_platform_driver(bao_ipcshmem_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Cerdeira <davidmcerdeira@osyx.tech>");
MODULE_AUTHOR("José Martins <jose@osyx.tech>");
MODULE_AUTHOR("João Peixoto <joaopeixoto@osyx.tech>");
MODULE_DESCRIPTION("Bao Hypervisor IPC Through Shared-memory Driver");
