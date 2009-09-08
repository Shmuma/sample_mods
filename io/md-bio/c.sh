#!/bin/sh

#make -C /home/shmuma/work/kernel/src/linux-2.6.29.4/ M=`pwd` clean
make -C /lib/modules/`uname -r`/build M=`pwd` clean
