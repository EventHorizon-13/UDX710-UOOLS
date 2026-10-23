/* All station records and addresses use growable containers; no list-size cap.
 * Only associated stations form the client list. Leases/neighbors enrich it;
 * stale leases never create phantom wireless clients. No DNS/network lookups. */
#include <gio/gio.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include "wifi_clients.h"

typedef struct {
  char mac[18];
  char *hostname;
  GPtrArray *ipv4, *ipv6;
} WifiClient;

static int valid_mac(const char *s) {
  if (!s || strlen(s) != 17) return 0;
  for (int i=0;i<17;i++) if ((i%3==2) ? s[i]!=':' : !g_ascii_isxdigit(s[i])) return 0;
  return 1;
}

static WifiClient *client_find(GPtrArray *clients, const char *mac) {
  if (!valid_mac(mac)) return NULL;
  for (guint i=0;i<clients->len;i++) {
    WifiClient *c = g_ptr_array_index(clients,i);
    if (!g_ascii_strcasecmp(c->mac,mac)) return c;
  }
  return NULL;
}

static void client_free(gpointer p) {
  WifiClient *c = p; g_free(c->hostname); g_ptr_array_free(c->ipv4,TRUE);
  g_ptr_array_free(c->ipv6,TRUE); g_free(c);
}

static WifiClient *client_add(GPtrArray *clients, const char *mac) {
  WifiClient *c = client_find(clients,mac);
  if (c || !valid_mac(mac)) return c;
  c = g_new0(WifiClient,1);
  for (int i=0;i<18;i++) c->mac[i] = g_ascii_tolower(mac[i]);
  c->ipv4 = g_ptr_array_new_with_free_func(g_free); c->ipv6 = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(clients,c); return c;
}

static void client_address(WifiClient *c, const char *s) {
  if (!c || !s) return;
  unsigned char raw[16]; char canonical[INET6_ADDRSTRLEN];
  int family = inet_pton(AF_INET,s,raw)==1 ? AF_INET : (inet_pton(AF_INET6,s,raw)==1 ? AF_INET6 : 0);
  if (!family) return;
  inet_ntop(family,raw,canonical,sizeof(canonical));
  if (!strcmp(canonical,"0.0.0.0") || !strcmp(canonical,"::")) return;
  GPtrArray *addresses = family==AF_INET ? c->ipv4 : c->ipv6;
  for (guint i=0;i<addresses->len;i++) if (!strcmp(g_ptr_array_index(addresses,i),canonical)) return;
  g_ptr_array_add(addresses,g_strdup(canonical));
}

static void client_hostname(WifiClient *c, const char *s) {
  if (!c || c->hostname || !s || !*s || !strcmp(s,"*") || !strcmp(s,"(none)") || !g_utf8_validate(s,-1,NULL)) return;
  for (const char *p=s;*p;p++) if ((unsigned char)*p < 32 || (unsigned char)*p==127) return;
  c->hostname = g_strdup(s);
}

static char **tokens(const char *s, guint *count) {
  char **split = g_strsplit_set(s," \t\r\n",-1);
  GPtrArray *out = g_ptr_array_new();
  for (char **p=split;*p;p++) if (**p) g_ptr_array_add(out,g_strdup(*p));
  *count = out->len; g_ptr_array_add(out,NULL); g_strfreev(split);
  return (char **)g_ptr_array_free(out,FALSE);
}

static void parse_neighbor(GPtrArray *clients, const char *line, const char *iface) {
  guint n; char **p = tokens(line,&n);
  const char *mac=NULL, *dev=NULL;
  for (guint i=1;i+1<n;i++) {
    if (!strcmp(p[i],"lladdr")) mac=p[i+1];
    if (!strcmp(p[i],"dev")) dev=p[i+1];
  }
  if (n && dev && (!strcmp(dev,"tether") || !strcmp(dev,iface))) client_address(client_find(clients,mac),p[0]);
  g_strfreev(p);
}

static void parse_lease(GPtrArray *clients, const char *line, int connman) {
  guint n; char **p = tokens(line,&n);
  if (n>=4 && valid_mac(p[1])) {
    int current = 1;
    if (!connman) {
      char *end; gint64 expiry = g_ascii_strtoll(p[0],&end,10);
      current = *p[0] && !*end && (expiry==0 || expiry >= time(NULL));
    }
    if (current) {
      WifiClient *c = client_find(clients,p[1]);
      client_address(c,p[2]); client_hostname(c,p[3]);
    }
  }
  g_strfreev(p);
}

static int read_stations(GPtrArray *clients, const char *iface) {
  const char *cmd = !strcmp(iface,"wlan0") ?
    "/usr/sbin/hostapd_cli -p /var/run/hostapd -i wlan0 all_sta 2>/dev/null" :
    "/usr/sbin/hostapd_cli -p /var/run/hostapd -i wlan1 all_sta 2>/dev/null";
  FILE *f=popen(cmd,"r"); if (!f) return 0;
  char *line=NULL; size_t cap=0; WifiClient *current=NULL; int ok=1;
  while (getline(&line,&cap,f)>=0) {
    g_strchomp(line);
    if (valid_mac(line)) current=client_add(clients,line);
    else if (!strcmp(line,"FAIL") || strstr(line,"Could not connect")) ok=0;
    else if (g_str_has_prefix(line,"ip_address=")) client_address(current,line+11);
    else if (g_str_has_prefix(line,"hostname=")) client_hostname(current,line+9);
  }
  if (ferror(f)) ok=0;
  int status=pclose(f); if (status==-1 || !WIFEXITED(status) || WEXITSTATUS(status)) ok=0;
  free(line); return ok;
}

