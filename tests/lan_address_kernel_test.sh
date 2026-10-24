#!/bin/bash
# Runner-only network namespace: no production interfaces or routes are touched.
set -euo pipefail
ns="cw52-lan-$RANDOM-$$"
trap 'ip netns del "$ns" >/dev/null 2>&1 || true' EXIT
ip netns add "$ns"
ip -n "$ns" link add sipa_eth0 type dummy
ip -n "$ns" link add tether type bridge
ip -n "$ns" link set sipa_eth0 up
ip -n "$ns" link set tether up
ip -n "$ns" address add 10.197.77.223/8 dev sipa_eth0
ip -n "$ns" address add 192.168.1.1/24 dev tether
ip -n "$ns" route add default dev sipa_eth0
ip -n "$ns" address replace 10.0.0.254/24 dev tether
ip -n "$ns" -4 route get 10.0.0.240 | grep -q 'dev tether'
ip -n "$ns" -4 route get 8.8.8.8 | grep -q 'dev sipa_eth0'
ip -n "$ns" -4 address show dev tether | grep -q '192.168.1.1/24'
ip -n "$ns" -4 address show dev tether | grep -q '10.0.0.254/24'
ip -n "$ns" address del 10.0.0.254/24 dev tether
ip -n "$ns" -4 route get 10.0.0.240 | grep -q 'dev sipa_eth0'
ip -n "$ns" -4 address show dev tether | grep -q '192.168.1.1/24'
echo "LAN /24 overrides cellular /8; maintenance address and cellular default survive"
