KSRC=/lib/modules/`uname -r`/build
KOBJ=/lib/modules/`uname -r`/build

ccflags-y=-I/usr/src/ofa_kernel/default/include/

obj-m += megaVM_server.o

default:
	make -C $(KSRC) M=`pwd` modules

install:
	make -C $(KSRC) M=`pwd` modules_install
	depmod -a

clean:
	rm -f *.o
	rm -f *.ko
	rm -f megaVM_server.mod.c
	rm -f modules.order 
	rm -f Module.markers
