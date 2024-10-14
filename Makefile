# Object files and module definition
obj-m += bao_iodispatcher.o
bao_iodispatcher-y := dm.o driver.o intc.o io_client.o io_dispatcher.o ioctls.o ioeventfd.o irqfd.o

# Targets for building, cleaning, and help
host:
	make -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) modules

clean:
	make -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) clean

help:
	make -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) help
