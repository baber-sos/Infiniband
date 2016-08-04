#!/bin/sh
reset
sudo dmesg -C

checkModule() {
  MODULE="megaVM_client"
  if lsmod | grep "$MODULE" &> /dev/null ; then
    echo "$MODULE is loaded!"
    return 0
  else
    echo "$MODULE is not loaded!"
    return 1
  fi
}

echo "Running initialization script for client\n"
sudo ifconfig ib0 12.12.12.2/24 up
echo "Assigned IP 12.12.12.2 to ib0\n\n"

cp /usr/src/ofa_kernel/default/Module.symvers /home/xen/Downloads/client/

cd ~/Downloads/client
sudo make
echo "Recompiled the code\n"


if checkModule; then
	sudo rmmod megaVM_client.ko
	echo "Removed the module\n"
fi

echo "Loaded the module\n"
sudo insmod megaVM_client.ko

