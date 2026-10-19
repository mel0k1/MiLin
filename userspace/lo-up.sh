#!/bin/sh
# lo: поднимает петлевой интерфейс; ядро вешает 127.0.0.1/8 на NETDEV_UP
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
ip link set lo up && echo "LO UP OK"
