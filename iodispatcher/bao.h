/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Provides some definitions for the Bao Hypervisor modules
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixoto@osyx.tech>
 *	José Martins <jose@osyx.tech>
 *	David Cerdeira <davidmcerdeira@osyx.tech>
 */

#ifndef __BAO_DRV_H
#define __BAO_DRV_H

#include <linux/fs.h>
#include <linux/types.h>

#define BAO_NAME_MAX_LEN 16
#define BAO_IO_MAX_DMS 16

#define BAO_IOEVENTFD_FLAG_DATAMATCH BIT(1)
#define BAO_IOEVENTFD_FLAG_DEASSIGN BIT(2)
#define BAO_IRQFD_FLAG_DEASSIGN 1U
#define BAO_IO_CLIENT_DESTROYING 0U

/* IPC through shared-memory hypercall ID */
#define BAO_IPCSHMEM_HYPERCALL_ID 0x1

/* Remote I/O Hypercall ID */
#define BAO_REMIO_HYPERCALL_ID 0x2

/**
 * struct bao_virtio_request - Parameters of a Bao VirtIO request
 * @dm_id: Device model ID
 * @addr: MMIO register address accessed
 * @op: Operation type (WRITE = 0, READ, ASK, NOTIFY)
 * @value: Value to write or read
 * @access_width: Access width (VirtIO MMIO supports 4-byte aligned accesses)
 * @request_id: Request ID of the I/O request
 */
struct bao_virtio_request {
	__u64 dm_id;
	__u64 addr;
	__u64 op;
	__u64 value;
	__u64 access_width;
	__u64 request_id;
};

/**
 * struct bao_ioeventfd - Parameters of an ioeventfd request
 * @fd: Eventfd file descriptor associated with the I/O request
 * @flags: Logical OR of BAO_IOEVENTFD_FLAG_*
 * @addr: Start address of the I/O range
 * @len: Length of the I/O range
 * @reserved: Reserved, must be 0
 * @data: Data for matching (used if data matching is enabled)
 */
struct bao_ioeventfd {
	__u32 fd;
	__u32 flags;
	__u64 addr;
	__u32 len;
	__u32 reserved;
	__u64 data;
};

/**
 * struct bao_irqfd - Parameters of an IRQFD request
 * @fd: File descriptor of the eventfd
 * @flags: Flags associated with the eventfd
 */
struct bao_irqfd {
	__s32 fd;
	__u32 flags;
};

/**
 * struct bao_dm_info - Parameters of a Bao device model
 * @id: Virtual ID of the DM
 * @shmem_addr: Base address of the shared memory
 * @shmem_size: Size of the shared memory
 * @irq: IRQ number
 * @fd: File descriptor of the DM
 */
struct bao_dm_info {
	__u32 id;
	__u64 shmem_addr;
	__u64 shmem_size;
	__u32 irq;
	__s32 fd;
};

/*
 * The ioctl type for Bao, documented in
 * Documentation/userspace-api/ioctl/ioctl-number.rst
 */
#define BAO_IOCTL_TYPE 0xA6

/*
 * Bao userspace IOCTL commands
 * Follows Linux kernel convention, see Documentation/driver-api/ioctl.rst
 */
#define BAO_IOCTL_DM_GET_INFO _IOWR(BAO_IOCTL_TYPE, 0x01, struct bao_dm_info)
#define BAO_IOCTL_IO_CLIENT_ATTACH \
	_IOWR(BAO_IOCTL_TYPE, 0x02, struct bao_virtio_request)
#define BAO_IOCTL_IO_REQUEST_COMPLETE \
	_IOW(BAO_IOCTL_TYPE, 0x03, struct bao_virtio_request)
#define BAO_IOCTL_IOEVENTFD _IOW(BAO_IOCTL_TYPE, 0x04, struct bao_ioeventfd)
#define BAO_IOCTL_IRQFD _IOW(BAO_IOCTL_TYPE, 0x05, struct bao_irqfd)

/**
 * struct bao_remio_hypercall_ctx - REMIO hypercall context
 * @dm_id: Device model identifier
 * @addr: Target address
 * @op: Operation code
 * @value: Value to read/write
 * @access_width: Access width in bytes
 * @request_id: Request identifier
 * @npend_req: Number of pending requests
 */
struct bao_remio_hypercall_ctx {
	u64 dm_id;
	u64 addr;
	u64 op;
	u64 value;
	u64 access_width;
	u64 request_id;
	u64 npend_req;
};

struct bao_dm;
struct bao_io_client;

typedef int (*bao_io_client_handler_t)(struct bao_io_client *client,
				       struct bao_virtio_request *req);

/**
 * enum bao_io_op - Bao hypervisor I/O operation types
 * @BAO_IO_WRITE:   Write operation
 * @BAO_IO_READ:    Read operation
 * @BAO_IO_ASK:     Request operation information (e.g., MMIO address)
 * @BAO_IO_NOTIFY:  Notify I/O completion
 */
