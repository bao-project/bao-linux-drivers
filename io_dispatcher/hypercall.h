// SPDX-License-Identifier: GPL-2.0
/*
 * Hypercall API for Bao Hypervisor
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef __BAO_HYPERCALL_H
#define __BAO_HYPERCALL_H

#include "bao.h"

#if defined(CONFIG_ARM64) || defined(CONFIG_ARM)
#include <linux/arm-smccc.h>
#elif CONFIG_RISCV
#include <asm/sbi.h>
#endif

/* Remote I/O Hypercall ID */
#define REMIO_HC_ID 0x2

/**
 * Remote I/O Hypercall return structure
 * @hyp_ret: The generic return value of Bao's hypercall
 * @remio_hyp_ret: The return value of the Remote I/O Hypercall
 * @pending_requests: The number of pending requests (only used in the Remote
 * I/O Ask Hypercall)
 */
struct remio_hypercall_ret {
    u64 hyp_ret;
    u64 remio_hyp_ret;
    u64 pending_requests;
};

#if defined(CONFIG_ARM64)
/**
 * asm_bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret asm_bao_hypercall_remio(struct bao_virtio_request* request)
{
    struct remio_hypercall_ret ret;
    register int x0 asm("x0") = ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
        ARM_SMCCC_OWNER_VENDOR_HYP, REMIO_HC_ID);
    register u64 x1 asm("x1") = request->dm_id;
    register u64 x2 asm("x2") = request->addr;
    register u64 x3 asm("x3") = request->op;
    register u64 x4 asm("x4") = request->value;
    register u64 x5 asm("x5") = request->request_id;
    register u64 x6 asm("x6") = 0;

    asm volatile("hvc 0\n\t" : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4), "=r"(x5),
        "=r"(x6) : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");

    ret.hyp_ret = 0;
    ret.remio_hyp_ret = x0;
    ret.pending_requests = x6;

    request->addr = x1;
    request->op = x2;
    request->value = x3;
    request->access_width = x4;
    request->request_id = x5;

    return ret;
}
#elif defined(CONFIG_ARM)
/**
 * asm_bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret asm_bao_hypercall_remio(struct bao_virtio_request* request)
{
    struct remio_hypercall_ret ret;
    register int x0 asm("r0") = ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
        ARM_SMCCC_OWNER_VENDOR_HYP, REMIO_HC_ID);
    register u32 x1 asm("r1") = request->dm_id;
    register u32 x2 asm("r2") = request->addr;
    register u32 x3 asm("r3") = request->op;
    register u32 x4 asm("r4") = request->value;
    register u32 x5 asm("r5") = request->request_id;
    register u32 x6 asm("r6") = 0;

    asm volatile("hvc 0\n\t" : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4), "=r"(x5),
        "=r"(x6) : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");

    ret.hyp_ret = 0;
    ret.remio_hyp_ret = x0;
    ret.pending_requests = x6;

    request->addr = x1;
    request->op = x2;
    request->value = x3;
    request->access_width = x4;
    request->request_id = x5;

    return ret;
}
#elif defined(CONFIG_RISCV)
/**
 * asm_bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret asm_bao_hypercall_remio(struct bao_virtio_request* request)
{
    struct remio_hypercall_ret ret;
    register uintptr_t a0 asm("a0") = (uintptr_t)(request->dm_id);
    register uintptr_t a1 asm("a1") = (uintptr_t)(request->addr);
    register uintptr_t a2 asm("a2") = (uintptr_t)(request->op);
    register uintptr_t a3 asm("a3") = (uintptr_t)(request->value);
    register uintptr_t a4 asm("a4") = (uintptr_t)(request->request_id);
    register uintptr_t a5 asm("a5") = (uintptr_t)(0);
    register uintptr_t a6 asm("a6") = (uintptr_t)(REMIO_HC_ID);
    register uintptr_t a7 asm("a7") = (uintptr_t)(0x08000ba0);

    asm volatile("ecall"
        : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7)
        : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7) : "memory");

    ret.hyp_ret = a0;
    ret.remio_hyp_ret = a1;
    ret.pending_requests = a7;

    request->addr = a2;
    request->op = a3;
    request->value = a4;
    request->access_width = a5;
    request->request_id = a6;

    return ret;
}
#endif

/**
 * bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret bao_hypercall_remio(struct bao_virtio_request* request)
{
    return asm_bao_hypercall_remio(request);
}

#endif /* __BAO_HYPERCALL_H */
