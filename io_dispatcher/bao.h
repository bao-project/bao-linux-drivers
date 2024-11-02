/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Provides the Bao Hypervisor IOCTLs and global structures
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef _BAO_H
#define _BAO_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/wait.h>

#define BAO_IO_WRITE       0x0
#define BAO_IO_READ        0x1
#define BAO_IO_ASK         0x2
#define BAO_IO_NOTIFY      0x3

#define BAO_NAME_MAX_LEN   16
#define BAO_IO_REQUEST_MAX 64
#define BAO_IO_MAX_DMS     16

/**
 * Contains the specific parameters of a Bao VirtIO request
 * @dm_id: Device Model ID
 * @addr: Gives the MMIO register address that was accessed
 * @op: Write, Read, Ask or Notify operation
 * @value: Value to write or read
 * @access_width: Access width (VirtIO MMIO only allows 4-byte wide and alligned
 * accesses)
 * @request_id: Request ID
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
 * Contains the specific parameters of a ioeventfd request
 * @fd:		The fd of eventfd associated with a hsm_ioeventfd
 * @flags:	Logical-OR of BAO_IOEVENTFD_FLAG_*
 * @addr:	The start address of IO range of ioeventfd
 * @len:	The length of IO range of ioeventfd
 * @reserved:	Reserved and should be 0
 * @data:	Data for data matching
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
 * Contains the specific parameters of a irqfd request
 * @fd: The file descriptor of the eventfd
 * @flags: The flags of the eventfd
 */
struct bao_irqfd {
    __s32 fd;
    __u32 flags;
};

/**
 * Contains the specific parameters of a Bao DM
 * @id: The virtual ID of the DM
 * @shmem_addr: The base address of the shared memory
 * @shmem_size: The size of the shared memory
 * @irq: The IRQ number
 * @fd: The file descriptor of the DM
 */
struct bao_dm_info {
    __u32 id;
    __u64 shmem_addr;
    __u64 shmem_size;
    __u32 irq;
    __s32 fd;
};

/* The ioctl type, listed in Documentation/userspace-api/ioctl/ioctl-number.rst
 */
#define BAO_IOCTL_TYPE                0xA6

/*
 * Common IOCTL IDs definition for Bao userspace
 * Follows the convention of the Linux kernel, listed in
 * Documentation/driver-api/ioctl.rst
 */
#define BAO_IOCTL_DM_GET_INFO         _IOWR(BAO_IOCTL_TYPE, 0x01, struct bao_dm_info)
#define BAO_IOCTL_IO_CLIENT_ATTACH    _IOWR(BAO_IOCTL_TYPE, 0x02, struct bao_virtio_request)
#define BAO_IOCTL_IO_REQUEST_COMPLETE _IOW(BAO_IOCTL_TYPE, 0x03, struct bao_virtio_request)
#define BAO_IOCTL_IOEVENTFD           _IOW(BAO_IOCTL_TYPE, 0x04, struct bao_ioeventfd)
#define BAO_IOCTL_IRQFD               _IOW(BAO_IOCTL_TYPE, 0x05, struct bao_irqfd)

#define BAO_IOEVENTFD_FLAG_DATAMATCH  (1 << 1)
#define BAO_IOEVENTFD_FLAG_DEASSIGN   (1 << 2)
#define BAO_IRQFD_FLAG_DEASSIGN       1U

#define BAO_IO_CLIENT_DESTROYING      0U

#define BAO_DM_FLAG_DESTROYING        0U
#define BAO_DM_FLAG_CLEARING_IOREQ    1U

struct bao_dm;
struct bao_io_client;

typedef int (*bao_io_client_handler_t)(struct bao_io_client* client, struct bao_virtio_request* req);

/**
 * Bao I/O client
 * @name: Client name
 * @dm:	The DM that the client belongs to
 * @list: List node for this bao_io_client
 * @is_control:	If this client is the control client
 * @flags: Flags (BAO_IO_CLIENT_*)
 * @virtio_requests: Array of all I/O requests that are free to process
 * @virtio_requests_lock: Lock to protect virtio_requests list
 * @range_list:	I/O ranges
 * @range_lock:	Semaphore to protect range_list
 * @handler: I/O requests handler of this client
 * @thread:	The thread which executes the handler
 * @wq:	The wait queue for the handler thread parking
 * @priv: Data for the thread
 */
