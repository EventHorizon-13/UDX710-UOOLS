#include <assert.h>
#include "../src/system/lan.c"

int main(int argc,char **argv) {
  LanConfig old,next;
  assert(parse_config("192.168.1.1,255.255.255.0,192.168.1.2,192.168.1.254,0",&old));
  assert(parse_config("192.168.10.1,255.255.255.0,192.168.10.2,192.168.10.254,0",&next));
  if(argc==2 && !strcmp(argv[1],"--rollback-fixture")) {char *s=migration_rollback_script("/tmp/fixture",&old,&next);fputs(s,stdout);g_free(s);return 0;}
  assert(valid_address_config(&old) && valid_address_config(&next));
  strcpy(next.address,"192.168.10.0");assert(!valid_address_config(&next));
  strcpy(next.address,"192.168.10.255");assert(!valid_address_config(&next));
  assert(parse_config("8.8.8.1,255.255.255.0,8.8.8.2,8.8.8.254,0",&next));assert(!valid_address_config(&next));
  assert(parse_config("10.0.0.52,255.255.255.0,10.0.0.100,10.0.0.254,0",&next));assert(valid_address_config(&next));
  assert(mask_prefix("255.255.255.0")==24 && mask_prefix("255.255.255.252")==30);
  assert(mask_prefix("255.0.255.0")==-1 && mask_prefix("0.0.0.0")==-1);
  assert(!private_network(0x0a000001U,0xfe000000U));
  assert(parse_config("192.168.1.254,255.255.255.0,192.168.1.2,192.168.1.253,0",&next));assert(valid_address_config(&next));
  strcpy(next.start,"192.168.1.1");assert(!valid_pool(&next));
  assert(safe_migration_dir(LAN_BACKUPS "/lan-address-AbC123"));
  assert(!safe_migration_dir(LAN_BACKUPS "/lan-address-../etc"));assert(!safe_migration_dir("/tmp/lan-address-AbC123"));
  assert(address_request(mg_str("{\"action\":\"address\",\"address\":\"10.0.0.52\",\"netmask\":\"255.255.255.0\",\"pool_start\":\"10.0.0.100\",\"pool_end\":\"10.0.0.254\",\"version\":\"v\",\"confirm\":true,\"validate_only\":true}"),0));
  assert(!address_request(mg_str("{\"action\":\"address\",\"address\":\"10.0.0.52\",\"netmask\":\"255.255.255.0\",\"pool_start\":\"10.0.0.100\",\"pool_end\":\"10.0.0.254\",\"version\":\"v\",\"confirm\":true,\"confirm\":false}"),0));
  assert(address_request(mg_str("{\"action\":\"confirm_address\",\"id\":\"lan-address-AbC123\",\"confirm\":true}"),1));
  assert(!address_request(mg_str("{\"action\":\"confirm_address\",\"id\":\"lan-address-AbC123\",\"confirm\":\"true\"}"),1));
  struct mg_connection c={0};inet_pton(AF_INET,"192.168.1.1",c.loc.addr.ip);
  assert(!confirmed_address_socket(&c,"10.0.0.52"));inet_pton(AF_INET,"10.0.0.52",c.loc.addr.ip);assert(confirmed_address_socket(&c,"10.0.0.52"));
  c.loc.is_ip6=true;assert(!confirmed_address_socket(&c,"10.0.0.52"));
  char *script=migration_rollback_script("/tmp/fixture",&old,&next);
  assert(strstr(script,"flock -x 9") && strstr(script,"while [ $n -lt 120 ]") && strstr(script,"committed"));
  assert(!strstr(script,"address flush") && !strstr(script,"-F INPUT"));g_free(script);
  puts("LAN address validation and confirmation tests passed (no network writes)");return 0;
}
