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

echo "Running initialization script for server\n"
sudo ifconfig ib0 12.12.12.1/24 up
echo "Assigned IP 12.12.12.1 to ib0\n\n"

cd /home/xen/Desktop/krping_compilable_server
sudo make
echo "Recompiled the code\n"

checkModule
if $?; then
	echo "Didnt remove the module\n"
else
	sudo rmmod rdma_krping
	echo "Removed the module\n"
fi
echo "Remeber to start opemn subnet manager (opensm)\n"
echo "Waiting for connection\n"
sudo insmod rdma_krping.ko





