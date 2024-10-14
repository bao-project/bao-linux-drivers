# Bao Hypervisor I/O Dispatcher Kernel Module

The I/O Dispatcher is a Linux kernel module that is loaded in the VirtIO backend VMs.
It is responsible for establishing the connection between the Remote I/O system and the Frontend Device Model and backend devices, offering a unified API to support various VirtIO backends.

### Setup instructions

1. Setup global environment variables:
```
export ARCH=arm64|arm32|riscv
export CROSS_COMPILE=path/to/your/toolchain
export KERN_DIR=path/to/your/linux
```

2. Build the out of tree kernel module:
```
make host
```

3. Copy the `bao_iodispatcher.ko` file to your target filesystem.

### Run instructions

1. When the Backend VM boots up, insert the kernel module:
```
insmod bao_iodispatcher.ko
```

2. From now on, you should be able to see the `/dev/bao-io-dispatcher` device node.