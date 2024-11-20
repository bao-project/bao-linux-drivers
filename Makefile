# Declare targets as phony to ensure they always run
.PHONY: iodispatcher ipc clean help

# Target for building only the iodispatcher module
iodispatcher:
	$(MAKE) -C $(KERN_DIR) M=$(PWD)/iodispatcher CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) modules

# Target for building only the ipc module
ipc:
	$(MAKE) -C $(KERN_DIR) M=$(PWD)/ipc CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) modules

# Target for building all modules
all: iodispatcher ipc

# Target for cleaning 
clean:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) clean

# Target for help
help:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) help
