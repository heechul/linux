obj-m = memsched.o

KVERSION = $(shell uname -r)
# 3.2.0rc3-custom
# 3.2.0-rc3-custom
all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
	rm *~
