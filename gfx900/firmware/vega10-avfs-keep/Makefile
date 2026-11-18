obj-m := vega10_avfs_keep.o
KVER ?= $(shell uname -r)
all:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR) modules
clean:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR) clean