struct bao_io_client {
    char name[BAO_NAME_MAX_LEN];
    struct bao_dm* dm;
    struct list_head list;
    bool is_control;
    unsigned long flags;
    struct list_head virtio_requests;
    struct mutex virtio_requests_lock;
    struct list_head range_list;
    struct rw_semaphore range_lock;
    bao_io_client_handler_t handler;
    struct task_struct* thread;
    wait_queue_head_t wq;
    void* priv;
};

/**
 * Bao backend device model (DM)
 * @list: Entry within global list of all DMs
 * @info: DM information (id, shmem_addr, shmem_size, irq, fd)
 * @shmem_base_addr: The base address of the shared memory (only used for
 * unmapping purposes)
 * @flags: Flags (BAO_IO_DISPATCHER_DM_*)
 * @ioeventfds: List to link all bao_ioeventfd
 * @ioeventfds_lock: Lock to protect ioeventfds list
 * @ioeventfd_client: Ioevenfd client
 * @irqfds: List to link all bao_irqfd
 * @irqfds_lock: Lock to protect irqfds list
 * @irqfd_server: Irqfd server
 * @io_clients_lock: Semaphore to protect io_clients
 * @io_clients:	List to link all bao_io_client
 * @control_client:	Control client
 */
struct bao_dm {
    struct list_head list;
    struct bao_dm_info info;
    void* shmem_base_addr;
    unsigned long flags;
    struct list_head ioeventfds;
    struct mutex ioeventfds_lock;
    struct bao_io_client* ioeventfd_client;
    struct list_head irqfds;
    struct mutex irqfds_lock;
    struct workqueue_struct* irqfd_server;
    struct rw_semaphore io_clients_lock;
    struct list_head io_clients;
    struct bao_io_client* control_client;
};

/**
 * Bao I/O request range
 * @list: List node for this range
 * @start: The start address of the range
 * @end: The end address of the range
 *
 */
struct bao_io_range {
    struct list_head list;
    u64 start;
    u64 end;
};

extern struct list_head bao_dm_list;
extern rwlock_t bao_dm_list_lock;

/**
 * Create the backend DM
 * @info: The DM information (id, shmem_addr, shmem_size, irq, fd)
 * @return dm on success, NULL on error
 */
struct bao_dm* bao_dm_create(struct bao_dm_info* info);

/**
 * Destroy the backend DM
 * @dm: The DM to be destroyed
 */
void bao_dm_destroy(struct bao_dm* dm);

/**
 * Get the DM information
 * @info: The DM information to be filled (id field contains the DM ID)
 * @return true on success, false on error
 */
bool bao_dm_get_info(struct bao_dm_info* info);

/**
 * DM ioctls handler
 * @filp: The open file pointer
 * @cmd: The ioctl command
 * @ioctl_param: The ioctl parameter
 */
long bao_dm_ioctl(struct file* filp, unsigned int cmd, unsigned long ioctl_param);

/**
 * Create an I/O client
 * @dm:	The DM that this client belongs to
 * @handler: The I/O client handler for the I/O requests
 * @data: Private data for the handler
 * @is_control:	If it is the control client
 * @name: The name of I/O client
 */
struct bao_io_client* bao_io_client_create(struct bao_dm* dm, bao_io_client_handler_t handler,
    void* data, bool is_control, const char* name);

/**
 * Destroy the I/O clients of the DM
 * @dm: The DM that the I/O clients belong to
 */
void bao_io_clients_destroy(struct bao_dm* dm);

/**
 * Attach the thread to the I/O client to wait for I/O requests
 * @client: The I/O client to handle the I/O request
 */
int bao_io_client_attach(struct bao_io_client* client);

/**
 * Add an I/O range monitor into an I/O client
 * @client: The I/O client that the range will be added
 * @start: The start address of the range
 * @end: The end address of the range
 */
int bao_io_client_range_add(struct bao_io_client* client, u64 start, u64 end);

/**
 * Delete an I/O range monitor from an I/O client
 * @client: The I/O client that the range will be deleted
 * @start: The start address of the range
 * @end: The end address of the range
 */
void bao_io_client_range_del(struct bao_io_client* client, u64 start, u64 end);

/**
 * Retrieve the oldest I/O request from the I/O client
 * @client: The I/O client
 * @req: The virtio request to be retrieved
 * @return 0 on success, <0 on failure
 */
