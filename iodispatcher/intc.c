// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher Interrupt Controller
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixoto@osyx.tech>
 *	José Martins <jose@osyx.tech>
 *	David Cerdeira <davidmcerdeira@osyx.tech>
 */

#include <linux/interrupt.h>
#include "bao.h"

/* Top-level handler registered by the Bao interrupt controller */
static void (*bao_intc_handler)(struct bao_dm* dm);

/**
 * bao_interrupt_handler - Top-level interrupt handler for Bao DM
 * @irq: Interrupt number
 * @dev: Pointer to the Bao device model (struct bao_dm)
 *
 * Invokes the registered Bao interrupt controller handler, if any.
 */
static irqreturn_t bao_interrupt_handler(int irq, void* dev)
{
    struct bao_dm* dm = (struct bao_dm*)dev;

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

int bao_intc_init(struct bao_dm* dm)
{
    char name[BAO_NAME_MAX_LEN];

    if (WARN_ON_ONCE(!dm)) {
        return -EINVAL;
    }

    scnprintf(name, sizeof(name), "bao-iodintc%d", dm->info.id);

    return request_irq(dm->info.irq, bao_interrupt_handler, 0, name, dm);
}

void bao_intc_destroy(struct bao_dm* dm)
{
    if (WARN_ON_ONCE(!dm)) {
        return;
    }

    free_irq(dm->info.irq, dm);
}
