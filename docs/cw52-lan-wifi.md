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

* LAN address/netmask now use the separate staged migration API below; a
  fixed maintenance alias preserves the existing management route.
* The vendor `Tethering` switch also controls USB/LAN transport and is never used
  as a DHCP switch. The independent LAN DHCP gate is described below.
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

## LAN DHCP switch

`GET /api/lan` includes `dhcp.supported`, `enabled`, `saved_enabled`,
`consistent`, and a boot-scoped state `version`. `POST /api/lan` accepts
`{"action":"dhcp","enabled":false,"version":"<fresh version>","confirm":true}`.
The request parser rejects extra/duplicate keys and non-boolean switches.

The switch controls CW52's IPv4 DHCP allocation on the shared `tether` LAN
(Ethernet, WiFi and USB). Off installs only an INPUT UDP destination-port 67
gate on that interface, using its own `CW52_LAN_DHCP` chain. On removes that gate.
ConnMan, Tethering, the bridge, addresses, routes, WiFi and management listeners
remain running. Existing leases are not revoked. New clients and renewals need
another reachable DHCP server or manual configuration while off. IPv6 addressing
is outside this switch. A server-side DHCP process can still listen on UDP67:
that alone does not mean it can allocate addresses through the closed gate.

A real DHCPDISCOVER-only probe (no DHCPREQUEST or interface reconfiguration)
confirmed one offer with the gate open, none while closed, and offers again after
removal on the CW52 firmware. The filter does not block forwarded DHCP exchanges
with a different server on a genuinely bridged LAN.

Before applying, the API checks original management endpoints and creates a
private durable `dhcp-switch-*` recovery directory under
`/mnt/data/local-udx710/`. A standalone 30-second rollback script survives server
exit. Runtime rules, persistence and management reachability must pass readback
before `committed` is written. Failed changes restore the prior state; incomplete
recovery retains the shared LAN lock. Inspect the directory and run
`sh rollback.sh --now` to recover manually. Never flush INPUT or unrelated chains.
Unknown/partial/foreign rules are treated as unavailable, not overwritten.

The desired setting is stored atomically in `lan-dhcp.state` under the same
userdata directory and restored during backend startup. Absence defaults to
DHCP enabled without firewall writes. Restoring a saved disabled setting on
backend startup is supported; whole-device reboot still requires hardware
verification. Saved/runtime disagreement is explicitly shown and blocks changes.

## Staged LAN IPv4 editing

The LAN form edits the private IPv4 address, contiguous mask (/8 through /30)
and DHCP pool together. Pool-only writes retain their existing API. The fixed
maintenance address `192.168.1.1` is retained as a secondary address on `tether`
when needed. Existing SSH, ttyd and the NAS/HTTPS route keep that maintenance
address. This is not a bridge to the main router: equal prefixes alone do not
join two physical LANs. IPv6 and cellular default routes are not configured here.

`POST /api/lan` with `action:"address"` requires `address`, `netmask`,
`pool_start`, `pool_end`, a fresh `version` and `confirm:true`.
`validate_only:true` validates without writes, locks, ARP probing or migration.
Router/network/broadcast conflicts, the maintenance address inside the pool,
other local interface networks and noncanonical/extra/duplicate fields are
rejected. The real save also probes the currently reachable LAN with ARP DAD.

Recovery is prepared under userdata before applying. The old persistent file,
configurations, boot ID, deadline and independent executable rollback are saved
in `lan-address-*`. The shared LAN lock blocks overlapping mutations. The
pending pointer survives backend exit. A 120-second watchdog and confirmations
share an advisory file lock to resolve timeout races. An unconfirmed change is
rolled back; an unconfirmed transaction found after a new device boot is also
rolled back by backend initialization. Incomplete recovery retains the pointer
and lock for inspection instead of claiming success.

After applying, open `http://<new IPv4>:6677/`, log in and choose **Keep new
address**. `action:"confirm_address"` requires the pending `id` and confirmation.
The accepted TCP socket must actually terminate at the new IPv4, independently
of Host/forwarding headers, and runtime configuration and maintenance endpoints
must still agree. Confirming through the old reverse-proxy target is rejected.
The UI displays a countdown and polls only while a migration is pending.

Install the packaged executable `cw52-lan-recovery.sh` with server/dist. The
managed CW52 boot entry invokes it after the original USB initialization; it
waits for ConnMan's configured LAN and adds only the missing maintenance alias.
It never flushes or replaces addresses. Backend initialization and pool saves
also preserve the alias and reconcile only the tagged new-subnet 6677 rule.
Rollback removes only its tagged rule and restores the previous subnet rule;
LAN DHCP's saved gate is preserved. Never use Tethering as an address switch.

Tests exercise private/mask/pool schemas, destination-socket confirmation,
maintenance-only alias restoration, and generated rollback with fake network
tools plus real filesystem locks (including failed setter and confirm/timeout
races). Production installation and validation do not imply a real address
migration or a device reboot has been performed.

Tagged source-NAT rules cover only the selected private LAN and the retained
maintenance subnet, and only outbound cellular `sipa_eth+` interfaces. They are
reconciled on apply/startup and restored on rollback without flushing vendor NAT
chains or changing the cellular default route. A tagged 6677 allow rule also
requires ingress on `tether`, so a matching cellular-side source cannot use it.
