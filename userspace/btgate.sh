#!/bin/sh
# btgate: btvhid (виртуальный контроллер) + bttest (HCI-сокет e2e) (M27)
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
[ -e /dev/vhci ] || { echo "BT FAIL no vhci"; exit 1; }
/sbin/btvhid &
BPID=$!
sleep 1
/sbin/bttest
RC=$?
kill $BPID 2>/dev/null
exit $RC
