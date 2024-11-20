// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher Interrupt Controller
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include "bao.h"
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

// handler for the interrupt
static void (*bao_intc_handler)(struct bao_dm* dm);

static irqreturn_t bao_interrupt_handler(int irq, void* dev)
{
    struct bao_dm* dm = (struct bao_dm*)dev;

    // if the handler is set, call it
    if (bao_intc_handler) {
        bao_intc_handler(dm);
    }

    return IRQ_HANDLED;
}

void bao_intc_setup_handler(void (*handler)(struct bao_dm* dm))
{
    bao_intc_handler = handler;
}

void bao_intc_remove_handler(void)
{
    bao_intc_handler = NULL;
}

int bao_intc_register(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];
    snprintf(name, BAO_NAME_MAX_LEN, "bao-iodintc%d", dm->info.id);
    return request_irq(dm->info.irq, bao_interrupt_handler, 0, name, dm);
}

void bao_intc_unregister(struct bao_dm* dm)
{
    free_irq(dm->info.irq, dm);
}
