#!/bin/sh
# sb-keys.sh (M22): тестовые ключи secure boot для гейта OVMF (бут D).
# PK/KEK/db — self-signed X509 (RSA-2048); mkinitramfs.sh подписывает ими
# лоадер и ядро (sbsign), гейт записывает их в vars virt-fw-vars'ом.
# keys/ в git не попадает, генерация один раз на песочницу; идемпотент —
# существующие ключи не трогаются (иначе подписи в build/ отваливаются).
set -e
cd "$(dirname "$0")/.."
mkdir -p keys
gen() {
    [ -f "keys/$1.crt" ] && return 0
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "keys/$1.key" -out "keys/$1.crt" \
        -days 3650 -subj "/CN=MiLin $1/"
}
gen PK
gen KEK
gen db
ls -l keys