enum bao_io_op {
	BAO_IO_WRITE = 0,
	BAO_IO_READ,
	BAO_IO_ASK,
	BAO_IO_NOTIFY,
};

/**
 * struct bao_io_client - Bao I/O client
 * @name: Client name
 * @dm: The DM that the client belongs to
 * @list: List node for this bao_io_client
 * @is_control: If this client is the control client
 * @flags: Flags (BAO_IO_CLIENT_*)
 * @virtio_requests: List of free I/O requests
 * @range_list: I/O ranges
 * @handler: I/O request handler for this client
 * @thread: Kernel thread executing the handler
 * @wq: Wait queue used for thread parking
 * @priv: Private data for the handler
 */
struct bao_io_client {
	char name[BAO_NAME_MAX_LEN];
	struct bao_dm *dm;
	struct list_head list;
	bool is_control;
	unsigned long flags;
	struct list_head virtio_requests;

	/* protects virtio_requests list */
	struct mutex virtio_requests_lock;

	struct list_head range_list;

	/* protects range_list */
	struct rw_semaphore range_lock;

	bao_io_client_handler_t handler;
	struct task_struct *thread;
	wait_queue_head_t wq;
	void *priv;
};

/**
 * struct bao_dm - Bao backend device model (DM)
 * @list: Entry within global list of all DMs
 * @info: DM information (id, shmem_addr, shmem_size, irq, fd)
 * @shmem_base_addr: The base address of the shared memory
 * @ioeventfds: List of all ioeventfds
 * @ioeventfd_client: Ioeventfd client
 * @irqfds: List of all irqfds
 * @irqfd_server: Workqueue responsible for irqfd handling
 * @io_clients: List of all bao_io_client
 * @control_client: Control client
 * @refcount: Each open file holds a reference to the DM
 */
struct bao_dm {
	struct list_head list;
	struct bao_dm_info info;
	void *shmem_base_addr;

	struct list_head ioeventfds;

	/* protects ioeventfds list */
	struct mutex ioeventfds_lock;

	struct bao_io_client *ioeventfd_client;

	struct list_head irqfds;

	/* protects irqfds list */
	struct mutex irqfds_lock;

	struct workqueue_struct *irqfd_server;

	/* protects io_clients list */
	struct rw_semaphore io_clients_lock;

	struct list_head io_clients;
	struct bao_io_client *control_client;

	refcount_t refcount;
};

/**
 * struct bao_io_range - Represents a range of I/O addresses
 * @list: List node for linking multiple ranges
 * @start: Start address of the range
 * @end: End address of the range (inclusive)
 */
struct bao_io_range {
	struct list_head list;
	u64 start;
	u64 end;
};

/* Global list of all Bao device models */
extern struct list_head bao_dm_list;

/* Lock protecting access to bao_dm_list */
extern rwlock_t bao_dm_list_lock;

/**
 * bao_dm_create - Create a backend device model (DM)
 * @info: DM information (id, shmem_addr, shmem_size, irq, fd)
 *
 * Return: Pointer to the created DM on success, NULL on error.
 */
struct bao_dm *bao_dm_create(struct bao_dm_info *info);

/**
 * bao_dm_destroy - Destroy a backend device model (DM)
 * @dm: DM to be destroyed
 */
void bao_dm_destroy(struct bao_dm *dm);

/**
 * bao_dm_get_info - Retrieve information of a DM
 * @info: Structure to be filled; id field must contain the DM ID
 *
 * Return: True on success, false on error.
 */
bool bao_dm_get_info(struct bao_dm_info *info);

/**
 * bao_io_client_create - Create a backend I/O client
 * @dm: DM this client belongs to
 * @handler: I/O client handler for requests
 * @data: Private data passed to the handler
 * @is_control: True if this is the control client
 * @name: Name of the I/O client
 *
 * Return: Pointer to the created I/O client, NULL on failure.
 */
struct bao_io_client *bao_io_client_create(struct bao_dm *dm,
					   bao_io_client_handler_t handler,
					   void *data, bool is_control,
					   const char *name);

/**
 * bao_io_clients_destroy - Destroy all I/O clients of a DM
 * @dm: DM whose I/O clients are to be destroyed
 */
void bao_io_clients_destroy(struct bao_dm *dm);

/**
 * bao_io_client_attach - Attach a thread to an I/O client
 * @client: I/O client to attach
 *
 * The thread will wait for I/O requests on this client.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_io_client_attach(struct bao_io_client *client);

/**
 * bao_io_client_range_add - Add an I/O range to monitor in a client
 * @client: I/O client
 * @start: Start address of the range
 * @end: End address of the range (inclusive)
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_io_client_range_add(struct bao_io_client *client, u64 start, u64 end);

/**
 * bao_io_client_range_del - Remove an I/O range from a client
 * @client: I/O client
 * @start: Start address of the range
 * @end: End address of the range (inclusive)
 */
