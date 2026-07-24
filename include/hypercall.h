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

#if defined(CONFIG_ARM64)
static inline unsigned long bao_ipcshmem_hypercall(unsigned long ipcshmem_id)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
					 ARM_SMCCC_OWNER_VENDOR_HYP,
					 BAO_IPCSHMEM_HYPERCALL_ID),
		      ipcshmem_id, 0, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long
bao_remio_hypercall(struct bao_remio_hypercall_ctx *ctx)
{
	register int x0 asm("x0") =
		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				   ARM_SMCCC_OWNER_VENDOR_HYP, BAO_REMIO_HYPERCALL_ID);
	register u64 x1 asm("x1") = ctx->dm_id;
	register u64 x2 asm("x2") = ctx->addr;
	register u64 x3 asm("x3") = ctx->op;
	register u64 x4 asm("x4") = ctx->value;
	register u64 x5 asm("x5") = ctx->request_id;
	register u64 x6 asm("x6") = 0;

	asm volatile("hvc 0\n\t"
		     : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4),
		       "=r"(x5), "=r"(x6)
		     : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		     : "memory");

	ctx->addr = x1;
	ctx->op = x2;
	ctx->value = x3;
	ctx->access_width = x4;
	ctx->request_id = x5;
	ctx->npend_req = x6;

	return x0;
}
#elif defined(CONFIG_ARM)
static inline unsigned long bao_ipcshmem_hypercall(unsigned long ipcshmem_id)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,
					 ARM_SMCCC_OWNER_VENDOR_HYP,
					 BAO_IPCSHMEM_HYPERCALL_ID),
		      ipcshmem_id, 0, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long
bao_remio_hypercall(struct bao_remio_hypercall_ctx *ctx)
{
	register int r0 asm("r0") =
		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,
				   ARM_SMCCC_OWNER_VENDOR_HYP, BAO_REMIO_HYPERCALL_ID);
	register u32 r1 asm("r1") = ctx->dm_id;
	register u32 r2 asm("r2") = ctx->addr;
	register u32 r3 asm("r3") = ctx->op;
	register u32 r4 asm("r4") = ctx->value;
	register u32 r5 asm("r5") = ctx->request_id;
	register u32 r6 asm("r6") = 0;

	asm volatile("hvc 0\n\t"
		     : "=r"(r0), "=r"(r1), "=r"(r2), "=r"(r3), "=r"(r4),
		       "=r"(r5), "=r"(r6)
		     : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
		     : "memory");

	ctx->addr = r1;
	ctx->op = r2;
	ctx->value = r3;
	ctx->access_width = r4;
	ctx->request_id = r5;
	ctx->npend_req = r6;

	return r0;
}
#elif defined(CONFIG_RISCV)
#define BAO_SBI_EXT_ID 0x08000ba0

static inline unsigned long bao_ipcshmem_hypercall(unsigned long ipcshmem_id)
{
	struct sbiret ret;

	ret = sbi_ecall(BAO_SBI_EXT_ID, BAO_IPCSHMEM_HYPERCALL_ID, ipcshmem_id,
			0, 0, 0, 0, 0);

	return ret.error;
}

static inline unsigned long
bao_remio_hypercall(struct bao_remio_hypercall_ctx *ctx)
{
	register uintptr_t a0 asm("a0") = (uintptr_t)(ctx->dm_id);
	register uintptr_t a1 asm("a1") = (uintptr_t)(ctx->addr);
	register uintptr_t a2 asm("a2") = (uintptr_t)(ctx->op);
	register uintptr_t a3 asm("a3") = (uintptr_t)(ctx->value);
	register uintptr_t a4 asm("a4") = (uintptr_t)(ctx->request_id);
	register uintptr_t a5 asm("a5") = (uintptr_t)(0);
	register uintptr_t a6 asm("a6") = (uintptr_t)(BAO_REMIO_HYPERCALL_ID);
	register uintptr_t a7 asm("a7") = (uintptr_t)(0x08000ba0);

	asm volatile("ecall"
		     : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4),
		       "+r"(a5), "+r"(a6), "+r"(a7)
		     :
		     : "memory");

	ctx->addr = a2;
	ctx->op = a3;
	ctx->value = a4;
	ctx->access_width = a5;
	ctx->request_id = a6;
	ctx->npend_req = a7;

	return a1;
}
#endif

#endif /* __BAO_HYPERCALL_H */
