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
  assert(mask_prefix_u32(0xffffff00U)==24 && mask_prefix_u32(0xfffffffcU)==30);
  assert(mask_prefix_u32(0xff00ff00U)==-1 && mask_prefix_u32(0)==0);
  assert(lan_addr_is_managed(&old,0xc0a80101U));
  assert(!lan_addr_is_managed(&old,0x0a0000feU));
  assert(!private_network(0x0a000001U,0xfe000000U));
  assert(parse_config("192.168.1.254,255.255.255.0,192.168.1.2,192.168.1.253,0",&next));assert(valid_address_config(&next));
  strcpy(next.start,"192.168.1.1");assert(!valid_pool(&next));
  /* The actual reported /24-within-cellular-/8 case is routable. Reject the
   * reverse containment, endpoints, equal prefixes and non-cellular overlaps. */
  assert(!interface_subnet_conflict("sipa_eth0",0x0a0000feU,0xffffff00U,0x0ac54ddfU,0xff000000U));
  assert(interface_subnet_conflict("sipa_eth0",0x0a0000feU,0xff000000U,0x0ac54ddfU,0xff000000U));
  assert(interface_subnet_conflict("sipa_eth0",0x0ac54dfeU,0xffffff00U,0x0ac54ddfU,0xff000000U));
  assert(interface_subnet_conflict("eth9",0x0a0000feU,0xffffff00U,0x0ac54ddfU,0xff000000U));
  assert(interface_subnet_conflict("sipa_eth0bad",0x0a0000feU,0xffffff00U,0x0ac54ddfU,0xff000000U));
  assert(interface_subnet_conflict("sipa_eth0",0x0a0000feU,0xffffff00U,0x0a000001U,0xffffff00U));
  assert(interface_subnet_conflict("sipa_eth0",0x0a0000feU,0xffffff00U,0x0a000001U,0xffff0000U));
  assert(interface_subnet_conflict("sipa_eth0",0x0a0000feU,0xffffff00U,0x0ac54ddfU,0xff00ff00U));
  assert(!interface_subnet_conflict("eth9",0xc0a80a01U,0xffffff00U,0x0ac54ddfU,0xff000000U));
  assert(safe_migration_dir(LAN_BACKUPS "/lan-address-AbC123"));
  assert(!safe_migration_dir(LAN_BACKUPS "/lan-address-../etc"));assert(!safe_migration_dir("/tmp/lan-address-AbC123"));
  assert(address_request(mg_str("{\"action\":\"address\",\"address\":\"10.0.0.52\",\"netmask\":\"255.255.255.0\",\"pool_start\":\"10.0.0.100\",\"pool_end\":\"10.0.0.254\",\"version\":\"v\",\"confirm\":true,\"validate_only\":true}"),0));
  assert(!address_request(mg_str("{\"action\":\"address\",\"address\":\"10.0.0.52\",\"netmask\":\"255.255.255.0\",\"pool_start\":\"10.0.0.100\",\"pool_end\":\"10.0.0.254\",\"version\":\"v\",\"confirm\":true,\"confirm\":false}"),0));
  assert(address_request(mg_str("{\"action\":\"confirm_address\",\"id\":\"lan-address-AbC123\",\"confirm\":true}"),1));
  assert(!address_request(mg_str("{\"action\":\"confirm_address\",\"id\":\"lan-address-AbC123\",\"confirm\":\"true\"}"),1));
  struct mg_connection c={0};inet_pton(AF_INET,"10.0.0.52",c.loc.addr.ip);
  assert(!confirmed_address_socket(&c,"10.0.0.52"));
  int listener=socket(AF_INET,SOCK_STREAM,0),client=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0 && client>=0);
  struct sockaddr_in endpoint={.sin_family=AF_INET};inet_pton(AF_INET,"127.0.0.1",&endpoint.sin_addr);
  assert(!bind(listener,(struct sockaddr *)&endpoint,sizeof(endpoint)) && !listen(listener,1));
  socklen_t endpoint_len=sizeof(endpoint);assert(!getsockname(listener,(struct sockaddr *)&endpoint,&endpoint_len));
  assert(!connect(client,(struct sockaddr *)&endpoint,sizeof(endpoint)));
  int accepted=accept(listener,NULL,NULL);assert(accepted>=0);c.fd=(void *)(intptr_t)accepted;
  /* Deliberately incorrect cached metadata, as with the wildcard listener. */
  assert(confirmed_address_socket(&c,"127.0.0.1"));
  assert(!confirmed_address_socket(&c,"10.0.0.52"));
  c.is_listening=true;assert(!confirmed_address_socket(&c,"127.0.0.1"));c.is_listening=false;
  c.is_udp=true;assert(!confirmed_address_socket(&c,"127.0.0.1"));c.is_udp=false;
  close(accepted);close(client);close(listener);
  char *script=migration_rollback_script("/tmp/fixture",&old,&next);
  assert(strstr(script,"flock -x 9") && strstr(script,"while [ $n -lt 120 ]") && strstr(script,"committed"));
  assert(!strstr(script,"address flush") && !strstr(script,"-F INPUT"));g_free(script);
  puts("LAN address validation and confirmation tests passed (no network writes)");return 0;
}