void bao_io_client_range_del(struct bao_io_client *client, u64 start, u64 end);

/**
 * bao_io_client_request - Retrieve the oldest I/O request from a client
 * @client: I/O client
 * @req: Pointer to virtio request structure to fill
 *
 * Return: 0 on success, negative error code if no request is available.
 */
int bao_io_client_request(struct bao_io_client *client,
			  struct bao_virtio_request *req);

/**
 * bao_io_client_push_request - Push an I/O request into a client
 * @client: I/O client
 * @req: I/O request to push
 *
 * Return: True if a request was pushed, false otherwise.
 */
bool bao_io_client_push_request(struct bao_io_client *client,
				struct bao_virtio_request *req);

/**
 * bao_io_client_pop_request - Pop the oldest I/O request from a client
 * @client: I/O client
 * @req: Buffer to store the popped request
 *
 * Return: True if a request was popped, false if the list was empty.
 */
bool bao_io_client_pop_request(struct bao_io_client *client,
			       struct bao_virtio_request *req);

/**
 * bao_io_client_find - Find the I/O client for a given request
 * @dm: DM that the I/O request belongs to
 * @req: I/O request to locate
 *
 * Return: Pointer to the I/O client handling the request, NULL if none found.
 */
struct bao_io_client *bao_io_client_find(struct bao_dm *dm,
					 struct bao_virtio_request *req);

/**
 * bao_ioeventfd_client_init - Initialize the Ioeventfd client for a DM
 * @dm: DM that the Ioeventfd client belongs to
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_ioeventfd_client_init(struct bao_dm *dm);

/**
 * bao_ioeventfd_client_destroy - Destroy the Ioeventfd client for a DM
 * @dm: DM that the Ioeventfd client belongs to
 */
void bao_ioeventfd_client_destroy(struct bao_dm *dm);

/**
 * bao_ioeventfd_client_config - Configure an Ioeventfd client
 * @dm: DM that the Ioeventfd client belongs to
 * @config: Ioeventfd configuration to apply
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_ioeventfd_client_config(struct bao_dm *dm,
				struct bao_ioeventfd *config);

/**
 * bao_irqfd_server_init - Initialize the Irqfd server for a DM
 * @dm: DM that the Irqfd server belongs to
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_irqfd_server_init(struct bao_dm *dm);

/**
 * bao_irqfd_server_destroy - Destroy the Irqfd server for a DM
 * @dm: DM that the Irqfd server belongs to
 */
void bao_irqfd_server_destroy(struct bao_dm *dm);

/**
 * bao_irqfd_server_config - Configure an Irqfd server
 * @dm: DM that the Irqfd server belongs to
 * @config: Irqfd configuration to apply
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_irqfd_server_config(struct bao_dm *dm, struct bao_irqfd *config);

/**
 * bao_io_dispatcher_init - Initialize the I/O Dispatcher for a DM
 * @dm: DM to initialize on the I/O Dispatcher
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_io_dispatcher_init(struct bao_dm *dm);

/**
 * bao_io_dispatcher_destroy - Destroy the I/O Dispatcher for a DM
 * @dm: DM to destroy on the I/O Dispatcher
 */
void bao_io_dispatcher_destroy(struct bao_dm *dm);

/**
 * bao_dispatch_io - Acquire and dispatch I/O requests from the Bao Hypervisor
 * @dm: DM whose I/O clients will handle the requests
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_dispatch_io(struct bao_dm *dm);

/**
 * bao_io_dispatcher_pause - Pause the I/O Dispatcher for a DM
 * @dm: DM to pause
 */
void bao_io_dispatcher_pause(struct bao_dm *dm);

/**
 * bao_io_dispatcher_resume - Resume the I/O Dispatcher for a DM
 * @dm: DM to resume
 */
void bao_io_dispatcher_resume(struct bao_dm *dm);

/**
 * bao_intc_init - Register the interrupt controller for a DM
 * @dm: DM that the interrupt controller belongs to
 *
 * Return: 0 on success, negative error code on failure.
 */
int bao_intc_init(struct bao_dm *dm);

/**
 * bao_intc_destroy - Unregister the interrupt controller for a DM
 * @dm: DM that the interrupt controller belongs to
 */
void bao_intc_destroy(struct bao_dm *dm);

/**
 * bao_intc_setup_handler - Setup the interrupt controller handler
 * @handler: Function pointer to the interrupt handler
 * @dm: DM that the interrupt controller belongs to
 */
void bao_intc_setup_handler(void (*handler)(struct bao_dm *dm));

/**
 * bao_intc_remove_handler - Remove the interrupt controller handler
 */
void bao_intc_remove_handler(void);

#endif /* __BAO_DRV_H */