static int read_neighbors(GPtrArray *clients, const char *iface, const char *command) {
  FILE *f=popen(command,"r"); if (!f) return 0;
  char *line=NULL; size_t cap=0;
  while (getline(&line,&cap,f)>=0) parse_neighbor(clients,line,iface);
  int ok=!ferror(f); int status=pclose(f); free(line);
  return ok && status!=-1 && WIFEXITED(status) && !WEXITSTATUS(status);
}

static void read_arp(GPtrArray *clients) {
  FILE *f=fopen("/proc/net/arp","r"); if (!f) return;
  char *line=NULL; size_t cap=0;
  while (getline(&line,&cap,f)>=0) {
    guint n; char **p=tokens(line,&n);
    if (n>=6 && !strcmp(p[5],"tether") && (strtoul(p[2],NULL,0)&2)) client_address(client_find(clients,p[3]),p[0]);
    g_strfreev(p);
  }
  free(line); fclose(f);
}

static int read_connman_leases(GPtrArray *clients) {
  GError *error=NULL;
  GDBusConnection *bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,&error);
  if (!bus) { g_clear_error(&error); return 0; }
  GVariant *r=g_dbus_connection_call_sync(bus,"net.connman","/","net.connman.Manager",
    "GetTetheringClients",NULL,G_VARIANT_TYPE("(as)"),G_DBUS_CALL_FLAGS_NONE,2000,NULL,&error);
  if (r) {
    GVariant *arr=g_variant_get_child_value(r,0); GVariantIter it; const char *line;
    g_variant_iter_init(&it,arr);
    while (g_variant_iter_next(&it,"&s",&line)) parse_lease(clients,line,1);
    g_variant_unref(arr); g_variant_unref(r);
  }
  int ok=r!=NULL; g_object_unref(bus); g_clear_error(&error); return ok;
}

static void read_lease_files(GPtrArray *clients) {
  const char *paths[]={"/var/lib/misc/dnsmasq.leases","/var/lib/connman/dhcp.leases","/run/dnsmasq.leases","/tmp/dhcp.leases"};
  for (size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
    FILE *f=fopen(paths[i],"r"); if (!f) continue;
    char *line=NULL; size_t cap=0;
    while (getline(&line,&cap,f)>=0) parse_lease(clients,line,0);
    free(line); fclose(f);
  }
}

static void read_hosts(GPtrArray *clients) {
  FILE *f=fopen("/etc/hosts","r"); if (!f) return;
  char *line=NULL; size_t cap=0;
  while (getline(&line,&cap,f)>=0) {
    char *comment=strchr(line,'#'); if (comment) *comment=0;
    guint n; char **p=tokens(line,&n);
    if (n>=2) for (guint i=0;i<clients->len;i++) {
      WifiClient *c=g_ptr_array_index(clients,i);
      for (int family=0;family<2;family++) {
        GPtrArray *ips=family ? c->ipv6 : c->ipv4;
        unsigned char raw[16]; char canonical[INET6_ADDRSTRLEN];
        int af=family ? AF_INET6 : AF_INET;
        if (inet_pton(af,p[0],raw)!=1) continue;
        inet_ntop(af,raw,canonical,sizeof(canonical));
        for (guint a=0;a<ips->len;a++) if (!strcmp(g_ptr_array_index(ips,a),canonical)) client_hostname(c,p[1]);
      }
    }
    g_strfreev(p);
  }
  free(line); fclose(f);
}

void wifi_clients_json(JsonBuilder *j, const char *iface) {
  GPtrArray *clients=g_ptr_array_new_with_free_func(client_free);
  int stations=read_stations(clients,iface);
  int ipv4=read_neighbors(clients,iface,"ip -4 neigh show 2>/dev/null");
  int ipv6=read_neighbors(clients,iface,"ip -6 neigh show 2>/dev/null");
  read_arp(clients); int leases=read_connman_leases(clients);
  read_lease_files(clients); read_hosts(clients);
  json_add_bool(j,"clients_complete",stations);
  json_arr_open(j,"client_warnings");
  if (!stations) json_arr_add_str(j,"hostapd station list unavailable");
  if (!ipv4) json_arr_add_str(j,"IPv4 neighbor table unavailable");
  if (!ipv6) json_arr_add_str(j,"IPv6 neighbor table unavailable");
  if (!leases) json_arr_add_str(j,"ConnMan lease table unavailable");
  json_arr_close(j);
  json_arr_open(j,"clients");
  for (guint i=0;i<clients->len;i++) {
    WifiClient *c=g_ptr_array_index(clients,i);
    json_arr_obj_open(j); json_add_str(j,"mac",c->mac); json_add_str(j,"hostname",c->hostname ? c->hostname : "");
    json_add_str(j,"interface",iface); json_add_str(j,"band",!strcmp(iface,"wlan0") ? "5 GHz" : "2.4 GHz");
    json_arr_open(j,"ipv4"); for (guint a=0;a<c->ipv4->len;a++) json_arr_add_str(j,g_ptr_array_index(c->ipv4,a)); json_arr_close(j);
    json_arr_open(j,"ipv6"); for (guint a=0;a<c->ipv6->len;a++) json_arr_add_str(j,g_ptr_array_index(c->ipv6,a)); json_arr_close(j);
    json_obj_close(j);
  }
  json_arr_close(j); g_ptr_array_free(clients,TRUE);
}
