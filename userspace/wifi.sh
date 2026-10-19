#!/bin/sh
# wifi: поднимает wlan* — wpa_supplicant + udhcpc (M25)
# hwsim-стенд: первый wlan* — AP (wpa_supplicant mode=2), второй — STA;
# DHCP-клиент проверяется на eth* против slirp (в hwsim-паре сервера нет).
# Реальное железо: один wlan* = STA, адрес по DHCP от точки доступа.
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
CT=/run/wpa_supplicant
mkdir -p $CT /var/run

# ping с потолком: зависший ping не блокирует сервис (M26)
ping_guard() {
    t="$1"; shift
    ping "$@" >/tmp/ping-g.log 2>&1 &
    p=$!
    i=0
    while kill -0 $p 2>/dev/null && [ $i -lt $t ]; do sleep 1; i=$((i + 1)); done
    if kill -0 $p 2>/dev/null; then
        kill -9 $p; wait $p
        return 99
    fi
    wait $p
}

AP=""
STA=""
for d in /sys/class/net/wlan*; do
    [ -e "$d" ] || exit 0
    w="$(basename "$d")"
    ifconfig "$w" up 2>/dev/null
    if [ -z "$AP" ]; then AP="$w"; else [ -z "$STA" ] && STA="$w"; fi
done
[ -n "$AP" ] || exit 0

if [ -z "$STA" ]; then
    # реальное железо: единственный wlan* работает станцией
    STA="$AP"
    AP=""
    wpa_supplicant -B -p $CT -i "$STA" -c /etc/wpa_supplicant/wpa_supplicant.conf
    i=0
    while [ $i -lt 30 ]; do
        st="$(wpa_cli -p $CT -i "$STA" status 2>/dev/null | grep wpa_state | cut -d= -f2)"
        [ "$st" = "COMPLETED" ] && break
        sleep 1; i=$((i + 1))
    done
    [ "$st" = "COMPLETED" ] && echo "WIFI E2E OK assoc $STA"
    udhcpc -i "$STA" -s /sbin/udhcpc.script -n -q -t 8 \
        && echo "WIFI E2E OK dhcp $STA"
    # ping реального шлюза (по маршруту от DHCP)
    gw="$(ip route show default 2>/dev/null | grep default | cut -d' ' -f3)"
    [ -n "$gw" ] && ping_guard 12 -c 3 -W 3 "$gw" \
        && echo "WIFI E2E OK ping $STA"
    exit 0
fi

# hwsim: AP-сторона со статикой, STA ассоциируется по WPA2-PSK
cat > /etc/wpa_supplicant/ap.conf <<EOF
ctrl_interface=$CT
network={
  mode=2
  frequency=2412
  ssid="MiLin-AP"
  psk="milinpass"
  key_mgmt=WPA-PSK
  pairwise=CCMP
}
EOF
wpa_supplicant -B -p $CT -i "$AP" -c /etc/wpa_supplicant/ap.conf
ip addr add 192.168.77.1/24 dev "$AP" 2>/dev/null
# DHCP-сервер на AP-стороне (ping-check dnsmasq = RAW-ICMP тест ядра)
dnsmasq --no-daemon --interface="$AP" --bind-dynamic \
    --dhcp-range=192.168.77.10,192.168.77.20,255.255.255.0,12h \
    --dhcp-authoritative --no-hosts --no-resolv \
    --dhcp-leasefile=/var/run/dnsmasq.leases &
sleep 2

wpa_supplicant -B -p $CT -i "$STA" -c /etc/wpa_supplicant/wpa_supplicant.conf
i=0
st=""
while [ $i -lt 30 ]; do
    st="$(wpa_cli -p $CT -i "$STA" status 2>/dev/null | grep wpa_state | cut -d= -f2)"
    [ "$st" = "COMPLETED" ] && break
    sleep 1; i=$((i + 1))
done
if [ "$st" = "COMPLETED" ]; then
    echo "WIFI E2E OK assoc $STA"
    ip addr add 192.168.77.2/24 dev "$STA" 2>/dev/null
    udhcpc -i "$STA" -s /sbin/udhcpc.script -t 6 -T 3 -n -q >/tmp/udh.log 2>&1 \
        && echo "WIFI E2E OK dhcp $STA"
    ping_guard 12 -c 3 -W 3 192.168.77.1 && echo "WIFI E2E OK ping $STA"
else
    echo "wifi: STA $STA не ассоциировался за 30с"
fi

# DHCP-клиент (udhcpc + хук) — на eth* против slirp/роутера
for d in /sys/class/net/eth*; do
    [ -e "$d" ] || continue
    e="$(basename "$d")"
    ifconfig "$e" up 2>/dev/null
    udhcpc -i "$e" -s /sbin/udhcpc.script -n -q -t 8 \
        && echo "WIFI E2E OK dhcp $e"
    ping_guard 12 -c 3 -W 3 10.0.2.2 && echo "WIFI E2E OK ping $e"
done

# сервис живёт до остановки супервизора (stop=term)
while :; do sleep 60; done
