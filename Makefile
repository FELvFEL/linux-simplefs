ifneq ($(KERNELRELEASE),)
obj-m += simplefs.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)
CC ?= gcc

all: simplefs_test
	$(MAKE) -C $(KDIR) M="$(PWD)" modules

simplefs_test: simplefs_test.c simplefs.h
	$(CC) -Wall -Wextra -O2 -o $@ simplefs_test.c

clean:
	$(MAKE) -C $(KDIR) M="$(PWD)" clean
	rm -f simplefs_test
endif
