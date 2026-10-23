/* CW52's vendor ConnMan TetheringDHCP interface, confirmed against libmumu.
 * Only the pool is writable: the installed ttyd and firewall pin the LAN subnet.
 * Do not toggle Tethering: that also controls the USB management transport. */
#include <gio/gio.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include "http_utils.h"
#include "json_builder.h"
#include "lan.h"

#define LAN_FILE "/var/mm/dhcp_config.ini"
#define LAN_BACKUPS "/mnt/data/local-udx710"
#define LAN_TECH "/net/connman/technology/gadget"
typedef struct { char address[16], mask[16], start[16], end[16]; int ttl; } LanConfig;

static int ipv4(const char *s, uint32_t *n) {
  struct in_addr a;
  if (!s || inet_pton(AF_INET, s, &a) != 1) return 0;
  char canonical[16]; inet_ntop(AF_INET, &a, canonical, sizeof(canonical));
  if (strcmp(s, canonical)) return 0;
  if (n) *n = ntohl(a.s_addr);
  return 1;
}

static int valid_pool(const LanConfig *v) {
  uint32_t a, m, s, e;
  if (!ipv4(v->address, &a) || !ipv4(v->mask, &m) ||
      !ipv4(v->start, &s) || !ipv4(v->end, &e)) return 0;
  uint32_t host = ~m;
  if (!m || host < 3 || (host & (host + 1)) ||
      (a & m) != (s & m) || (a & m) != (e & m)) return 0;
  return s <= e && (s & host) && (e & host) != host && !(s <= a && a <= e);
}

static char *config_string(const LanConfig *v) {
  return g_strdup_printf("%s,%s,%s,%s,%d", v->address, v->mask, v->start, v->end, v->ttl);
}

static int parse_config(const char *s, LanConfig *v) {
  char **p = g_strsplit(s, ",", -1);
  int ok = g_strv_length(p) == 5;
  if (ok) {
    for (int i = 0; i < 4; i++) if (!ipv4(p[i], NULL)) ok = 0;
    char *end; gint64 ttl = g_ascii_strtoll(p[4], &end, 10);
    if (!p[4][0] || *end || ttl < 0 || ttl > 2147483647) ok = 0;
    if (ok) {
      g_strlcpy(v->address, p[0], 16); g_strlcpy(v->mask, p[1], 16);
      g_strlcpy(v->start, p[2], 16); g_strlcpy(v->end, p[3], 16); v->ttl = (int)ttl;
      ok = valid_pool(v);
    }
  }
  g_strfreev(p); return ok;
}

static GVariant *connman_call(const char *method, GVariant *args, const GVariantType *type) {
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!bus) { g_clear_error(&error); return NULL; }
  GVariant *r = g_dbus_connection_call_sync(bus, "net.connman", LAN_TECH,
    "net.connman.Technology", method, args, type, G_DBUS_CALL_FLAGS_NONE, 4000, NULL, &error);
  g_object_unref(bus); g_clear_error(&error); return r;
}

static int read_live(LanConfig *v) {
  GVariant *r = connman_call("GetProperties", NULL, G_VARIANT_TYPE("(a{sv})"));
  if (!r) return 0;
  GVariant *dict = g_variant_get_child_value(r, 0);
  const char *s = NULL;
  int ok = g_variant_lookup(dict, "TetheringDHCP", "&s", &s) && parse_config(s, v);
  g_variant_unref(dict); g_variant_unref(r); return ok;
}

static int apply_live(const LanConfig *v) {
  char *s = config_string(v);
  GVariant *r = connman_call("SetProperty", g_variant_new("(sv)", "TetheringDHCP",
                              g_variant_new_string(s)), G_VARIANT_TYPE("()"));
  g_free(s);
  if (!r) return 0;
  g_variant_unref(r); return 1;
}

static int sync_parent(const char *path) {
  char *dir = g_path_get_dirname(path);
  int fd = open(dir, O_RDONLY | O_DIRECTORY); g_free(dir);
  if (fd < 0) return 0;
  int ok = fsync(fd) == 0; close(fd); return ok;
}

