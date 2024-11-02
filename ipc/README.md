# Bao Hypervisor IPC Through Shared-memory Kernel Module

The IPC kernel module enables communication between two VMs via a mutually established shared memory region.

### Setup instructions

1. Setup global environment variables:
```
export ARCH=arm64|arm32|riscv
export CROSS_COMPILE=path/to/your/toolchain
export KERN_DIR=path/to/your/linux
```

2. Build the out of tree kernel module:
```
make ipc
```

3. Copy the `bao_ipc.ko` file to your target filesystem.

### Run instructions

1. When the Backend VM boots up, insert the kernel module:
```
insmod bao_ipc.ko
```

2. From now on, you should be able to see the `/dev/baoipcX` device node if configured any Bao IPC object.