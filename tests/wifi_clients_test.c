#include <assert.h>
#include "../src/system/wifi_clients.c"

int main(void) {
  GPtrArray *all=g_ptr_array_new_with_free_func(client_free);
  WifiClient *c=client_add(all,"AA:BB:CC:DD:EE:FF");
  assert(c && !strcmp(c->mac,"aa:bb:cc:dd:ee:ff"));
  assert(client_add(all,"aa:bb:cc:dd:ee:ff")==c && all->len==1);
  assert(!client_add(all,"bad-mac"));
  parse_neighbor(all,"192.168.1.20 dev tether lladdr aa:bb:cc:dd:ee:ff REACHABLE","wlan0");
  parse_neighbor(all,"192.168.1.21 dev wlan0 lladdr AA:BB:CC:DD:EE:FF STALE","wlan0");
  parse_neighbor(all,"fe80::1 dev tether lladdr aa:bb:cc:dd:ee:ff STALE","wlan0");
  parse_neighbor(all,"240e:0000:1::a dev tether lladdr aa:bb:cc:dd:ee:ff STALE","wlan0");
  parse_neighbor(all,"240e:0:1::a dev tether lladdr aa:bb:cc:dd:ee:ff REACHABLE","wlan0");
  parse_neighbor(all,"192.168.1.80 dev tether FAILED","wlan0");
  parse_neighbor(all,"192.168.1.81 dev sipa_eth0 lladdr aa:bb:cc:dd:ee:ff REACHABLE","wlan0");
  assert(c->ipv4->len==2 && c->ipv6->len==2);
  parse_lease(all,"rndis aa:bb:cc:dd:ee:ff 192.168.1.22 a-very-long-phone-hostname 123",1);
  assert(c->ipv4->len==3 && !strcmp(c->hostname,"a-very-long-phone-hostname"));
  parse_lease(all,"1 aa:bb:cc:dd:ee:ff 192.168.1.90 expired *",0);
  assert(c->ipv4->len==3);
  parse_lease(all,"0 aa:bb:cc:dd:ee:ff 192.168.1.23 * *",0);
  parse_lease(all,"0 11:22:33:44:55:66 192.168.1.24 unknown *",0);
  assert(c->ipv4->len==4 && all->len==1);
  for(int i=1;i<=600;i++) {
    char mac[18],ip[64]; snprintf(mac,sizeof(mac),"02:00:00:00:%02x:%02x",i/256,i%256);
    assert(client_add(all,mac)); snprintf(ip,sizeof(ip),"240e:1::%x",i); client_address(c,ip);
  }
  assert(all->len==601 && c->ipv6->len==602);
  char longname[2049]; memset(longname,'a',2048); longname[2048]=0;
  WifiClient *other=g_ptr_array_index(all,1); client_hostname(other,longname);
  assert(strlen(other->hostname)==2048);
  JsonBuilder *j=json_new(); json_obj_open(j); json_add_str(j,"hostname",other->hostname);
  json_arr_open(j,"ipv6"); for(guint i=0;i<c->ipv6->len;i++) json_arr_add_str(j,g_ptr_array_index(c->ipv6,i));
  json_arr_close(j); json_obj_close(j); char *out=json_finish(j);
  assert(strlen(out)>4096 && strstr(out,"240e:1::258")); free(out);
  g_ptr_array_free(all,TRUE);
  puts("WiFi client tests passed (601 stations, multiple IPv4/IPv6, long hostname)");
  return 0;
}