int bao_io_client_request(struct bao_io_client* client, struct bao_virtio_request* req);

/**
 * Push an I/O request into the I/O client request list
 * @client: The I/O Client that the I/O request belongs to
 * @req: The I/O request to be pushed
 */
void bao_io_client_push_request(struct bao_io_client* client, struct bao_virtio_request* req);

/**
 * Pop an I/O request from the I/O client request list
 * @client: The I/O client that the I/O request belongs to
 * @req: The I/O request to be popped
 * @return true if the I/O request was popped, false otherwise
 */
bool bao_io_client_pop_request(struct bao_io_client* client, struct bao_virtio_request* req);

/**
 * Find the I/O client that the I/O request belongs to
 * @dm: The DM that the I/O request belongs to
 * @req: The I/O request
 * @return The I/O client that the I/O request belongs to, or NULL if there is
 * no client
 */
struct bao_io_client* bao_io_client_find(struct bao_dm* dm, struct bao_virtio_request* req);

/**
 * Initialize the Ioeventfd client
 * @dm: The DM that the Ioeventfd client belongs to
 */
int bao_ioeventfd_client_init(struct bao_dm* dm);

/**
 * Destroy the Ioeventfd client
 * @dm: The DM that the Ioeventfd client belongs to
 */
void bao_ioeventfd_client_destroy(struct bao_dm* dm);

/**
 * Configure the Ioeventfd client
 * @dm: The DM that the Ioeventfd client belongs to
 * @config: The ioeventfd configuration
 */
int bao_ioeventfd_client_config(struct bao_dm* dm, struct bao_ioeventfd* config);

/**
 * Initialize the Irqfd server
 * @dm: The DM that the Irqfd server belongs to
 */
int bao_irqfd_server_init(struct bao_dm* dm);

/**
 * Destroy the Irqfd server
 * @dm: The DM that the Irqfd server belongs to
 */
void bao_irqfd_server_destroy(struct bao_dm* dm);

/**
 * Configure the Irqfd server
 * @dm: The DM that the Irqfd server belongs to
 * @config: The irqfd configuration
 */
int bao_irqfd_server_config(struct bao_dm* dm, struct bao_irqfd* config);

/**
 * Initialize the I/O Dispatcher
 * @dm: The DM to be initialized on the I/O Dispatcher
 */
int bao_io_dispatcher_init(struct bao_dm* dm);

/**
 * Destroy the I/O Dispatcher
 * @dm: The DM to be destroyed on the I/O Dispatcher
 */
void bao_io_dispatcher_destroy(struct bao_dm* dm);

/**
 * Setup the I/O Dispatcher
 */
int bao_io_dispatcher_setup(void);

/**
 * Remove the I/O Dispatcher
 */
void bao_io_dispatcher_remove(void);

/**
 * Acquires the I/O requests from the Bao Hypervisor and dispatches them to the
 * respective I/O client
 * @dm: The DM that the I/O clients belongs to
 * @return: 0 on success, <0 on failure
 */
int bao_dispatch_io(struct bao_dm* dm);

/**
 * Pause the I/O Dispatcher
 * @dm: The DM that will be paused
 */
void bao_io_dispatcher_pause(struct bao_dm* dm);

/**
 * Resume the I/O Dispatcher
 * @dm: The DM that will be resumed
 */
void bao_io_dispatcher_resume(struct bao_dm* dm);

/**
 * Register the interrupt controller
 * @dm: The DM that the interrupt controller belongs to
 */
int bao_intc_register(struct bao_dm* dm);

/**
 * Unregister the interrupt controller
 * @dm: The DM that the interrupt controller belongs to
 */
void bao_intc_unregister(struct bao_dm* dm);

/**
 * Setup the interrupt controller handler
 * @handler: The interrupt handler
 * @dm: The DM that the interrupt controller belongs to
 */
void bao_intc_setup_handler(void (*handler)(struct bao_dm* dm));

/**
 * Remove the interrupt controller handler
 */
void bao_intc_remove_handler(void);

/**
 * I/O Dispatcher kernel module ioctls handler
 * @filp: The open file pointer
 * @cmd: The ioctl command
 * @ioctl_param: The ioctl parameter
 */
long bao_io_dispatcher_driver_ioctl(struct file* filp, unsigned int cmd, unsigned long ioctl_param);

#endif /* _BAO_H */
