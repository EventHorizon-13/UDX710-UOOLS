/**
 * @file wifi.c
 * @brief CW52 WiFi AP status and configuration through hostapd.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include "exec_utils.h"
#include "http_utils.h"
#include "json_builder.h"
#include "wifi.h"
#include "wifi_clients.h"

#define HOSTAPD_CLI "/home/root/6677/cw52-hostapd-cli.sh"
#define HOSTAPD_CTRL "/var/run/hostapd"
#define CONF_2G "/var/mm/hostapd_2.4g.conf"
#define CONF_5G "/var/mm/hostapd_5g.conf"
#define CONF_LIMIT 16384

static pthread_mutex_t wifi_config_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  const char *band, *iface, *conf;
  char state[32], ssid[128], channel[16];
  int available, hidden, max_clients, password_set;
  char encryption[20];
} WifiBand;

static int run_hostapd(const char *iface, const char *command, char *out, size_t size) {
  return run_command(out, size, HOSTAPD_CLI, "-p", HOSTAPD_CTRL, "-i",
                     (char *)iface, (char *)command, NULL);
}

static int read_config_value(const char *path, const char *key, char *value, size_t size) {
  FILE *fp = fopen(path, "r");
  if (!fp) return -1;
  char line[1024];
  size_t len = strlen(key);
  value[0] = '\0';
  while (fgets(line, sizeof(line), fp)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (strncmp(line, key, len) == 0 && line[len] == '=') {
      char *v = line + len + 1;
      v[strcspn(v, "\r\n")] = '\0';
      snprintf(value, size, "%s", v);
      fclose(fp);
      return 0;
    }
  }
  fclose(fp);
  return -1;
}

static void parse_status(WifiBand *band, const char *status) {
  char copy[4096];
  snprintf(copy, sizeof(copy), "%s", status);
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *v = strchr(line, '=');
    if (!v) continue;
    *v++ = '\0';
    if (!strcmp(line, "state")) snprintf(band->state, sizeof(band->state), "%s", v);
    else if (!strcmp(line, "ssid[0]") || !strcmp(line, "ssid")) snprintf(band->ssid, sizeof(band->ssid), "%s", v);
    else if (!strcmp(line, "channel")) snprintf(band->channel, sizeof(band->channel), "%s", v);
  }
}

static int valid_psk(const char *s) {
  if (strlen(s)!=64) return 0;
  for (size_t i=0;i<64;i++) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F'))) return 0;
  return 1;
}
static void read_security(const char *path, char mode[20], int *password_set) {
  char wpa[24]={0},key[80]={0},pass[128]={0},psk[128]={0};
  read_config_value(path,"wpa",wpa,sizeof(wpa));
  read_config_value(path,"wpa_key_mgmt",key,sizeof(key));
  read_config_value(path,"wpa_passphrase",pass,sizeof(pass));
  read_config_value(path,"wpa_psk",psk,sizeof(psk));
  snprintf(mode,20,"%s",!strcmp(wpa,"0") || !wpa[0] ? "open" :
    (!strcmp(wpa,"2") && !strcmp(key,"WPA-PSK") ? "wpa2-psk" : "unsupported"));
  *password_set=(strlen(pass)>=8 && strlen(pass)<=63) || valid_psk(psk);
}

static int read_band(WifiBand *band) {
  char status[4096] = {0}, hidden[32] = {0};
  band->available = 0;
  if (run_hostapd(band->iface, "status", status, sizeof(status)) || strstr(status, "FAIL")) return -1;
  parse_status(band, status);
  if (!band->state[0]) return -1;
  band->hidden = !read_config_value(band->conf, "ignore_broadcast_ssid", hidden, sizeof(hidden)) && atoi(hidden) != 0;
  char maximum[24]={0};
  read_security(band->conf,band->encryption,&band->password_set);
  band->max_clients=!read_config_value(band->conf,"max_num_sta",maximum,sizeof(maximum)) ? atoi(maximum) : 0;
  band->available = 1;
  return 0;
}

static void add_band_json(JsonBuilder *j, const WifiBand *band) {
  json_arr_obj_open(j);
  json_add_str(j, "band", band->band);
  json_add_str(j, "interface", band->iface);
  json_add_bool(j, "available", band->available);
  json_add_bool(j, "enabled", !strcmp(band->state, "ENABLED"));
  json_add_bool(j, "hidden", band->hidden);
  json_add_str(j,"encryption",band->encryption);
  json_add_int(j,"max_clients",band->max_clients);
  json_add_int(j,"max_clients_limit",32);
  json_add_bool(j,"password_set",band->password_set);
  json_add_str(j, "state", band->state);
  json_add_str(j, "ssid", band->ssid);
  json_add_str(j, "channel", band->channel);
  if (band->available) wifi_clients_json(j,band->iface);
  else { json_arr_open(j,"clients"); json_arr_close(j); }
  json_obj_close(j);
}

static void handle_wifi_status(struct mg_connection *c) {
  WifiBand bands[] = {
    {.band = "2.4 GHz", .iface = "wlan1", .conf = CONF_2G},
    {.band = "5 GHz", .iface = "wlan0", .conf = CONF_5G},
  };
  int count = 0;
  for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) if (!read_band(&bands[i])) count++;
  JsonBuilder *j = json_new();
  json_obj_open(j); json_add_int(j, "Code", 0); json_add_str(j, "Error", "");
  json_key_obj_open(j, "Data"); json_add_bool(j, "supported", count > 0);
  json_arr_open(j, "bands");
  for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) add_band_json(j, &bands[i]);
  json_arr_close(j); json_obj_close(j); json_obj_close(j);
  HTTP_OK_FREE(c, json_finish(j));
}

static int valid_ssid(const char *s) {
  size_t n = strlen(s);
  if (!n || n > 32) return 0;
  for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f) return 0;
  return 1;
}

static int valid_password(const char *s) {
  size_t n = strlen(s);
  if (n < 8 || n > 63) return 0;
  for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return 0;
  return 1;
}

static int valid_channel(const char *band, int channel) {
  if (!strcmp(band, "2g")) return channel >= 1 && channel <= 13;
  static const int channels5[] = {36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,149,153,157,161,165};
  for (size_t i = 0; i < sizeof(channels5) / sizeof(channels5[0]); i++) if (channels5[i] == channel) return 1;
  return 0;
}

static int write_all(int fd, const char *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, buf + off, n - off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

static int replace_file(const char *path, const char *buf, size_t n, mode_t mode, uid_t uid, gid_t gid) {
  char tmp[256];
  if (snprintf(tmp, sizeof(tmp), "%s.6677.XXXXXX", path) >= (int)sizeof(tmp)) return -1;
  int fd = mkstemp(tmp);
  if (fd < 0) return -1;
  int ok = fchown(fd, uid, gid) == 0 && fchmod(fd, mode) == 0 &&
           write_all(fd, buf, n) == 0 && fsync(fd) == 0;
  int e = errno;
  if (close(fd)) ok = 0;
  if (!ok || rename(tmp, path)) {
    unlink(tmp);
    errno = e;
    return -1;
  }
  char parent[256]; snprintf(parent,sizeof(parent),"%s",path);
  char *slash = strrchr(parent,'/'); if (!slash) return -1; *slash = 0;
  fd = open(parent,O_RDONLY|O_DIRECTORY); if (fd < 0) return -1;
  ok = fsync(fd) == 0; close(fd); return ok ? 0 : -1;
}

typedef struct {
  const char *ssid,*password,*encryption;
  int channel,hidden,max_clients;
  int ssid_set,password_set,channel_set,hidden_set,encryption_set,max_set;
} WifiUpdate;

static int append_line(char *out,size_t size,size_t *used,const char *line) {
  size_t n=strlen(line);
  if(*used+n+2>=size)return -1;
  memcpy(out+*used,line,n);*used+=n;out[(*used)++]='\n';out[*used]=0;return 0;
}
static int key_is(const char *line,const char *key) {
  size_t n=strlen(key);return !strncmp(line,key,n) && line[n]=='=';
}
static int security_key(const char *line) {
  const char *keys[]={"wpa","wpa_key_mgmt","wpa_pairwise","rsn_pairwise","auth_algs","wpa_passphrase","wpa_psk","sae_password","ieee80211w"};
  for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++)if(key_is(line,keys[i]))return 1;
  return 0;
}
/* Build a complete candidate without writing the original or exposing secrets. */
static int render_config(const char *orig,size_t n,const WifiUpdate *u,char *out,size_t size) {
  if(n>=CONF_LIMIT || memchr(orig,0,n))return -1;
  char input[CONF_LIMIT],line[180],saved_pass[128]={0},saved_psk[128]={0};
  memcpy(input,orig,n);input[n]=0;size_t used=0;out[0]=0;
  char *save=NULL;
  for(char *p=strtok_r(input,"\n",&save);p;p=strtok_r(NULL,"\n",&save)) {
    p[strcspn(p,"\r")]=0;
    if(key_is(p,"wpa_passphrase"))snprintf(saved_pass,sizeof(saved_pass),"%s",strchr(p,'=')+1);
    if(key_is(p,"wpa_psk"))snprintf(saved_psk,sizeof(saved_psk),"%s",strchr(p,'=')+1);
    if((u->ssid_set && (key_is(p,"ssid") || key_is(p,"ssid2"))) ||
       (u->channel_set && key_is(p,"channel")) ||
       (u->hidden_set && key_is(p,"ignore_broadcast_ssid")) ||
       (u->max_set && key_is(p,"max_num_sta")) ||
       (u->encryption_set && security_key(p)) ||
       (u->password_set && (key_is(p,"wpa_passphrase") || key_is(p,"wpa_psk")))) continue;
    if(append_line(out,size,&used,p))return -1;
  }
  if(u->ssid_set) {
    char hex[65];size_t len=strlen(u->ssid);
    for(size_t i=0;i<len;i++)snprintf(hex+i*2,3,"%02x",(unsigned char)u->ssid[i]);
    hex[len*2]=0;
    snprintf(line,sizeof(line),"ssid2=%s",hex);if(append_line(out,size,&used,line))return -1;
  }
  if(u->channel_set){snprintf(line,sizeof(line),"channel=%d",u->channel);if(append_line(out,size,&used,line))return -1;}
  if(u->hidden_set){snprintf(line,sizeof(line),"ignore_broadcast_ssid=%d",!!u->hidden);if(append_line(out,size,&used,line))return -1;}
  if(u->max_set){snprintf(line,sizeof(line),"max_num_sta=%d",u->max_clients);if(append_line(out,size,&used,line))return -1;}
  if(u->encryption_set) {
    if(append_line(out,size,&used,"auth_algs=1"))return -1;
    if(!strcmp(u->encryption,"open")) {
      if(append_line(out,size,&used,"wpa=0") || append_line(out,size,&used,"ieee80211w=0"))return -1;
    } else {
      if(append_line(out,size,&used,"wpa=2") || append_line(out,size,&used,"wpa_key_mgmt=WPA-PSK") ||
         append_line(out,size,&used,"rsn_pairwise=CCMP") || append_line(out,size,&used,"ieee80211w=0"))return -1;
      const char *pass=u->password_set?u->password:saved_pass;
      if(valid_password(pass))snprintf(line,sizeof(line),"wpa_passphrase=%s",pass);
      else if(!u->password_set && valid_psk(saved_psk))snprintf(line,sizeof(line),"wpa_psk=%s",saved_psk);
      else return -1;
      if(append_line(out,size,&used,line))return -1;
    }
  } else if(u->password_set) {
    snprintf(line,sizeof(line),"wpa_passphrase=%s",u->password);if(append_line(out,size,&used,line))return -1;
  }
  return (int)used;
}
static int patch_config(const char *path,const WifiUpdate *u) {
  FILE *fp=fopen(path,"r");if(!fp)return -1;
  struct stat st;char rollback[CONF_LIMIT],out[CONF_LIMIT];
  if(fstat(fileno(fp),&st)){fclose(fp);return -1;}
  size_t got=fread(rollback,1,sizeof(rollback),fp);int bad=ferror(fp);fclose(fp);
  if(bad || got>=CONF_LIMIT)return -1;
  int n=render_config(rollback,got,u,out,sizeof(out));if(n<0)return -1;
  size_t used=(size_t)n;
  char backup[256];
  if (snprintf(backup, sizeof(backup), "%s.6677.bak", path) >= (int)sizeof(backup)) return -1;
  if (replace_file(backup, rollback, got, st.st_mode & 0777, st.st_uid, st.st_gid)) return -1;
  if (replace_file(path, out, used, st.st_mode & 0777, st.st_uid, st.st_gid)) {
    replace_file(path,rollback,got,st.st_mode & 0777,st.st_uid,st.st_gid);
    return -1;
  }
  return 0;
}

