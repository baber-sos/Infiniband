#!/bin/sh
reset
sudo dmesg -C

checkModule() {
  MODULE="megaVM_server"
  if lsmod | grep "$MODULE" &> /dev/null ; then
    echo "$MODULE is loaded!"
    return 0
  else
    echo "$MODULE is not loaded!"
    return 1
  fi
}

echo "Running initialization script for server\n"
sudo ifconfig ib0 12.12.12.1/24 up
echo "Assigned IP 12.12.12.1 to ib0\n\n"

cp /usr/src/ofa_kernel/default/Module.symvers /home/kvmmaster1/Downloads/server/

cd /home/xen/Downloads/server
sudo make
echo "Recompiled the code\n"


if checkModule; then
	sudo rmmod megaVM_server.ko
	echo "Removed the module\n"
fi

echo "Remeber to start opemn subnet manager (opensm)\n"
echo "Waiting for connection\n"
sudo insmod megaVM_server.ko