static int atomic_file(const char *path, const char *data, size_t len, const struct stat *st) {
  char *tmp = g_strconcat(path, ".XXXXXX", NULL);
  int fd = mkstemp(tmp), ok = fd >= 0;
  if (ok) ok = fchown(fd, st->st_uid, st->st_gid) == 0 && fchmod(fd, st->st_mode & 0777) == 0;
  size_t off = 0;
  while (ok && off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = 0; else off += (size_t)n;
  }
  if (ok) ok = fsync(fd) == 0;
  if (fd >= 0 && close(fd)) ok = 0;
  if (ok) ok = rename(tmp, path) == 0 && sync_parent(path);
  if (!ok) unlink(tmp);
  g_free(tmp); return ok;
}

static int lan_address_present(const LanConfig *v) {
  struct ifaddrs *all;
  if (getifaddrs(&all)) return 0;
  uint32_t address, mask; ipv4(v->address, &address); ipv4(v->mask, &mask);
  int found = 0;
  for (struct ifaddrs *p = all; p; p = p->ifa_next)
    if (p->ifa_addr && p->ifa_netmask && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, "tether") &&
        ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr) == address &&
        ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr) == mask) found = 1;
  freeifaddrs(all); return found;
}

static int management_reachable(const LanConfig *v) {
  const int ports[] = {80, 7681};
  for (size_t i = 0; i < 2; i++) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(ports[i])};
    inet_pton(AF_INET, v->address, &a.sin_addr);
    int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (rc && errno == EINPROGRESS) {
      fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf); struct timeval tv = {1, 0};
      int err = 1; socklen_t n = sizeof(err);
      rc = select(fd + 1, NULL, &wf, NULL, &tv) > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) && !err ? 0 : -1;
    }
    close(fd); if (rc) return 0;
  }
  return lan_address_present(v);
}

/* Independent executable watchdog survives server exit. Recovery materials are
 * durable BEFORE any network write. No change touches routing or firewall. */
static pid_t start_watchdog(const char *dir, const LanConfig *old) {
  char *value = config_string(old);
  char *script = g_strdup_printf(
    "#!/bin/sh\ncd '%s' || exit 1\n"
    "restore() {\n dbus-send --system --print-reply --reply-timeout=4000 --dest=net.connman "
    LAN_TECH " net.connman.Technology.SetProperty string:TetheringDHCP variant:string:'%s' >dbus-rollback.log 2>&1\n"
    " cp -p dhcp_config.ini.before " LAN_FILE ".rollback.tmp && mv -f " LAN_FILE ".rollback.tmp " LAN_FILE "\n sync\n}\n"
    "if [ \"$1\" = --now ]; then restore; exit $?; fi\n"
    "n=0\nwhile [ $n -lt 30 ]; do [ ! -e committed ] || exit 0; sleep 1; n=$((n+1)); done\n"
    "restore\n", dir, value);
  char *path = g_build_filename(dir, "rollback.sh", NULL);
  struct stat st = {.st_mode = 0700, .st_uid = geteuid(), .st_gid = getegid()};
  int ok = atomic_file(path, script, strlen(script), &st);
  pid_t pid = ok ? fork() : -1;
  if (pid == 0) {
    setsid(); int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    execl("/bin/sh", "sh", path, (char *)NULL); _exit(127);
  }
  g_free(path); g_free(script); g_free(value); return pid;
}

static int same_config(const LanConfig *a, const LanConfig *b) {
  return !strcmp(a->address,b->address) && !strcmp(a->mask,b->mask) &&
    !strcmp(a->start,b->start) && !strcmp(a->end,b->end) && a->ttl == b->ttl;
}