static int restore_config(const char *path, mode_t mode, uid_t uid, gid_t gid) {
  char backup[256], buf[CONF_LIMIT];
  if (snprintf(backup, sizeof(backup), "%s.6677.bak", path) >= (int)sizeof(backup)) return -1;
  FILE *fp = fopen(backup, "r");
  if (!fp) return -1;
  size_t n = fread(buf, 1, sizeof(buf), fp);
  int bad = ferror(fp);
  fclose(fp);
  return bad ? -1 : replace_file(path, buf, n, mode, uid, gid);
}

static int wifi_configure_schema(struct mg_str body) {
  int root_len=0;
  if(body.len<2 || body.len>2048 || mg_json_get(body,"$",&root_len)!=0 || (size_t)root_len!=body.len || body.buf[0]!='{')return 0;
  struct mg_str key,val;size_t offset=0;unsigned seen=0;
  const char *names[]={"action","band","ssid","password","channel","hidden","encryption","max_clients","validate_only"};
  while((offset=mg_json_next(body,offset,&key,&val))!=0) {
    int i;for(i=0;i<9;i++)if(key.len==strlen(names[i])+2 && key.buf[0]=='"' && !memcmp(key.buf+1,names[i],key.len-2))break;
    if(i==9 || (seen&(1u<<i)))return 0;
    seen|=1u<<i;
    if(i==5 || i==8){if(mg_strcmp(val,mg_str("true")) && mg_strcmp(val,mg_str("false")))return 0;}
    else if(i==4 || i==7){char path[40];double num;snprintf(path,sizeof(path),"$.%s",names[i]);
      if(!mg_json_get_num(body,path,&num) || num<1 || num>165 || num!=(int)num || (i==7 && num>32))return 0;}
    else if(val.len<2 || val.buf[0]!='"' || val.buf[val.len-1]!='"')return 0;
  }
  return (seen&3u)==3u;
}

