#!/bin/sh
# Move only the locally managed ttyd; never restart ConnMan or Tethering.
set -eu
umask 077
D=/mnt/data/local-ttyd
CONFIG=/var/mm/dhcp_config.ini
RUN=/run
PORT=7681
[ ! -e "$D/disabled" ] || exit 1
[ -x "$D/ttyd" ] || exit 1
ADDR=$(sed -n 's/^gateway_ip=//p' "$CONFIG")
printf '%s\n' "$ADDR" | awk -F. 'NR!=1 {exit 1} NF!=4 {exit 1} {for(i=1;i<=4;i++) if($i !~ /^[0-9]+$/ || length($i)>3 || $i+0>255 || (length($i)>1 && substr($i,1,1)=="0")) exit 1}' || exit 1
LOCK=$RUN/local-ttyd-$PORT.lock
PIDFILE=$RUN/local-ttyd-$PORT.pid
mkdir "$LOCK" 2>/dev/null || exit 1
trap 'rmdir "$LOCK" 2>/dev/null || :' EXIT
EXE=$(readlink -f "$D/ttyd")
owned() {
 case "$1" in ''|*[!0-9]*) return 1;; esac
 [ "$1" -gt 1 ] && [ "$(readlink /proc/$1/exe 2>/dev/null)" = "$EXE" ]
}
ready() { curl --noproxy '*' -fsS --connect-timeout 1 --max-time 1 -o /dev/null "http://$ADDR:$PORT/"; }
if [ -r "$PIDFILE" ]; then
 read PID < "$PIDFILE"
 case "$PID" in ''|*[!0-9]*) exit 1;; esac
 [ "$PID" -gt 1 ] || exit 1
 if kill -0 "$PID" 2>/dev/null; then
  owned "$PID" || exit 1
  if tr '\000' '\n' < "/proc/$PID/cmdline" | grep -Fxq -- "$ADDR" && ready; then exit 0; fi
  kill "$PID"
  N=0
  while owned "$PID" && [ "$N" -lt 5 ]; do sleep 1; N=$((N+1)); done
  ! owned "$PID" || exit 1
  rm -f "$PIDFILE"
 fi
fi
"$D/ttyd" -W -i "$ADDR" -p "$PORT" /bin/bash </dev/null >/dev/null 2>&1 &
PID=$!
N=0
while [ "$N" -lt 5 ]; do
 sleep 1
 if ! owned "$PID"; then exit 1; fi
 if ready; then
  printf '%s\n' "$PID" > "$PIDFILE.rebind"
  mv "$PIDFILE.rebind" "$PIDFILE"
  cat /proc/sys/kernel/random/boot_id > "$RUN/local-ttyd-$PORT.boot-id"
  exit 0
 fi
 N=$((N+1))
done
if owned "$PID"; then kill "$PID"; fi
exit 1