static int valid_request(struct mg_str body) {
  int len = 0;
  if (body.len > 2048 || mg_json_get(body,"$",&len) != 0 || (size_t)len != body.len || body.buf[0] != '{') return 0;
  const char *names[] = {"\"pool_start\"","\"pool_end\"","\"version\"","\"confirm\"","\"validate_only\""};
  unsigned seen = 0; size_t off = 0; struct mg_str key,val;
  while ((off = mg_json_next(body,off,&key,&val))) {
    int which = -1;
    for (int i=0;i<5;i++) if (!mg_strcmp(key,mg_str(names[i]))) which=i;
    if (which<0 || (seen & (1U<<which))) return 0;
    seen |= 1U<<which;
    if (which<3) {
      if (val.len<2 || val.buf[0]!='"' || val.buf[val.len-1]!='"') return 0;
      for (size_t i=1;i+1<val.len;i++) if (val.buf[i]=='\\' || (unsigned char)val.buf[i]<32) return 0;
    } else if (mg_strcmp(val,mg_str("true")) && mg_strcmp(val,mg_str("false"))) return 0;
  }
  return (seen & 15) == 15;
}

static void lan_status(struct mg_connection *c) {
  LanConfig v;
  int supported = read_live(&v);
  JsonBuilder *j = json_new(); json_obj_open(j); json_add_int(j,"Code",0);
  json_key_obj_open(j,"Data"); json_add_bool(j,"supported",supported);
  if (supported) {
    json_add_str(j,"interface","tether"); json_add_str(j,"address",v.address); json_add_str(j,"netmask",v.mask);
    uint32_t mask; ipv4(v.mask,&mask); int prefix = 0; for (;mask;mask <<= 1) prefix++;
    json_add_int(j,"prefix",prefix); json_add_str(j,"pool_start",v.start); json_add_str(j,"pool_end",v.end);
    char *version = config_string(&v); json_add_str(j,"version",version); g_free(version);
    json_add_bool(j,"pool_writable",lan_address_present(&v));
    json_arr_open(j,"writable_fields"); json_arr_add_str(j,"pool_start"); json_arr_add_str(j,"pool_end"); json_arr_close(j);
    json_arr_open(j,"dns");
    FILE *f = fopen("/etc/resolv.conf","r"); char *line = NULL; size_t cap = 0;
    GHashTable *seen = g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    while (f && getline(&line,&cap,f) >= 0) {
      char ip[INET6_ADDRSTRLEN], extra; unsigned char binary[16];
      if (sscanf(line,"nameserver %45s %c",ip,&extra) == 1 &&
          (inet_pton(AF_INET,ip,binary)==1 || inet_pton(AF_INET6,ip,binary)==1) && !g_hash_table_contains(seen,ip)) {
        json_arr_add_str(j,ip); g_hash_table_add(seen,g_strdup(ip));
      }
    }
    if (f) fclose(f); free(line); g_hash_table_destroy(seen); json_arr_close(j);
  }
  json_obj_close(j); json_obj_close(j); HTTP_OK_FREE(c,json_finish(j));
}

