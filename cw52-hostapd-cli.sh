#!/bin/sh
# CW52 hostapd RELOAD uses cached configuration; SIGHUP re-reads its file.
# All status/client/control requests retain the vendor CLI behavior.
CLI=/usr/sbin/hostapd_cli
if [ "$#" -eq 5 ] && [ "$1" = -p ] && [ "$2" = /var/run/hostapd ] && [ "$3" = -i ]; then
    case "$5" in
        reload|enable)
            case "$4" in
                wlan1) name=hostapd ;;
                wlan0) name=hostapd_5g ;;
                *) echo FAIL; exit 1 ;;
            esac
            pid=$(pidof "$name")
            case "$pid" in ''|*[!0-9]*) echo FAIL; exit 1 ;; esac
            [ "$(readlink "/proc/$pid/exe")" = "/usr/sbin/$name" ] || { echo FAIL; exit 1; }
            kill -HUP "$pid" || { echo FAIL; exit 1; }
            if [ "$5" = reload ]; then echo OK; exit 0; fi
            sleep 1
            ;;
    esac
fi
exec "$CLI" "$@"
