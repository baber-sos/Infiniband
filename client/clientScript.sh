#!/bin/sh
reset
sudo dmesg -C

checkModule() {
  MODULE="rdma_krping"
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

cd ~/Downloads/client
sudo make
echo "Recompiled the code\n"

checkModule
if $?; then
	echo "Didnt remove the module\n"
else
	sudo rmmod rdma_krping
	echo "Removed the module\n"
fi

sudo insmod rdma_krping.ko

echo "Loaded the module\n"

echo "Starting the client\n\n"
echo "client,addr=12.12.12.1,port=9999,count=100" >/proc/krping