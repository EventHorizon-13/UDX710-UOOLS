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

#define HOSTAPD_CLI "/usr/sbin/hostapd_cli"
#define HOSTAPD_CTRL "/var/run/hostapd"
#define CONF_2G "/var/mm/hostapd_2.4g.conf"
#define CONF_5G "/var/mm/hostapd_5g.conf"
#define CONF_LIMIT 16384

static pthread_mutex_t wifi_config_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  const char *band, *iface, *conf;
  char state[32], ssid[128], channel[16];
  int available, hidden;
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

static int read_band(WifiBand *band) {
  char status[4096] = {0}, hidden[32] = {0};
  band->available = 0;
  if (run_hostapd(band->iface, "status", status, sizeof(status)) || strstr(status, "FAIL")) return -1;
  parse_status(band, status);
  if (!band->state[0]) return -1;
  band->hidden = !read_config_value(band->conf, "ignore_broadcast_ssid", hidden, sizeof(hidden)) && atoi(hidden) != 0;
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

static int patch_config(const char *path, const char *ssid, const char *password,
                        int channel, int hidden, int cs, int cp, int cc, int ch) {
  FILE *fp = fopen(path, "r");
  if (!fp) return -1;
  struct stat st;
  if (fstat(fileno(fp), &st) || fseek(fp, 0, SEEK_END)) { fclose(fp); return -1; }
  long sz = ftell(fp);
  if (sz < 0 || sz >= CONF_LIMIT || fseek(fp, 0, SEEK_SET)) { fclose(fp); return -1; }
  char orig[CONF_LIMIT], rollback[CONF_LIMIT], out[CONF_LIMIT];
  size_t got = fread(orig, 1, (size_t)sz, fp);
  fclose(fp);
  if (got != (size_t)sz) return -1;
  memcpy(rollback, orig, got);
  orig[got] = '\0';

  int ws = 0, wp = 0, wc = 0, wh = 0;
  size_t used = 0;
  char *save = NULL;
  for (char *line = strtok_r(orig, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *eq = strchr(line, '=');
    size_t k = eq && line[0] != '#' ? (size_t)(eq - line) : 0;
    int is_ssid = eq && ((k == 4 && !strncmp(line, "ssid", 4)) || (k == 5 && !strncmp(line, "ssid2", 5)));
    int is_pass = eq && k == 14 && !strncmp(line, "wpa_passphrase", 14);
    int is_channel = eq && k == 7 && !strncmp(line, "channel", 7);
    int is_hidden = eq && k == 21 && !strncmp(line, "ignore_broadcast_ssid", 21);
    char repl[128];
    const char *chosen = line;
    if (is_ssid && cs) {
      if (ws) continue;
      size_t n = strlen(ssid); char hex[65];
      for (size_t i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", (unsigned char)ssid[i]);
      hex[n * 2] = '\0'; snprintf(repl, sizeof(repl), "ssid2=%s", hex); chosen = repl; ws = 1;
    } else if (is_pass && cp) {
      if (wp) continue;
      snprintf(repl, sizeof(repl), "wpa_passphrase=%s", password); chosen = repl; wp = 1;
    } else if (is_channel && cc) {
      if (wc) continue;
      snprintf(repl, sizeof(repl), "channel=%d", channel); chosen = repl; wc = 1;
    } else if (is_hidden && ch) {
      if (wh) continue;
      snprintf(repl, sizeof(repl), "ignore_broadcast_ssid=%d", hidden ? 1 : 0); chosen = repl; wh = 1;
    }
    size_t n = strlen(chosen);
    if (used + n + 2 >= sizeof(out)) return -1;
    memcpy(out + used, chosen, n); used += n; out[used++] = '\n';
  }
  if (cs && !ws) {
    size_t n = strlen(ssid); char hex[65];
    for (size_t i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", (unsigned char)ssid[i]);
    hex[n * 2] = '\0';
    int m = snprintf(out + used, sizeof(out) - used, "ssid2=%s\n", hex);
    if (m < 0 || (size_t)m >= sizeof(out) - used) return -1;
    used += (size_t)m;
  }
  if (cp && !wp) { int m = snprintf(out + used, sizeof(out) - used, "wpa_passphrase=%s\n", password); if (m < 0 || (size_t)m >= sizeof(out) - used) return -1; used += (size_t)m; }
  if (cc && !wc) { int m = snprintf(out + used, sizeof(out) - used, "channel=%d\n", channel); if (m < 0 || (size_t)m >= sizeof(out) - used) return -1; used += (size_t)m; }
  if (ch && !wh) { int m = snprintf(out + used, sizeof(out) - used, "ignore_broadcast_ssid=%d\n", hidden ? 1 : 0); if (m < 0 || (size_t)m >= sizeof(out) - used) return -1; used += (size_t)m; }

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

static void handle_wifi_configure(struct mg_connection *c, struct mg_http_message *hm) {
  char *band = mg_json_get_str(hm->body, "$.band");
  char *ssid = mg_json_get_str(hm->body, "$.ssid");
  char *password = mg_json_get_str(hm->body, "$.password");
  double chnum = 0;
  int hidden_value = 0;
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
      (!change_ssid && !change_password && !change_channel && !change_hidden)) {
    HTTP_ERROR(c, 400, "invalid WiFi configuration request");
    goto done;
  }
  if ((change_ssid && !valid_ssid(ssid)) ||
      (change_password && !valid_password(password)) ||
      (change_channel && (chnum < 1 || chnum > 165 || chnum != (int)chnum || !valid_channel(band, (int)chnum)))) {
    HTTP_ERROR(c, 400, "invalid SSID, password, or channel");
    goto done;
  }

  iface = !strcmp(band, "2g") ? "wlan1" : "wlan0";
  conf = !strcmp(band, "2g") ? CONF_2G : CONF_5G;
  if (stat(conf, &st)) { HTTP_ERROR(c, 500, "hostapd configuration is unavailable"); goto done; }
  if (run_hostapd(iface,"status",output,sizeof(output)) || strstr(output,"FAIL") || !strstr(output,"state=")) {
    HTTP_ERROR(c,503,"hostapd state unavailable; configuration was not changed"); goto done;
  }
  was_enabled = strstr(output,"state=ENABLED") != NULL;
  if (patch_config(conf, ssid ? ssid : "", password ? password : "",
                   (int)chnum, hidden_value, change_ssid, change_password,
                   change_channel, change_hidden)) {
    HTTP_ERROR(c, 500, "failed to update hostapd configuration; recovery copy retained");
    goto done;
  }

  if (was_enabled) {
    memset(output, 0, sizeof(output));
    int applied = !run_hostapd(iface,"reconfigure",output,sizeof(output)) && strstr(output,"OK");
    int verified = 0;
    for (int attempt=0;applied && attempt<5;attempt++) {
      WifiBand actual = {0}; memset(output,0,sizeof(output));
      if (!run_hostapd(iface,"status",output,sizeof(output))) {
        parse_status(&actual,output);
        verified = !strcmp(actual.state,"ENABLED") && (!change_ssid || !strcmp(actual.ssid,ssid)) &&
          (!change_channel || atoi(actual.channel)==(int)chnum);
      }
      if (verified) break;
      sleep(1);
    }
    if (!verified) {
      int restored = !restore_config(conf, st.st_mode & 0777, st.st_uid, st.st_gid);
      if (restored) { memset(output, 0, sizeof(output)); run_hostapd(iface, "reconfigure", output, sizeof(output)); }
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
  free(band); free(ssid); free(password);
}

static void handle_wifi_control(struct mg_connection *c, struct mg_http_message *hm) {
  char *band = mg_json_get_str(hm->body, "$.band");
  int enabled = -1, value = 0;
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
