#!/bin/bash

# source: https://gist.github.com/mintsuki/2079737d76f30c7dae1e8cb791325082

set -e

ARCH="$1"
INTERVAL="$2"
CMDLINE="$3"
KERNEL="$4"

if [ -z "$ARCH" ]; then
    printf "Usage: $0 [QEMU architecture] [sample interval in seconds] [QEMU cmdline] [kernel ELF file]\n"
    exit 1
fi

if [ -z "$INTERVAL" ]; then
    INTERVAL=1
fi

SOCKET="$(mktemp)"
OUTFILE="$(mktemp)"
SUBSHELL="$(mktemp)"

trap "rm -f '$SOCKET' '$OUTFILE' '$SUBSHELL'" EXIT INT TERM HUP

(
echo $BASHPID >"$SUBSHELL"
while true; do
    echo "info registers" >>"$SOCKET"
    sleep $INTERVAL
done
) &

tail -f "$SOCKET" | qemu-system-"$ARCH" $CMDLINE -monitor stdio \
    | grep -o '[RE]IP=[0-9a-f]*' \
    | sed 's/[RE]IP=//g' \
    | sort | uniq -c | sort -nr >"$OUTFILE"

kill $(cat "$SUBSHELL")

ISLINENUM=1
for p in $(cat "$OUTFILE"); do
    if [ $ISLINENUM = 1 ]; then
        ISLINENUM=0
        echo -ne "$p\t"
        continue
    fi

    if ! [ -z "$KERNEL" ]; then
        addr2line -e "$KERNEL" "0x$p" | tr -d '\n'
        echo " (0x$p)"
    else
        echo "(0x$p)"
    fi

    ISLINENUM=1
done
