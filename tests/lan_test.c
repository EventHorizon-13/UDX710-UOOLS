#include <assert.h>
#include "../src/system/lan.c"

int main(void) {
  LanConfig v;
  assert(parse_config("192.168.1.1,255.255.255.0,192.168.1.2,192.168.1.254,0",&v));
  assert(valid_pool(&v));
  strcpy(v.start,"192.168.1.0"); assert(!valid_pool(&v));
  strcpy(v.start,"192.168.1.1"); assert(!valid_pool(&v));
  strcpy(v.start,"192.168.2.2"); assert(!valid_pool(&v));
  strcpy(v.start,"192.168.1.200"); strcpy(v.end,"192.168.1.100"); assert(!valid_pool(&v));
  strcpy(v.end,"192.168.1.255"); assert(!valid_pool(&v));
  strcpy(v.mask,"255.0.255.0"); assert(!valid_pool(&v));
  assert(!parse_config("192.168.1.1,255.255.255.0,192.168.1.2,192.168.1.254,0x",&v));
  assert(!parse_config("192.168.1.1,255.255.255.0,192.168.1.2,192.168.1.254,0,extra",&v));
  assert(!ipv4("192.168.01.2",NULL)); assert(!ipv4("192.168.1.2;reboot",NULL));
  assert(!ipv4("256.1.2.3",NULL));
  assert(valid_request(mg_str("{\"pool_start\":\"192.168.1.2\",\"pool_end\":\"192.168.1.254\",\"version\":\"v\",\"confirm\":true,\"validate_only\":true}")));
  assert(!valid_request(mg_str("{\"pool_start\":\"192.168.1.2\",\"pool_end\":\"192.168.1.254\",\"version\":\"v\",\"confirm\":true,\"address\":\"192.168.2.1\"}")));
  assert(!valid_request(mg_str("{\"pool_start\":\"192.168.1.2\",\"pool_start\":\"192.168.1.3\",\"pool_end\":\"192.168.1.254\",\"version\":\"v\",\"confirm\":true}")));
  assert(!valid_request(mg_str("{\"pool_start\":\"192.168.1.2\\u0000evil\",\"pool_end\":\"192.168.1.254\",\"version\":\"v\",\"confirm\":true}")));
  assert(!valid_request(mg_str("{\"pool_start\":\"192.168.1.2\",\"pool_end\":\"192.168.1.254\",\"version\":\"v\",\"confirm\":\"true\"}")));
  assert(!valid_request(mg_str("{} garbage")));
  puts("LAN validation tests passed (no network writes)");
  return 0;
}