static void handle_wifi_configure(struct mg_connection *c, struct mg_http_message *hm) {
  if(!wifi_configure_schema(hm->body)){HTTP_ERROR(c,400,"invalid WiFi field, value type, or duplicate field");return;}
  char *encryption=mg_json_get_str(hm->body,"$.encryption");
  double maxnum=0;int change_max=mg_json_get_num(hm->body,"$.max_clients",&maxnum);
  bool validate_only=false;mg_json_get_bool(hm->body,"$.validate_only",&validate_only);
  char *band = mg_json_get_str(hm->body, "$.band");
  char *ssid = mg_json_get_str(hm->body, "$.ssid");
  char *password = mg_json_get_str(hm->body, "$.password");
  double chnum = 0;
  bool hidden_value = false;
  int change_channel = mg_json_get_num(hm->body, "$.channel", &chnum);
  int change_hidden = mg_json_get_bool(hm->body, "$.hidden", &hidden_value);
  int change_ssid = ssid && ssid[0];
  int change_password = password && password[0];
  const char *iface = NULL, *conf = NULL;
  struct stat st;
  char output[1024] = {0};
  int was_enabled = 0;

  pthread_mutex_lock(&wifi_config_mutex);
  if (!band || (strcmp(band, "2g") && strcmp(band, "5g")) ||
      (!change_ssid && !change_password && !change_channel && !change_hidden && !encryption && !change_max)) {
    HTTP_ERROR(c, 400, "invalid WiFi configuration request");
    goto done;
  }
  if ((change_ssid && !valid_ssid(ssid)) ||
      (change_password && !valid_password(password)) ||
      (change_channel && (chnum < 1 || chnum > 165 || chnum != (int)chnum || !valid_channel(band, (int)chnum)))) {
    HTTP_ERROR(c, 400, "invalid SSID, password, or channel");
    goto done;
  }

  if(encryption && strcmp(encryption,"open") && strcmp(encryption,"wpa2-psk")) {
    HTTP_ERROR(c,400,"encryption must be open or wpa2-psk");goto done;
  }
  if(encryption && !strcmp(encryption,"open") && change_password) {
    HTTP_ERROR(c,400,"open WiFi does not use a password");goto done;
  }
  iface = !strcmp(band, "2g") ? "wlan1" : "wlan0";
  conf = !strcmp(band, "2g") ? CONF_2G : CONF_5G;
  if (stat(conf, &st)) { HTTP_ERROR(c, 500, "hostapd configuration is unavailable"); goto done; }
  if (run_hostapd(iface,"status",output,sizeof(output)) || strstr(output,"FAIL") || !strstr(output,"state=")) {
    HTTP_ERROR(c,503,"hostapd state unavailable; configuration was not changed"); goto done;
  }
  was_enabled = strstr(output,"state=ENABLED") != NULL;
  char current_security[20];int password_available=0;
  read_security(conf,current_security,&password_available);
  const char *target=encryption?encryption:current_security;
  if(!strcmp(target,"wpa2-psk") && !change_password &&
     (!password_available || strcmp(current_security,"wpa2-psk"))) {
    HTTP_ERROR(c,400,"a new WPA2 password is required");goto done;
  }
  if(change_password && strcmp(target,"wpa2-psk")) {
    HTTP_ERROR(c,400,"select WPA2-PSK before setting a password");goto done;
  }
  if(validate_only) {HTTP_OK(c,"{\"Code\":0,\"Error\":\"\",\"Data\":\"WiFi configuration is valid; no changes applied\"}");goto done;}
  WifiUpdate update={.ssid=ssid,.password=password,.encryption=encryption,.channel=(int)chnum,
    .hidden=hidden_value,.max_clients=(int)maxnum,.ssid_set=change_ssid,.password_set=change_password,
    .channel_set=change_channel,.hidden_set=change_hidden,.encryption_set=encryption!=NULL,.max_set=change_max};
  if (patch_config(conf,&update)) {
    HTTP_ERROR(c,500,"failed to update hostapd configuration; recovery copy retained");goto done;
  }

  if (was_enabled) {
    memset(output, 0, sizeof(output));
    int applied = !run_hostapd(iface,"reload",output,sizeof(output)) && strstr(output,"OK");
    int verified = 0;
    for (int attempt=0;applied && attempt<5;attempt++) {
      WifiBand actual = {0}; memset(output,0,sizeof(output));
      if (!run_hostapd(iface,"status",output,sizeof(output))) {
        parse_status(&actual,output);
        verified = !strcmp(actual.state,"ENABLED") && (!change_ssid || !strcmp(actual.ssid,ssid)) &&
          (!change_channel || atoi(actual.channel)==(int)chnum);
      }
      if(verified && (change_hidden || change_max)) {
        char value[32]={0};
        if(change_hidden)verified=!read_config_value(conf,"ignore_broadcast_ssid",value,sizeof(value)) && atoi(value)==(int)hidden_value;
        if(verified && change_max)verified=!read_config_value(conf,"max_num_sta",value,sizeof(value)) && atoi(value)==(int)maxnum;
      }
      if(verified && encryption) {
        char live[2048]={0};
        verified=!run_hostapd(iface,"get_config",live,sizeof(live));
        if(verified)verified=strstr(live,!strcmp(encryption,"open")?"wpa=0":"wpa=2")!=NULL;
        if(verified && !strcmp(encryption,"wpa2-psk"))verified=strstr(live,"key_mgmt=WPA-PSK")!=NULL;
      }
      if (verified) break;
      sleep(1);
    }
    if (!verified) {
      int restored = !restore_config(conf, st.st_mode & 0777, st.st_uid, st.st_gid);
      if (restored) { memset(output, 0, sizeof(output)); run_hostapd(iface, "reload", output, sizeof(output)); }
      HTTP_ERROR(c, 500, restored ? "hostapd rejected config; previous file restored" : "hostapd rejected config; restore failed");
      goto done;
    }
  }

  JsonBuilder *j = json_new();
  json_obj_open(j); json_add_int(j, "Code", 0); json_add_str(j, "Error", "");
  json_add_str(j, "Data", was_enabled ? "WiFi 配置已保存并应用" : "WiFi 配置已保存，下次开启热点时应用"); json_obj_close(j);
  HTTP_OK_FREE(c, json_finish(j));

done:
  pthread_mutex_unlock(&wifi_config_mutex);
  free(band); free(ssid); free(password);free(encryption);
}

