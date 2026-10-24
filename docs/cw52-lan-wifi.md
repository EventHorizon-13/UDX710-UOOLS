# CW52 LAN and wireless management

Confirmed on CW52's Yocto/Linux 4.14 firmware. This is not the SZ50/CP100 LAN implementation.

## LAN capability boundary

`GET /api/lan` reads the vendor ConnMan `net.connman.Technology.GetProperties`
at `/net/connman/technology/gadget`. `TetheringDHCP` is a string in the form
`gateway,mask,start,end,ttl`. Both the running device and `libmumu.so`'s
`mm_platform_dhcp_set_ip` confirm that the original setter uses this property.
Persistent configuration is `/var/mm/dhcp_config.ini`, group `global`.
The original port-80 `changeDhcpCfg` submits `lan_ipaddr`, `dhcpStart`, `dhcpEnd`.

The compact LAN section supports **DHCP pool start and end**. It displays the
actual LAN address/prefix, netmask and system DNS resolvers as read-only values.

Deliberately omitted write controls:

* LAN address/netmask: existing ttyd binds `192.168.1.1` and 6677's access chain
  allows `192.168.1.0/24`. A subnet migration would also require changes to those
  independently maintained entry points and the NAS management route. This API
  rejects address/mask fields and preserves both in every write.
* DHCP enable/disable: this firmware exposes `Tethering`, which also controls the
  USB/LAN transport; no independently confirmed DHCP-only switch was found.
* Lease time: the only vendor field is `ttl=0`; its meaning/unit and treatment of
  zero were not established. It is preserved verbatim and not labeled lease time.
* Default gateway: the default route is cellular `default dev sipa_eth0 scope
  link`, managed by oFono/ConnMan. It is not a LAN next-hop setting.
* DNS editing: no confirmed DHCP DNS setter accompanies `TetheringDHCP`.
  `/etc/resolv.conf` describes system resolvers, not necessarily DHCP option 6.

## Save and recovery

`POST /api/lan` accepts exactly `pool_start`, `pool_end`, `version`, `confirm`
and optional `validate_only`. Unknown/duplicate fields, noncanonical IPv4,
wrong subnets, network/broadcast/router addresses, reversed pools, stale state
and absent confirmation are rejected before mutation. `validate_only:true`
performs harmless validation without creating backups, locks or applying settings.

A save checks persistent/live state agreement, the existing `tether` address and
the original port 80 / ttyd 7681 management endpoints. It verifies `/mnt/data`
is a separate mounted filesystem, then creates a private recovery directory:
`/mnt/data/local-udx710/lan-XXXXXX/`.

Before any network write, `dhcp_config.ini.before` and an executable
`rollback.sh` are fsynced there. A separate shell watchdog is launched before
the temporary configuration replaces the original file atomically. Ownership
and permissions are retained. The vendor property is set through D-Bus and
read back; the unchanged management address and endpoints must still work.
Only then is `committed` written atomically. Failure immediately reapplies the
old property and file; a 30-second watchdog also restores them if the server
exits or does not commit. Recovery leaves `/run/udx710-lan.lock` locked to prevent
overlapping retries. After inspecting recovery, run `sh <directory>/rollback.sh
--now`, verify management, then remove that lock. Backups are never auto-deleted.

WiFi configuration also uses fsynced temporary files, preserved ownership/mode
and recovery copies. An enabled AP must return the requested SSID/channel and
ENABLED status after reload, otherwise its previous configuration is restored.

## Wireless clients

The station list is streamed from `hostapd_cli all_sta` for each actual wireless
interface (`wlan1` 2.4 GHz, `wlan0` 5 GHz). Growable records are enriched by:

* IPv4 and IPv6 `ip neigh` on the LAN bridge/wireless interface;
* valid `/proc/net/arp` entries;
* the vendor `net.connman.Manager.GetTetheringClients` string array on `/`;
* local dnsmasq-compatible lease files when present and unexpired;
* `/etc/hosts` names for discovered addresses and any hostapd-provided names.

Only currently listed hostapd stations become clients; historical leases do not
create extra stations. Addresses are canonicalized and deduplicated, with no
fixed station-count or per-client address-count cap. Unavailable hostname/IP
values render as `—`. Source failures appear in `client_warnings`; a failed
station query is never presented as a reliable zero-client count. No external
lookup, Internet request, reverse DNS or active client probing is used.

## Validation limits

Build and C sanitizer tests run only in GitHub Actions. Fixtures cover invalid
LAN payloads, protected fields, duplicate keys, stale-style inputs, multiple
IPv4/IPv6 records, lease matching, long hostnames and 601 station records.
Deployment verification uses GETs, anonymous-write rejection, malformed payloads
and `validate_only`. Real LAN/DHCP/WiFi writes and reboot are deliberately not
performed during this maintenance run.

## CW52 configuration reload fix (2026-10-24)

The vendor CLI does not support `reconfigure`. Its `reload` command returns OK
but reuses the in-memory configuration instead of reading the changed file.
The packaged `cw52-hostapd-cli.sh` adapter sends SIGHUP to the unique hostapd
process whose executable matches the requested radio. SIGHUP reads the file;
status/client/disable requests still use the vendor CLI. Enabling a radio first
loads any configuration saved while it was disabled. Missing/ambiguous processes,
wrong executables and signal failures fail closed. No service is restarted.

Deploy `6677/cw52-hostapd-cli.sh` alongside `6677/server` at
`/home/root/6677/cw52-hostapd-cli.sh`, with executable permissions. The Actions
artifact includes both files. Manual backend-only upgrades must install the
adapter too. The device's earlier local repair uses the equivalent adapter at
`/mnt/data/6677-wifi`; this source build uses the bundled path.

A real 2.4 GHz SSID change and restoration via the authenticated API were verified
against the running hostapd state. New-password association, 5 GHz changes,
DFS switching, save-while-disabled and reboot were not verified on hardware.
The Python adapter tests mock process discovery, executable lookup, signals,
waits and the vendor CLI; they do not touch a running access point.
WiFi settings call hostapd directly; LAN DHCP calls ConnMan directly. Neither
setter calls mm_app_service, but stopping that service was not tested.
