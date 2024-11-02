# Declare targets as phony to ensure they always run
.PHONY: io_dispatcher clean help

# Target for building only the io_dispatcher module
io_dispatcher:
	$(MAKE) -C $(KERN_DIR) M=$(PWD)/io_dispatcher CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) modules

# Target for cleaning 
clean:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) clean

# Target for help
help:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) help
