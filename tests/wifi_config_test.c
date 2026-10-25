#include <assert.h>
#include "../src/system/wifi.c"
static const char original[]="interface=wlan1\n# keep-comment\nssid=old\nchannel=6\nignore_broadcast_ssid=0\nmax_num_sta=32\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase=fixture-password\ncustom_vendor_option=keep\n";
int main(void) {
 char out[CONF_LIMIT];WifiUpdate u={.encryption="open",.encryption_set=1,.max_set=1,.max_clients=12,.hidden_set=1,.hidden=1};
 assert(render_config(original,strlen(original),&u,out,sizeof(out))>0);
 assert(strstr(out,"wpa=0\n") && !strstr(out,"wpa_passphrase=") && !strstr(out,"wpa_key_mgmt="));
 assert(strstr(out,"ignore_broadcast_ssid=1\n") && strstr(out,"max_num_sta=12\n") && strstr(out,"custom_vendor_option=keep\n"));
 char open[CONF_LIMIT];snprintf(open,sizeof(open),"%s",out);
 u=(WifiUpdate){.encryption="wpa2-psk",.encryption_set=1};
 assert(render_config(open,strlen(open),&u,out,sizeof(out))<0);
 u.password="new-fixture-password";u.password_set=1;
 assert(render_config(open,strlen(open),&u,out,sizeof(out))>0 && strstr(out,"wpa=2\n") && strstr(out,"wpa_passphrase=new-fixture-password\n"));
 u=(WifiUpdate){.encryption="wpa2-psk",.encryption_set=1};
 assert(render_config(original,strlen(original),&u,out,sizeof(out))>0 && strstr(out,"wpa_passphrase=fixture-password\n"));
 u=(WifiUpdate){.ssid="中文WiFi",.ssid_set=1,.max_set=1,.max_clients=1};
 assert(render_config(original,strlen(original),&u,out,sizeof(out))>0 && strstr(out,"ssid2=") && !strstr(out,"\nssid=old") && strstr(out,"max_num_sta=1\n"));
 assert(render_config(original,strlen(original),&u,out,24)<0);
 u=(WifiUpdate){.password="updated-fixture",.password_set=1};
 const char *psk="wpa_psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
 assert(render_config(psk,strlen(psk),&u,out,sizeof(out))>0 && !strstr(out,"wpa_psk=") && strstr(out,"wpa_passphrase=updated-fixture"));
 const char *valid[]={"{\"action\":\"configure\",\"band\":\"2g\",\"max_clients\":1}","{\"action\":\"configure\",\"band\":\"5g\",\"max_clients\":32,\"hidden\":false,\"encryption\":\"wpa2-psk\",\"validate_only\":true}"};
 for(size_t i=0;i<2;i++)assert(wifi_configure_schema(mg_str(valid[i])));
 const char *bad[]={"{}","[]","{\"action\":\"configure\",\"band\":\"2g\",\"max_clients\":33}","{\"action\":\"configure\",\"band\":\"2g\",\"max_clients\":0}","{\"action\":\"configure\",\"band\":\"2g\",\"max_clients\":1.5}","{\"action\":\"configure\",\"band\":\"2g\",\"max_clients\":\"12\"}","{\"action\":\"configure\",\"band\":\"2g\",\"hidden\":1}","{\"action\":\"configure\",\"band\":\"2g\",\"encryption\":false}","{\"action\":\"configure\",\"band\":\"2g\",\"max_clients\":1,\"max_clients\":32}","{\"action\":\"configure\",\"band\":\"2g\",\"surprise\":true}","{\"action\":\"configure\",\"band\":\"2g\"} trailing"};
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!wifi_configure_schema(mg_str(bad[i])));
 /* Actual atomic patch + backup + recovery on a private fixture, no AP involved. */
 char dir[]="/tmp/wifi-config-XXXXXX";assert(mkdtemp(dir));char path[256],backup[280];snprintf(path,sizeof(path),"%s/hostapd.conf",dir);snprintf(backup,sizeof(backup),"%s.6677.bak",path);
 FILE *f=fopen(path,"w");assert(f);assert(fwrite(original,1,strlen(original),f)==strlen(original));fclose(f);chmod(path,0600);
 u=(WifiUpdate){.hidden=1,.hidden_set=1,.max_clients=31,.max_set=1};assert(!patch_config(path,&u));
 char value[64];assert(!read_config_value(path,"max_num_sta",value,sizeof(value)) && !strcmp(value,"31"));
 struct stat st;assert(!stat(path,&st) && (st.st_mode&0777)==0600);assert(!restore_config(path,0600,st.st_uid,st.st_gid));
 f=fopen(path,"r");size_t n=fread(out,1,sizeof(out),f);fclose(f);assert(n==strlen(original) && !memcmp(out,original,n));
 unlink(backup);unlink(path);rmdir(dir);
 puts("WiFi encryption, key cleanup, bounds, schema, hidden SSID, atomic backup and rollback fixtures passed");return 0;
}
