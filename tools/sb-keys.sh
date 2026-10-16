#!/bin/sh
# sb-keys.sh (M22): тестовые ключи secure boot для гейта OVMF (бут D).
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
