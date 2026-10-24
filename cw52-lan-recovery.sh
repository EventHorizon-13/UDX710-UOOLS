#!/bin/sh
# Restore the fixed management alias without flushing or replacing any address.
# --boot waits for ConnMan's LAN bridge to acquire its configured IPv4 first.
set -eu
n=0
limit=1
[ "${1:-}" != --boot ] || limit=120
while [ "$n" -lt "$limit" ]; do
    addresses=$(ip -4 -o address show dev tether 2>/dev/null || :)
    if printf '%s\n' "$addresses" | grep -q 'inet 192\.168\.1\.1/'; then exit 0; fi
    if printf '%s\n' "$addresses" | grep -q 'inet '; then
        ip -4 address add 192.168.1.1/24 dev tether 2>/dev/null || :
        ip -4 -o address show dev tether | grep -q 'inet 192\.168\.1\.1/' && exit 0
    fi
    n=$((n+1))
    [ "$n" -ge "$limit" ] || sleep 1
done
exit 1