static void handle_wifi_control(struct mg_connection *c, struct mg_http_message *hm) {
  char *band = mg_json_get_str(hm->body, "$.band");
  int enabled = -1;
  bool value = false;
  if (mg_json_get_bool(hm->body, "$.enabled", &value)) enabled = value;
  if (!band || (strcmp(band, "2g") && strcmp(band, "5g")) || enabled < 0) {
    free(band); HTTP_ERROR(c, 400, "band must be 2g or 5g and enabled must be boolean"); return;
  }
  const char *iface = !strcmp(band, "2g") ? "wlan1" : "wlan0";
  char output[1024] = {0};
  int result = run_hostapd(iface, enabled ? "enable" : "disable", output, sizeof(output));
  free(band);
  if (result || strstr(output, "FAIL")) {
    JsonBuilder *j = json_new(); json_obj_open(j); json_add_int(j, "Code", 1);
    json_add_str(j, "Error", "hostapd rejected the radio change"); json_add_str(j, "Data", output);
    json_obj_close(j); HTTP_OK_FREE(c, json_finish(j)); return;
  }
  JsonBuilder *j = json_new(); json_obj_open(j); json_add_int(j, "Code", 0); json_add_str(j, "Error", "");
  json_add_str(j, "Data", enabled ? "热点已开启" : "热点已关闭"); json_obj_close(j);
  HTTP_OK_FREE(c, json_finish(j));
}

void handle_wifi(struct mg_connection *c, struct mg_http_message *hm) {
  HTTP_CHECK_ANY(c, hm);
  if (http_is_method(hm, "GET")) handle_wifi_status(c);
  else if (http_is_method(hm, "POST")) {
    char *action = mg_json_get_str(hm->body, "$.action");
    if (action && !strcmp(action, "configure")) handle_wifi_configure(c, hm);
    else handle_wifi_control(c, hm);
    free(action);
  } else http_method_error(c);
}

