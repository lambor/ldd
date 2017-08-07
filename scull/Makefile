# Comment/uncomment the following line to disable/enable debugging
DEBUG = y

# Add your debugging flag (or not) to ccflags.
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSCULL_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

ccflags-y += $(DEBFLAGS)

ifneq ($(KERNELRELEASE),)
	scull-objs := main.o pipe.o access.o
	obj-m := scull.o

else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif


clean:
	rm -rf *.o *.ko *.order *.symvers *.mod.c