static void lan_save(struct mg_connection *c, struct mg_http_message *hm) {
  if (!valid_request(hm->body)) { HTTP_ERROR(c,400,"invalid or unsupported LAN fields"); return; }
  LanConfig old, next, actual;
  char *start = mg_json_get_str(hm->body,"$.pool_start"), *end = mg_json_get_str(hm->body,"$.pool_end");
  char *version = mg_json_get_str(hm->body,"$.version");
  int confirm = 0, validate = 0;
  mg_json_get_bool(hm->body,"$.confirm",&confirm); mg_json_get_bool(hm->body,"$.validate_only",&validate);
  if (!read_live(&old)) { HTTP_ERROR(c,503,"CW52 ConnMan DHCP interface unavailable"); goto done; }
  next = old;
  if (!start || !end || !ipv4(start,NULL) || !ipv4(end,NULL)) { HTTP_ERROR(c,400,"invalid IPv4 pool"); goto done; }
  g_strlcpy(next.start,start,16); g_strlcpy(next.end,end,16);
  if (!valid_pool(&next)) { HTTP_ERROR(c,400,"pool must be usable LAN addresses, ordered, and exclude the router"); goto done; }
  char *current = config_string(&old);
  int fresh = version && !strcmp(version,current); g_free(current);
  if (!fresh) { HTTP_ERROR(c,409,"LAN state changed; refresh before saving"); goto done; }
  if (!confirm) { HTTP_ERROR(c,400,"explicit confirmation required"); goto done; }
  if (!lan_address_present(&old)) { HTTP_ERROR(c,409,"LAN configuration differs from management interface"); goto done; }
  if (validate) { HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"valid; no changes applied\"}"); goto done; }
  if (same_config(&old,&next)) { HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"unchanged\"}"); goto done; }

  int lock = open("/run/udx710-lan.lock",O_WRONLY|O_CREAT|O_EXCL,0600);
  if (lock < 0) { HTTP_ERROR(c,409,"LAN save locked; inspect recovery before retrying"); goto done; }
  close(lock);
  char *original = NULL, *patched = NULL, *dir = NULL; gsize len = 0, plen = 0;
  struct stat st; GKeyFile *kf = g_key_file_new(); pid_t watcher = -1;
  int prepared = 0, committed_ok = 0;
  if (lstat(LAN_FILE,&st) || !S_ISREG(st.st_mode) || st.st_size > 65536 ||
      !g_file_get_contents(LAN_FILE,&original,&len,NULL) ||
      !g_key_file_load_from_data(kf,original,len,G_KEY_FILE_KEEP_COMMENTS,NULL)) goto failed;
  const char *names[] = {"gateway_ip","mask","start_ip","end_ip"};
  const char *values[] = {old.address,old.mask,old.start,old.end};
  for (int i=0;i<4;i++) {
    char *v = g_key_file_get_string(kf,"global",names[i],NULL);
    int match = v && !strcmp(v,values[i]); g_free(v); if (!match) goto failed;
  }
  if (!g_key_file_has_key(kf,"global","ttl",NULL) ||
      g_key_file_get_integer(kf,"global","ttl",NULL) != old.ttl || !management_reachable(&old)) goto failed;
  struct stat mountst, parentst;
  if (stat("/mnt/data",&mountst) || stat("/mnt",&parentst) || mountst.st_dev == parentst.st_dev) goto failed;
  dir = g_strdup(LAN_BACKUPS "/lan-XXXXXX"); if (!mkdtemp(dir)) goto failed;
  char *backup = g_build_filename(dir,"dhcp_config.ini.before",NULL);
  int backed = atomic_file(backup,original,len,&st); g_free(backup); if (!backed) goto failed;
  g_key_file_set_string(kf,"global","start_ip",next.start); g_key_file_set_string(kf,"global","end_ip",next.end);
  patched = g_key_file_to_data(kf,&plen,NULL); if (!patched) goto failed;
  watcher = start_watchdog(dir,&old); if (watcher < 0) goto failed;
  prepared = 1;
  if (!atomic_file(LAN_FILE,patched,plen,&st)) goto failed;
  if (!apply_live(&next) || !read_live(&actual) || !same_config(&actual,&next) || !management_reachable(&old)) goto failed;
  char *commit = g_build_filename(dir,"committed",NULL);
  struct stat private = {.st_mode=0600,.st_uid=geteuid(),.st_gid=getegid()};
  int committed = atomic_file(commit,"verified\n",9,&private);
  if (!committed) unlink(commit);
  g_free(commit); if (!committed) goto failed;
  committed_ok = 1;
  HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"DHCP pool saved and verified\"}");
  goto cleanup;
failed:
  if (prepared) {
    int restored = apply_live(&old) && read_live(&actual) && same_config(&actual,&old);
    if (!atomic_file(LAN_FILE,original,len,&st)) restored = 0;
    HTTP_ERROR(c,500,restored ? "apply failed; previous configuration restored" : "apply failed; recovery watchdog will retry; use saved rollback.sh");
  } else HTTP_ERROR(c,500,"recovery preparation failed; LAN was not changed");
cleanup:
  /* A failed apply keeps the independent watchdog and lock until recovery. */
  if (!prepared || committed_ok) unlink("/run/udx710-lan.lock");
  g_key_file_free(kf); g_free(original); g_free(patched); g_free(dir);
done:
  free(start); free(end); free(version);
}

void handle_lan(struct mg_connection *c, struct mg_http_message *hm) {
  HTTP_CHECK_ANY(c,hm);
  if (http_is_method(hm,"GET")) lan_status(c);
  else if (http_is_method(hm,"POST")) lan_save(c,hm);
  else http_method_error(c);
}
