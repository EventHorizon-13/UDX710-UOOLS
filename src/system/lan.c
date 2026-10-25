/* CW52's vendor ConnMan TetheringDHCP interface, confirmed against libmumu.
 * IPv4 changes are staged, keep a maintenance alias and require new-IP confirmation.
 * Do not toggle Tethering: that also controls the USB management transport. */
#include <gio/gio.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include "http_utils.h"
#include "json_builder.h"
#include "lan.h"
#include "exec_utils.h"

#define LAN_FILE "/var/mm/dhcp_config.ini"
#define LAN_BACKUPS "/mnt/data/local-udx710"
#define LAN_RECOVERY_IP "192.168.1.1"
#define DHCP_RELOAD_BOOT LAN_BACKUPS "/lan-dhcp-reload.boot"
#define DHCP_RELOAD_RESTORE \
  "if [ -f dhcp-reload.before ]; then\n" \
  " cp -p dhcp-reload.before " DHCP_RELOAD_BOOT ".rollback && mv -f " DHCP_RELOAD_BOOT ".rollback " DHCP_RELOAD_BOOT " || exit 1\n" \
  "elif [ -f dhcp-reload.absent ]; then\n" \
  " rm -f " DHCP_RELOAD_BOOT " || exit 1\nelse exit 1\nfi\n"
#define LAN_RECOVERY_HELPER "/home/root/6677/cw52-lan-recovery.sh"
static int recovery_present(void);
static int ensure_recovery(void);
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

static int mask_prefix_u32(uint32_t mask) {
  int prefix=0;
  while(mask&0x80000000U){prefix++;mask<<=1;}
  return mask?-1:prefix;
}

static int valid_pool(const LanConfig *v) {
  uint32_t a, m, s, e;
  if (!ipv4(v->address, &a) || !ipv4(v->mask, &m) ||
      !ipv4(v->start, &s) || !ipv4(v->end, &e)) return 0;
  uint32_t host = ~m;
  if (!m || host < 3 || (host & (host + 1)) ||
      (a & m) != (s & m) || (a & m) != (e & m)) return 0;
  uint32_t recovery=0xc0a80101U;
  if (a!=recovery && s<=recovery && recovery<=e) return 0;
  return (a & host) && (a & host)!=host && s <= e && (s & host) && (e & host) != host && !(s <= a && a <= e);
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

/* This firmware stores TetheringDHCP parameters without reloading the running
 * embedded DHCP server. Keep its gate closed until a subsequent device boot. */
static int dhcp_reload_required(void) {
  char *saved=NULL,*boot=NULL;gsize len=0;struct stat st;
  if(lstat(DHCP_RELOAD_BOOT,&st))return errno==ENOENT?0:-1;
  if(!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>64||
     !g_file_get_contents(DHCP_RELOAD_BOOT,&saved,&len,NULL))return -1;
  int ok=g_file_get_contents("/proc/sys/kernel/random/boot_id",&boot,NULL,NULL);
  int result=ok?!strcmp(saved,boot):-1;g_free(saved);g_free(boot);return result;
}
static int backup_dhcp_reload(const char *dir) {
  char *saved=NULL;gsize len=0;struct stat st;
  int present=!lstat(DHCP_RELOAD_BOOT,&st);
  if(!present && errno!=ENOENT)return 0;
  if(present && (!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>64||
      !g_file_get_contents(DHCP_RELOAD_BOOT,&saved,&len,NULL)))return 0;
  char *path=g_build_filename(dir,present?"dhcp-reload.before":"dhcp-reload.absent",NULL);
  struct stat private={.st_mode=0600,.st_uid=geteuid(),.st_gid=getegid()};
  int ok=atomic_file(path,present?saved:"",present?len:0,&private);
  g_free(saved);g_free(path);return ok;
}
static int mark_dhcp_reload(void) {
  char *boot=NULL;gsize len=0;
  if(!g_file_get_contents("/proc/sys/kernel/random/boot_id",&boot,&len,NULL))return 0;
  struct stat private={.st_mode=0600,.st_uid=geteuid(),.st_gid=getegid()};
  int ok=atomic_file(DHCP_RELOAD_BOOT,boot,len,&private);g_free(boot);return ok;
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

static int read_persisted(LanConfig *v) {
  char *data=NULL; gsize len=0;
  GKeyFile *kf=g_key_file_new();
  int ok=g_file_get_contents(LAN_FILE,&data,&len,NULL) &&
    g_key_file_load_from_data(kf,data,len,G_KEY_FILE_KEEP_COMMENTS,NULL);
  char *s=g_key_file_get_string(kf,"global","gateway_ip",NULL);
  char *m=g_key_file_get_string(kf,"global","mask",NULL);
  char *start=g_key_file_get_string(kf,"global","start_ip",NULL);
  char *end=g_key_file_get_string(kf,"global","end_ip",NULL);
  ok=ok && s && m && start && end &&
    g_key_file_has_key(kf,"global","ttl",NULL);
  if(ok) {
    g_strlcpy(v->address,s,16);g_strlcpy(v->mask,m,16);
    g_strlcpy(v->start,start,16);g_strlcpy(v->end,end,16);
    v->ttl=g_key_file_get_integer(kf,"global","ttl",NULL);
    ok=valid_pool(v);
  }
  g_free(s);g_free(m);g_free(start);g_free(end);g_free(data);
  g_key_file_free(kf);return ok;
}

static int lan_addr_is_managed(const LanConfig *v,uint32_t address) {
  uint32_t wanted=0,recovery=0;
  ipv4(v->address,&wanted);ipv4(LAN_RECOVERY_IP,&recovery);
  return address==wanted || address==recovery;
}

/* Reconcile a stale kernel address left by the vendor after reboot.  Only
 * addresses observed on tether and not equal to the persisted LAN or the
 * maintenance alias are touched; the same address is also removed from the
 * bridge's member interfaces so it cannot win a same-subnet route lookup. */
static int remove_stale_lan_addresses(const LanConfig *v) {
  struct ifaddrs *all;
  if(getifaddrs(&all))return -1;
  typedef struct { uint32_t address,mask; } StaleAddress;
  GArray *stale=g_array_new(FALSE,FALSE,sizeof(StaleAddress));
  for(struct ifaddrs *p=all;p;p=p->ifa_next) {
    if(!p->ifa_addr||!p->ifa_netmask||p->ifa_addr->sa_family!=AF_INET||
       strcmp(p->ifa_name,"tether"))continue;
    uint32_t address=ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
    uint32_t mask=ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
    if(lan_addr_is_managed(v,address))continue;
    StaleAddress item={address,mask};g_array_append_val(stale,item);
  }
  freeifaddrs(all);
  const char *interfaces[]={"tether","sipa_usb0","usb0","wlan0","wlan1"};
  int changed=0,ok=1;
  for(guint i=0;i<stale->len&&ok;i++) {
    StaleAddress item=g_array_index(stale,StaleAddress,i);
    int prefix=mask_prefix_u32(item.mask);
    if(prefix<0) {ok=0;break;}
    struct in_addr raw={.s_addr=htonl(item.address)};
    char ip[INET_ADDRSTRLEN],cidr[32],out[1024];
    if(!inet_ntop(AF_INET,&raw,ip,sizeof(ip))) {ok=0;break;}
    snprintf(cidr,sizeof(cidr),"%s/%d",ip,prefix);
    for(size_t n=0;n<sizeof(interfaces)/sizeof(interfaces[0]);n++) {
      if(run_command(out,sizeof(out),"/sbin/ip","-4","address","del",cidr,"dev",(char *)interfaces[n],NULL)==0) {
        changed=1;
      }
    }
  }
  g_array_free(stale,TRUE);
  return ok?changed:-1;
}

static int lan_kernel_consistent(const LanConfig *v) {
  struct ifaddrs *all;
  if(getifaddrs(&all))return 0;
  uint32_t wanted=0;
  if(!ipv4(v->address,&wanted)){freeifaddrs(all);return 0;}
  int desired=0,stale=0;
  for(struct ifaddrs *p=all;p;p=p->ifa_next) {
    if(!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET||strcmp(p->ifa_name,"tether"))continue;
    uint32_t address=ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
    if(address==wanted)desired=1;
    else if(!lan_addr_is_managed(v,address))stale=1;
  }
  freeifaddrs(all);return desired&&!stale;
}

static int schedule_tethering_reload(void) {
  const char *path=LAN_BACKUPS "/tethering-reload.sh";
  const char *log=LAN_BACKUPS "/tethering-reload.log";
  char *script=g_strdup_printf(
    "#!/bin/sh\nL=%s\necho \"$(date +%%s) runtime reload requested\" >\"$L\"\n"
    "sleep 3\n"
    "dbus-send --system --print-reply --reply-timeout=10000 --dest=net.connman "
    LAN_TECH " net.connman.Technology.SetProperty string:Tethering variant:boolean:false >>\"$L\" 2>&1 || exit 1\n"
    "sleep 5\n"
    "n=0\n"
    "while [ $n -lt 5 ]; do\n"
    " dbus-send --system --print-reply --reply-timeout=10000 --dest=net.connman "
    LAN_TECH " net.connman.Technology.SetProperty string:Tethering variant:boolean:true >>\"$L\" 2>&1 && exit 0\n"
    " n=$((n+1)); sleep 2\n"
    "done\n"
    "exit 1\n",log);
  struct stat st={.st_mode=0700,.st_uid=geteuid(),.st_gid=getegid()};
  int ok=atomic_file(path,script,strlen(script),&st);
  pid_t pid=ok?fork():-1;
  if(pid==0) {
    setsid();int fd=open("/dev/null",O_RDWR);
    if(fd>=0){dup2(fd,0);dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);}
    execl("/bin/sh","sh",path,(char *)NULL);_exit(127);
  }
  g_free(script);return pid>0;
}

/* Probe the terminal where start.sh binds it: the saved LAN address.
 * Keep the recovery-address fallback for older ttyd installations. */
static int management_tcp_reachable(const char *address, unsigned short port) {
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, address, &a.sin_addr) != 1) return 0;
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) return 0;
  int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
  if (rc && errno == EINPROGRESS) {
    fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf); struct timeval tv = {1, 0};
    int err = 1; socklen_t n = sizeof(err);
    rc = select(fd + 1, NULL, &wf, NULL, &tv) > 0 &&
      !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) && !err ? 0 : -1;
  }
  close(fd);
  return rc == 0;
}

static int management_services_reachable(const char *lan, const char *recovery,
                                         unsigned short app_port, unsigned short maintenance_port) {
  /* Address confirmation is made in the UDX app on 6677. The ttyd helper
   * checks its own rebind separately; requiring a TCP probe to its listener
   * here can reject a valid LAN address when it is also the recovery address. */
  return management_tcp_reachable(lan, app_port) &&
    management_tcp_reachable(recovery, maintenance_port);
}

#define LAN_TERMINAL_REBIND "/home/root/6677/cw52-ttyd-rebind.sh"
static int rebind_terminal(void) {
  char out[1024];
  return !run_command(out,sizeof(out),"/bin/sh",LAN_TERMINAL_REBIND,NULL);
}

static int management_reachable(const LanConfig *v) {
  return lan_address_present(v) && recovery_present() &&
    management_services_reachable(v->address, LAN_RECOVERY_IP, 6677, 80);
}

/* Independent executable watchdog survives server exit. Recovery materials are
 * durable BEFORE any network write. */
static pid_t start_watchdog(const char *dir, const LanConfig *old) {
  char *value = config_string(old);
  char *script = g_strdup_printf(
    "#!/bin/sh\ncd '%s' || exit 1\n"
    "restore() {\n" DHCP_RELOAD_RESTORE " dbus-send --system --print-reply --reply-timeout=4000 --dest=net.connman "
    LAN_TECH " net.connman.Technology.SetProperty string:TetheringDHCP variant:string:'%s' >dbus-rollback.log 2>&1\n"
    " cp -p dhcp_config.ini.before " LAN_FILE ".rollback.tmp && mv -f " LAN_FILE ".rollback.tmp " LAN_FILE "\n /bin/sh " LAN_RECOVERY_HELPER "\n sync\n}\n"
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

/* The vendor DHCP server shares tether across Ethernet, WiFi and USB. Gate
 * only requests delivered to this host; never stop ConnMan or Tethering. */
#define DHCP_STATE LAN_BACKUPS "/lan-dhcp.state"
#define DHCP_CHAIN "CW52_LAN_DHCP"
#define DHCP_JUMP "-A INPUT -i tether -p udp -m udp --dport 67 -j " DHCP_CHAIN
#define DHCP_IPTABLES "/usr/sbin/iptables"
typedef struct { int actual, desired, had_file, reload_required; char version[65]; } DhcpState;

/* Refuse foreign/partial rules rather than overwriting an unrelated chain. */
static int dhcp_parse_rules(const char *rules, int *enabled) {
  char **lines = g_strsplit(rules,"\n",-1); int chain=0,drop=0,jump=0,bad=0;
  for (char **p=lines;*p;p++) {
    if (!strcmp(*p,"-N " DHCP_CHAIN)) chain++;
    else if (!strcmp(*p,"-A " DHCP_CHAIN " -j DROP")) drop++;
    else if (!strcmp(*p,DHCP_JUMP)) jump++;
    else if (strstr(*p,DHCP_CHAIN)) bad=1;
  }
  g_strfreev(lines);
  if (bad || chain>1 || drop>1 || jump>1) return 0;
  if (!chain && !drop && !jump) { *enabled=1; return 1; }
  if (chain==1 && drop==1 && jump==1) { *enabled=0; return 1; }
  return 0;
}

static int dhcp_read(DhcpState *s) {
  char output[16384]; memset(s,0,sizeof(*s)); s->desired=1;
  if (run_command(output,sizeof(output),DHCP_IPTABLES,"-w","2","-S",NULL) ||
      strlen(output)>=sizeof(output)-1 || !dhcp_parse_rules(output,&s->actual)) return 0;
  char *text=NULL; gsize n=0;
  if (g_file_get_contents(DHCP_STATE,&text,&n,NULL)) {
    s->had_file=1;
    if (n==8 && !memcmp(text,"enabled\n",8)) s->desired=1;
    else if (n==9 && !memcmp(text,"disabled\n",9)) s->desired=0;
    else { g_free(text); return 0; }
    g_free(text);
  } else if (access(DHCP_STATE,F_OK)==0 || errno!=ENOENT) return 0;
  s->reload_required=dhcp_reload_required();if(s->reload_required<0)return 0;
  char *boot=NULL;g_file_get_contents("/proc/sys/kernel/random/boot_id",&boot,NULL,NULL);
  char *input=g_strdup_printf("v2:%d:%d:%d:%d:%s",s->actual,s->desired,s->had_file,s->reload_required,boot?boot:"");
  char *hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,input,-1);
  g_strlcpy(s->version,hash,sizeof(s->version)); g_free(hash); g_free(input); g_free(boot);
  return 1;
}

static int dhcp_rules_apply(int enabled) {
  char out[16384]; int actual;
  if (run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-S",NULL) ||
      !dhcp_parse_rules(out,&actual)) return 0;
  if (actual==enabled) return 1;
  if (!enabled) {
    if (run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-N",DHCP_CHAIN,NULL) ||
        run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-A",DHCP_CHAIN,"-j","DROP",NULL) ||
        run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-I","INPUT","1","-i","tether","-p","udp","--dport","67","-j",DHCP_CHAIN,NULL)) return 0;
  } else {
    if (run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-D","INPUT","-i","tether","-p","udp","--dport","67","-j",DHCP_CHAIN,NULL) ||
        run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-F",DHCP_CHAIN,NULL) ||
        run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-X",DHCP_CHAIN,NULL)) return 0;
  }
  return !run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-S",NULL) &&
    dhcp_parse_rules(out,&actual) && actual==enabled;
}

static int dhcp_valid_request(struct mg_str body) {
  int len=0;
  if (body.len>1024 || mg_json_get(body,"$",&len)!=0 || (size_t)len!=body.len || body.buf[0]!='{') return 0;
  const char *names[]={"\"action\"","\"enabled\"","\"version\"","\"confirm\""};
  unsigned seen=0; size_t off=0; struct mg_str key,val;
  while ((off=mg_json_next(body,off,&key,&val))) {
    int which=-1;
    for (int i=0;i<4;i++) if (!mg_strcmp(key,mg_str(names[i]))) which=i;
    if (which<0 || (seen&(1U<<which))) return 0;
    seen|=1U<<which;
    if (which==1 || which==3) {
      if (mg_strcmp(val,mg_str("true")) && mg_strcmp(val,mg_str("false"))) return 0;
    } else {
      if (val.len<2 || val.buf[0]!='"' || val.buf[val.len-1]!='"') return 0;
      for (size_t i=1;i+1<val.len;i++) if (val.buf[i]=='\\' || (unsigned char)val.buf[i]<32) return 0;
    }
  }
  char *action=mg_json_get_str(body,"$.action"); int ok=seen==15 && action && !strcmp(action,"dhcp"); free(action);
  return ok;
}

/* Durable, self-contained rollback does not depend on the server process. */
static char *dhcp_watchdog_script(const DhcpState *old) {
  const char *restore_rules=old->actual ?
    "iptables -w 2 -D INPUT -i tether -p udp --dport 67 -j " DHCP_CHAIN " 2>/dev/null || :\n"
    "iptables -w 2 -F " DHCP_CHAIN " 2>/dev/null || :\n"
    "iptables -w 2 -X " DHCP_CHAIN " 2>/dev/null || :\n" :
    "iptables -w 2 -N " DHCP_CHAIN " 2>/dev/null || :\n"
    "iptables -w 2 -F " DHCP_CHAIN " && iptables -w 2 -A " DHCP_CHAIN " -j DROP\n"
    "iptables -w 2 -C INPUT -i tether -p udp --dport 67 -j " DHCP_CHAIN " 2>/dev/null || "
    "iptables -w 2 -I INPUT 1 -i tether -p udp --dport 67 -j " DHCP_CHAIN "\n";
  const char *restore_file=old->had_file ?
    "cp -p state.before " DHCP_STATE ".rollback && mv -f " DHCP_STATE ".rollback " DHCP_STATE "\n" :
    "rm -f " DHCP_STATE "\n";
  return g_strdup_printf("#!/bin/sh\ncd \"$(dirname \"$0\")\" || exit 1\n"
    "if [ \"$1\" != --now ]; then n=0; while [ $n -lt 30 ]; do [ ! -e committed ] || exit 0; sleep 1; n=$((n+1)); done; fi\n"
    "%s%s"
    "sync\n",restore_rules,restore_file);
}

static int dhcp_run_rollback(const char *dir) {
  char *path=g_build_filename(dir,"rollback.sh",NULL),out[1024];
  int ok=!run_command(out,sizeof(out),"/bin/sh",path,"--now",NULL); g_free(path); return ok;
}

static void dhcp_save(struct mg_connection *c, struct mg_http_message *hm) {
  if (!dhcp_valid_request(hm->body)) { HTTP_ERROR(c,400,"invalid DHCP switch request"); return; }
  bool enabled=false,confirm=false; char *version=mg_json_get_str(hm->body,"$.version");
  mg_json_get_bool(hm->body,"$.enabled",&enabled); mg_json_get_bool(hm->body,"$.confirm",&confirm);
  DhcpState old,actual; LanConfig lan;
  if (!confirm) { HTTP_ERROR(c,400,"explicit confirmation required"); goto done; }
  if (!dhcp_read(&old) || !read_live(&lan)) { HTTP_ERROR(c,503,"DHCP state unavailable; no changes applied"); goto done; }
  if (!version || strcmp(version,old.version)) { HTTP_ERROR(c,409,"DHCP state changed; refresh before switching"); goto done; }
  if (old.actual!=old.desired) { HTTP_ERROR(c,409,"DHCP saved and runtime state differ; inspect recovery"); goto done; }
  if (enabled && old.reload_required) { HTTP_ERROR(c,409,"restart CW52 before enabling DHCP after a LAN configuration change");goto done; }
  if (old.actual==enabled) { HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"unchanged\"}"); goto done; }
  if (!management_reachable(&lan)) { HTTP_ERROR(c,409,"management connection check failed; no changes applied"); goto done; }
  int lock=open("/run/udx710-lan.lock",O_WRONLY|O_CREAT|O_EXCL,0600);
  if (lock<0) { HTTP_ERROR(c,409,"LAN save locked; inspect recovery before retrying"); goto done; }
  close(lock);
  char *dir=NULL,*script=NULL,*path=NULL; int prepared=0,ok=0; pid_t pid=-1;
  struct stat st={.st_mode=0600,.st_uid=geteuid(),.st_gid=getegid()},mnt,parent;
  if (stat("/mnt/data",&mnt) || stat("/mnt",&parent) || mnt.st_dev==parent.st_dev) goto failed;
  dir=g_strdup(LAN_BACKUPS "/dhcp-switch-XXXXXX"); if (!mkdtemp(dir)) goto failed;
  if (old.had_file) {
    path=g_build_filename(dir,"state.before",NULL);
    const char *value=old.desired?"enabled\n":"disabled\n";
    ok=atomic_file(path,value,strlen(value),&st);g_free(path);path=NULL;if (!ok) goto failed;
  }
  script=dhcp_watchdog_script(&old); path=g_build_filename(dir,"rollback.sh",NULL);
  st.st_mode=0700;ok=atomic_file(path,script,strlen(script),&st);if (!ok) goto failed;
  pid=fork();
  if (pid<0) goto failed;
  if (!pid) {
    setsid();int fd=open("/dev/null",O_RDWR);
    if (fd>=0) { dup2(fd,0);dup2(fd,1);dup2(fd,2);if(fd>2)close(fd); }
    execl("/bin/sh","sh",path,(char *)NULL);_exit(127);
  }
  prepared=1;g_free(path);path=NULL;
  if (!dhcp_rules_apply(enabled) || !management_reachable(&lan)) goto failed;
  const char *value=enabled?"enabled\n":"disabled\n";st.st_mode=0600;
  if (!atomic_file(DHCP_STATE,value,strlen(value),&st) || !dhcp_read(&actual) ||
      actual.actual!=enabled || actual.desired!=enabled) goto failed;
  path=g_build_filename(dir,"committed",NULL);
  if (!atomic_file(path,"verified\n",9,&st)) { unlink(path);goto failed; }
  unlink("/run/udx710-lan.lock");
  HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"LAN DHCP setting saved and verified\"}");goto cleanup;
failed:
  if (prepared) {
    int restored=dhcp_run_rollback(dir) && dhcp_read(&actual) && actual.actual==old.actual && actual.desired==old.desired;
    /* Let the watchdog retry an incomplete recovery. */
    if (restored) {
      g_free(path);path=g_build_filename(dir,"committed",NULL);st.st_mode=0600;
      if (atomic_file(path,"restored\n",9,&st)) unlink("/run/udx710-lan.lock");
    }
    HTTP_ERROR(c,500,restored?"DHCP change failed; previous state restored":"DHCP change failed; recovery watchdog will retry");
  } else { unlink("/run/udx710-lan.lock");HTTP_ERROR(c,500,"DHCP recovery preparation failed; no changes applied"); }
cleanup:
  g_free(path);g_free(script);g_free(dir);
done:
  free(version);
}

void lan_dhcp_init(void) {
  DhcpState s;
  if (dhcp_read(&s) && s.had_file && s.actual!=s.desired && !dhcp_rules_apply(s.desired))
    fprintf(stderr,"LAN DHCP restore failed; inspect saved/runtime state\n");
}

#define LAN_RECOVERY_NET "192.168.1.0/24"
#define LAN_PENDING LAN_BACKUPS "/lan-address.pending"
#define LAN_ADDRESS_TAG "cw52-lan-address"
typedef struct { char dir[256]; LanConfig old,next; long long deadline; int same_boot; } AddressMigration;

static int mask_prefix(const char *text) {
  uint32_t mask; if (!ipv4(text,&mask) || !mask || ((~mask)&((~mask)+1))) return -1;
  int prefix=0;for (;mask;mask<<=1) prefix++;return prefix;
}
static int private_network(uint32_t address,uint32_t mask) {
  uint32_t first=address&mask,last=first|~mask;
  return (first>=0x0a000000U && last<=0x0affffffU) ||
    (first>=0xac100000U && last<=0xac1fffffU) || (first>=0xc0a80000U && last<=0xc0a8ffffU);
}
static int valid_address_config(const LanConfig *v) {
  uint32_t a,m,s,e,recovery;int prefix=mask_prefix(v->mask);
  if (prefix<8 || prefix>30 || !ipv4(v->address,&a) || !ipv4(v->mask,&m) || !valid_pool(v)) return 0;
  if (!(a&~m) || (a&~m)==~m || !private_network(a,m)) return 0;
  ipv4(v->start,&s);ipv4(v->end,&e);ipv4(LAN_RECOVERY_IP,&recovery);
  return a==recovery || recovery<s || recovery>e;
}
static int cellular_interface(const char *name) {
  if(strncmp(name,"sipa_eth",8)||!name[8])return 0;
  for(const char *p=name+8;*p;p++)if(!g_ascii_isdigit(*p))return 0;
  return 1;
}
/* A strictly narrower LAN wins over the cellular connected route. Never allow
 * the LAN to contain the cellular endpoint or overlap another LAN interface. */
static int interface_subnet_conflict(const char *name,uint32_t a,uint32_t m,uint32_t other,uint32_t other_mask) {
  uint32_t host=~other_mask;
  if(!other_mask || (host&(host+1)))return 1;
  uint32_t first=a&m,last=first|~m,other_first=other&other_mask,other_last=other_first|host;
  if(first>other_last || other_first>last)return 0;
  if(cellular_interface(name) && m>other_mask && (other&m)!=first &&
     other_first<=first && last<=other_last)return 0;
  return 1;
}
static int address_conflicts_locally(const LanConfig *v) {
  uint32_t a,m;ipv4(v->address,&a);ipv4(v->mask,&m);
  struct ifaddrs *all;if(getifaddrs(&all))return 1;int conflict=0;
  for(struct ifaddrs *p=all;p;p=p->ifa_next) {
    if(!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family!=AF_INET ||
       p->ifa_netmask->sa_family!=AF_INET || !strcmp(p->ifa_name,"tether") || !strcmp(p->ifa_name,"lo"))continue;
    uint32_t n=ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
    uint32_t mask=ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
    if(interface_subnet_conflict(p->ifa_name,a,m,n,mask))conflict=1;
  }
  freeifaddrs(all);return conflict;
}
static char *address_cidr(const LanConfig *v,int network) {
  uint32_t a,m;ipv4(v->address,&a);ipv4(v->mask,&m);struct in_addr n={.s_addr=htonl(network?(a&m):a)};
  char ip[16];inet_ntop(AF_INET,&n,ip,sizeof(ip));return g_strdup_printf("%s/%d",ip,mask_prefix(v->mask));
}
static int install_lan_address(const LanConfig *v) {
  if(lan_address_present(v))return 1;
  char *cidr=address_cidr(v,0),out[1024];
  int ok=!run_command(out,sizeof(out),"/sbin/ip","-4","address","replace",cidr,"dev","tether",NULL);
  g_free(cidr);return ok && lan_address_present(v);
}
static int lan_route_reaches_tether(const LanConfig *v) {
  char out[2048];
  if(run_command(out,sizeof(out),"/sbin/ip","-4","route","get",(char *)v->start,NULL) ||
     strlen(out)>=sizeof(out)-1)return 0;
  char **words=g_strsplit_set(out," \t\r\n",-1);int ok=0;
  for(int i=0;words[i] && words[i+1];i++)if(!strcmp(words[i],"dev") && !strcmp(words[i+1],"tether"))ok=1;
  g_strfreev(words);return ok;
}
static int recovery_present(void) {
  struct ifaddrs *all;if(getifaddrs(&all))return 0;int found=0;struct in_addr wanted;inet_pton(AF_INET,LAN_RECOVERY_IP,&wanted);
  for(struct ifaddrs *p=all;p;p=p->ifa_next)
    if(p->ifa_addr && p->ifa_addr->sa_family==AF_INET && !strcmp(p->ifa_name,"tether") &&
       ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr==wanted.s_addr)found=1;
  freeifaddrs(all);return found;
}
static int ensure_recovery(void) {
  if(recovery_present())return 1;char out[1024];
  return !run_command(out,sizeof(out),"/sbin/ip","-4","address","add",LAN_RECOVERY_IP "/24","dev","tether",NULL) && recovery_present();
}
static int web_subnet_rule(const LanConfig *v,int add) {
  char *cidr=address_cidr(v,1),out[2048];int ok=1;
  if(strcmp(cidr,LAN_RECOVERY_NET)) {
    int exists=!run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-C","CW52_UDX_WEB","-i","tether","-s",cidr,"-m","comment","--comment",LAN_ADDRESS_TAG,"-j","RETURN",NULL);
    if(add && !exists)ok=!run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-I","CW52_UDX_WEB","1","-i","tether","-s",cidr,"-m","comment","--comment",LAN_ADDRESS_TAG,"-j","RETURN",NULL);
    if(!add && exists)ok=!run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-D","CW52_UDX_WEB","-i","tether","-s",cidr,"-m","comment","--comment",LAN_ADDRESS_TAG,"-j","RETURN",NULL);
  }
  g_free(cidr);return ok;
}
/* Tagged NAT rules preserve Internet access for the selected LAN and the
 * maintenance subnet even if the vendor keeps its old pool-specific rule. */
static int nat_cidr_rule(const char *cidr,const char *tag,int add) {
  char out[2048];int exists=!run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-t","nat","-C","POSTROUTING","-s",(char *)cidr,"-o","sipa_eth+","-m","comment","--comment",(char *)tag,"-j","MASQUERADE",NULL);
  if(add && !exists)return !run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-t","nat","-A","POSTROUTING","-s",(char *)cidr,"-o","sipa_eth+","-m","comment","--comment",(char *)tag,"-j","MASQUERADE",NULL);
  if(!add && exists)return !run_command(out,sizeof(out),DHCP_IPTABLES,"-w","2","-t","nat","-D","POSTROUTING","-s",(char *)cidr,"-o","sipa_eth+","-m","comment","--comment",(char *)tag,"-j","MASQUERADE",NULL);
  return 1;
}
static int nat_subnet_rule(const LanConfig *v,int add) {
  char *cidr=address_cidr(v,1);int ok=!strcmp(cidr,LAN_RECOVERY_NET) || nat_cidr_rule(cidr,LAN_ADDRESS_TAG,add);g_free(cidr);return ok;
}
static int nat_recovery_rule(const LanConfig *v) {
  char *cidr=address_cidr(v,1);int ok=nat_cidr_rule(LAN_RECOVERY_NET,"cw52-lan-recovery",strcmp(cidr,LAN_RECOVERY_NET)!=0);g_free(cidr);return ok;
}
static long long monotonic_seconds(void) { struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (long long)t.tv_sec; }
static int read_small_file(const char *path,char **out,size_t limit) {
  struct stat st;gsize n=0;
  return !lstat(path,&st) && S_ISREG(st.st_mode) && st.st_size>=0 && (size_t)st.st_size<limit &&
    g_file_get_contents(path,out,&n,NULL) && n<limit;
}
static int safe_migration_dir(const char *s) {
  const char *prefix=LAN_BACKUPS "/lan-address-";size_t n=strlen(prefix);
  if(strncmp(s,prefix,n) || strlen(s)!=n+6)return 0;
  for(size_t i=n;s[i];i++)if(!g_ascii_isalnum(s[i]))return 0;
  return 1;
}
static int read_migration(AddressMigration *m) {
  memset(m,0,sizeof(*m));char *dir=NULL,*before=NULL,*after=NULL,*deadline=NULL,*boot=NULL,*current=NULL,*path=NULL;int result=-1;
  if(!read_small_file(LAN_PENDING,&dir,256)) { if(access(LAN_PENDING,F_OK) && errno==ENOENT)result=0;goto done; }
  g_strchomp(dir);if(!safe_migration_dir(dir))goto done;g_strlcpy(m->dir,dir,sizeof(m->dir));
  path=g_build_filename(dir,"before",NULL);int ok=read_small_file(path,&before,256);g_free(path);path=NULL;if(!ok)goto done;
  path=g_build_filename(dir,"after",NULL);ok=read_small_file(path,&after,256);g_free(path);path=NULL;if(!ok)goto done;
  if(!parse_config(before,&m->old) || !parse_config(after,&m->next))goto done;
  path=g_build_filename(dir,"deadline",NULL);ok=read_small_file(path,&deadline,32);g_free(path);path=NULL;if(!ok)goto done;
  char *end;m->deadline=g_ascii_strtoll(deadline,&end,10);if(*end || m->deadline<=0)goto done;
  path=g_build_filename(dir,"boot-id",NULL);ok=read_small_file(path,&boot,64);g_free(path);path=NULL;if(!ok)goto done;
  if(!g_file_get_contents("/proc/sys/kernel/random/boot_id",&current,NULL,NULL))goto done;
  m->same_boot=!strcmp(boot,current);result=1;
done:g_free(dir);g_free(before);g_free(after);g_free(deadline);g_free(boot);g_free(current);g_free(path);return result;
}
static int confirmed_address_socket(const struct mg_connection *c,const char *address) {
  /* Mongoose can retain the wildcard listener address in c->loc on accept.
   * Read the connected socket's actual destination, never Host/proxy headers. */
  struct sockaddr_storage local; socklen_t len=sizeof(local);struct in_addr wanted;
  if(!c || c->is_listening || c->is_udp || !c->fd ||
     inet_pton(AF_INET,address,&wanted)!=1 ||
     getsockname((int)(intptr_t)c->fd,(struct sockaddr *)&local,&len) ||
     local.ss_family!=AF_INET || len<sizeof(struct sockaddr_in))return 0;
  return ((struct sockaddr_in *)&local)->sin_addr.s_addr==wanted.s_addr;
}
/* Strict schemas are independent from the pool-only and DHCP switch APIs. */
static int address_request(struct mg_str body,int confirm_action) {
  const char *save[]={"\"action\"","\"address\"","\"netmask\"","\"pool_start\"","\"pool_end\"","\"version\"","\"confirm\"","\"validate_only\""};
  const char *finish[]={"\"action\"","\"id\"","\"confirm\""};
  const char **names=confirm_action?finish:save;int count=confirm_action?3:8,len=0;
  if(body.len>2048 || mg_json_get(body,"$",&len)!=0 || (size_t)len!=body.len || body.buf[0]!='{')return 0;
  unsigned seen=0;size_t off=0;struct mg_str key,val;
  while((off=mg_json_next(body,off,&key,&val))) {
    int which=-1;for(int i=0;i<count;i++)if(!mg_strcmp(key,mg_str(names[i])))which=i;
    if(which<0 || (seen&(1U<<which)))return 0;seen|=1U<<which;
    int boolean=confirm_action?which==2:which>=6;
    if(boolean) { if(mg_strcmp(val,mg_str("true")) && mg_strcmp(val,mg_str("false")))return 0; }
    else {
      if(val.len<2 || val.buf[0]!='"' || val.buf[val.len-1]!='"')return 0;
      for(size_t i=1;i+1<val.len;i++)if(val.buf[i]=='\\' || (unsigned char)val.buf[i]<32)return 0;
    }
  }
  unsigned required=confirm_action?7:127;if((seen&required)!=required)return 0;
  char *action=mg_json_get_str(body,"$.action");int ok=action && !strcmp(action,confirm_action?"confirm_address":"address");free(action);return ok;
}
static char *migration_rollback_script(const char *dir,const LanConfig *old,const LanConfig *next) {
  char *value=config_string(old),*oldcidr=address_cidr(old,0),*newcidr=address_cidr(next,0);
  char *oldnet=address_cidr(old,1),*newnet=address_cidr(next,1);
  char *remove_new=strcmp(newnet,LAN_RECOVERY_NET)?g_strdup_printf(
    "iptables -w 2 -D CW52_UDX_WEB -i tether -s %s -m comment --comment " LAN_ADDRESS_TAG " -j RETURN 2>/dev/null || :\n",newnet):g_strdup("");
  char *restore_old=strcmp(oldnet,LAN_RECOVERY_NET)?g_strdup_printf(
    "iptables -w 2 -C CW52_UDX_WEB -i tether -s %s -m comment --comment " LAN_ADDRESS_TAG " -j RETURN 2>/dev/null || "
    "iptables -w 2 -I CW52_UDX_WEB 1 -i tether -s %s -m comment --comment " LAN_ADDRESS_TAG " -j RETURN || exit 1\n",oldnet,oldnet):g_strdup("");
  char *nat_remove=strcmp(newnet,LAN_RECOVERY_NET)?g_strdup_printf(
    "iptables -w 2 -t nat -D POSTROUTING -s %s -o sipa_eth+ -m comment --comment " LAN_ADDRESS_TAG " -j MASQUERADE 2>/dev/null || :\n",newnet):g_strdup("");
  char *nat_restore=strcmp(oldnet,LAN_RECOVERY_NET)?g_strdup_printf(
    "iptables -w 2 -t nat -C POSTROUTING -s %s -o sipa_eth+ -m comment --comment " LAN_ADDRESS_TAG " -j MASQUERADE 2>/dev/null || "
    "iptables -w 2 -t nat -A POSTROUTING -s %s -o sipa_eth+ -m comment --comment " LAN_ADDRESS_TAG " -j MASQUERADE || exit 1\n",oldnet,oldnet):g_strdup("");
  const char *nat_recovery=strcmp(oldnet,LAN_RECOVERY_NET)?
    "iptables -w 2 -t nat -C POSTROUTING -s " LAN_RECOVERY_NET " -o sipa_eth+ -m comment --comment cw52-lan-recovery -j MASQUERADE 2>/dev/null || "
    "iptables -w 2 -t nat -A POSTROUTING -s " LAN_RECOVERY_NET " -o sipa_eth+ -m comment --comment cw52-lan-recovery -j MASQUERADE || exit 1\n":
    "iptables -w 2 -t nat -D POSTROUTING -s " LAN_RECOVERY_NET " -o sipa_eth+ -m comment --comment cw52-lan-recovery -j MASQUERADE 2>/dev/null || :\n";
  char *remove_addr=strcmp(oldcidr,newcidr)?g_strdup_printf("ip -4 address del %s dev tether 2>/dev/null || :\n",newcidr):g_strdup("");
  char *s=g_strdup_printf(
    "#!/bin/sh\nD='%s'\ncd \"$D\" || exit 1\n"
    "if [ \"$1\" != --now ]; then n=0; while [ $n -lt 120 ]; do [ ! -e committed ] || exit 0; sleep 1; n=$((n+1)); done; fi\n"
    "exec 9>finalize.lock\nflock -x 9 || exit 1\n[ ! -e committed ] || exit 0\n"
    "[ \"$(cat " LAN_PENDING " 2>/dev/null)\" = \"$D\" ] || exit 0\n"
    "ok=0\nn=0\nwhile [ $n -lt 3 ]; do\n"
    " dbus-send --system --print-reply --reply-timeout=4000 --dest=net.connman " LAN_TECH
    " net.connman.Technology.SetProperty string:TetheringDHCP variant:string:'%s' >rollback.log 2>&1 && { ok=1; break; }\n"
    " sleep 1; n=$((n+1))\ndone\n"
    DHCP_RELOAD_RESTORE
    "cp -p dhcp_config.ini.before " LAN_FILE ".rollback && mv -f " LAN_FILE ".rollback " LAN_FILE " || exit 1\n"
    "ip -4 address replace %s dev tether || exit 1\n%s"
    "/bin/sh " LAN_RECOVERY_HELPER " || exit 1\n"
    "/bin/sh " LAN_TERMINAL_REBIND " || exit 1\n"
    "if [ \"$(cat " DHCP_STATE " 2>/dev/null)\" = disabled ]; then\n"
    " iptables -w 2 -N " DHCP_CHAIN " 2>/dev/null || :\n"
    " iptables -w 2 -C " DHCP_CHAIN " -j DROP 2>/dev/null || iptables -w 2 -A " DHCP_CHAIN " -j DROP || exit 1\n"
    " iptables -w 2 -C INPUT -i tether -p udp --dport 67 -j " DHCP_CHAIN " 2>/dev/null || iptables -w 2 -I INPUT 1 -i tether -p udp --dport 67 -j " DHCP_CHAIN " || exit 1\nfi\n%s%s"
    "%s%s%s[ $ok -eq 1 ] || exit 1\n"
    "dbus-send --system --print-reply --reply-timeout=4000 --dest=net.connman " LAN_TECH
    " net.connman.Technology.GetProperties 2>/dev/null | grep -F '%s' >/dev/null || exit 1\n"
    "echo restored > restored\nsync\nrm -f " LAN_PENDING "\n"
    "[ \"$(cat /run/udx710-lan.lock 2>/dev/null)\" != \"$D\" ] || rm -f /run/udx710-lan.lock\n",
    dir,value,oldcidr,remove_addr,remove_new,restore_old,nat_remove,nat_restore,nat_recovery,value);
  g_free(value);g_free(oldcidr);g_free(newcidr);g_free(oldnet);g_free(newnet);g_free(remove_new);g_free(restore_old);g_free(remove_addr);g_free(nat_remove);g_free(nat_restore);return s;
}
static int migration_rollback(const char *dir) {
  char *path=g_build_filename(dir,"rollback.sh",NULL),out[2048];int ok=!run_command(out,sizeof(out),"/bin/sh",path,"--now",NULL);g_free(path);return ok;
}
static int migration_file(const char *dir,const char *name,const char *value,mode_t mode) {
  char *path=g_build_filename(dir,name,NULL);struct stat st={.st_mode=mode,.st_uid=geteuid(),.st_gid=getegid()};
  int ok=atomic_file(path,value,strlen(value),&st);g_free(path);return ok;
}
static void address_confirm(struct mg_connection *c,struct mg_http_message *hm) {
  if(!address_request(hm->body,1)) { HTTP_ERROR(c,400,"invalid address confirmation");return; }
  AddressMigration m;bool confirm=false;char *id=mg_json_get_str(hm->body,"$.id");mg_json_get_bool(hm->body,"$.confirm",&confirm);
  if(!confirm || !id || read_migration(&m)!=1 || strcmp(id,strrchr(m.dir,'/')+1)) { HTTP_ERROR(c,409,"no matching address change awaiting confirmation");goto done; }
  if(!confirmed_address_socket(c,m.next.address)) { HTTP_ERROR(c,409,"open and confirm from the new IPv4 address");goto done; }
  char *lockpath=g_build_filename(m.dir,"finalize.lock",NULL);int fd=open(lockpath,O_WRONLY|O_CREAT,0600);g_free(lockpath);
  if(fd<0 || flock(fd,LOCK_EX|LOCK_NB)) { if(fd>=0)close(fd);HTTP_ERROR(c,409,"address recovery is in progress");goto done; }
  LanConfig actual;AddressMigration fresh;
  if(read_migration(&fresh)!=1 || strcmp(fresh.dir,m.dir) || !fresh.same_boot || monotonic_seconds()>=fresh.deadline ||
      !read_live(&actual) || !same_config(&actual,&m.next) || !lan_address_present(&actual) || !recovery_present() || !management_reachable(&actual) || address_conflicts_locally(&actual) || !lan_route_reaches_tether(&actual)) {
    close(fd);HTTP_ERROR(c,409,"address verification failed or confirmation expired; recovery will restore the previous settings");goto done;
  }
  if(strcmp(m.old.address,LAN_RECOVERY_IP) && strcmp(m.old.address,m.next.address) && lan_address_present(&m.old)) {
    char *cidr=address_cidr(&m.old,0),out[1024];
    int ok=!run_command(out,sizeof(out),"/sbin/ip","-4","address","del",cidr,"dev","tether",NULL);g_free(cidr);
    if(!ok) { close(fd);HTTP_ERROR(c,500,"could not remove the previous LAN address; recovery remains armed");goto done; }
  }
  if(!migration_file(m.dir,"committed","confirmed from new address\n",0600)) { close(fd);HTTP_ERROR(c,500,"could not persist confirmation; recovery remains armed");goto done; }
  unlink(LAN_PENDING);unlink("/run/udx710-lan.lock");sync_parent(LAN_PENDING);close(fd);
  HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"LAN IPv4 confirmed and saved\"}");
done:free(id);
}
static void address_save(struct mg_connection *c,struct mg_http_message *hm) {
  if(!address_request(hm->body,0)) { HTTP_ERROR(c,400,"invalid LAN address fields");return; }
  char *address=mg_json_get_str(hm->body,"$.address"),*mask=mg_json_get_str(hm->body,"$.netmask");
  char *start=mg_json_get_str(hm->body,"$.pool_start"),*end=mg_json_get_str(hm->body,"$.pool_end"),*version=mg_json_get_str(hm->body,"$.version");
  bool confirm=false,validate=false;mg_json_get_bool(hm->body,"$.confirm",&confirm);mg_json_get_bool(hm->body,"$.validate_only",&validate);
  LanConfig old,next,actual;DhcpState dhcp_before;
  if(!dhcp_read(&dhcp_before) || dhcp_before.actual!=dhcp_before.desired) { HTTP_ERROR(c,409,"DHCP saved and runtime state differ; inspect recovery");goto done; }
  if(!read_live(&old)) { HTTP_ERROR(c,503,"ConnMan LAN configuration unavailable");goto done; }
  next=old;if(!ipv4(address,NULL)||!ipv4(mask,NULL)||!ipv4(start,NULL)||!ipv4(end,NULL)) { HTTP_ERROR(c,400,"use canonical IPv4 addresses and a contiguous netmask");goto done; }
  g_strlcpy(next.address,address,16);g_strlcpy(next.mask,mask,16);g_strlcpy(next.start,start,16);g_strlcpy(next.end,end,16);
  if(!valid_address_config(&next)) { HTTP_ERROR(c,400,"use a private LAN /8 to /30 with a usable router and pool; exclude the maintenance address");goto done; }
  if(address_conflicts_locally(&next)) { HTTP_ERROR(c,400,"new LAN conflicts with another local interface or contains a cellular address");goto done; }
  char *current=config_string(&old);int fresh=version&&!strcmp(version,current);g_free(current);
  if(!fresh) { HTTP_ERROR(c,409,"LAN state changed; refresh before saving");goto done; }
  if(!confirm) { HTTP_ERROR(c,400,"explicit confirmation required");goto done; }
  if(!lan_address_present(&old)) { HTTP_ERROR(c,409,"configured and running LAN addresses differ");goto done; }
  if(validate) { HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"valid; no changes applied\"}");goto done; }
  if(same_config(&old,&next)) { HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"unchanged\"}");goto done; }
  if(dhcp_before.actual) { HTTP_ERROR(c,409,"turn off CW52 LAN DHCP before changing its IPv4 address");goto done; }
  if(!strcmp(old.address,next.address) && !strcmp(old.mask,next.mask)) { HTTP_ERROR(c,400,"use the DHCP pool save operation for pool-only changes");goto done; }
  if(access(LAN_RECOVERY_HELPER,R_OK)||access(LAN_TERMINAL_REBIND,R_OK)||access("/usr/bin/flock",X_OK)||!ensure_recovery()||!management_reachable(&old)) { HTTP_ERROR(c,409,"maintenance recovery path unavailable; no changes applied");goto done; }
  if(strcmp(old.address,next.address) && strcmp(next.address,LAN_RECOVERY_IP)) {
    char out[1024];if(run_command(out,sizeof(out),"/bin/arping","-D","-q","-c","2","-w","3","-I","tether",next.address,NULL)) { HTTP_ERROR(c,409,"the new address is in use or the duplicate-address probe failed");goto done; }
  }
  int lock=open("/run/udx710-lan.lock",O_WRONLY|O_CREAT|O_EXCL,0600);
  if(lock<0) { HTTP_ERROR(c,409,"another LAN change is pending; inspect recovery");goto done; }
  char *dir=NULL,*original=NULL,*patched=NULL,*script=NULL,*path=NULL;gsize len=0,plen=0;int prepared=0,pending=0;const char *stage="recovery preparation";
  struct stat st,mnt,parent;GKeyFile *kf=g_key_file_new();pid_t pid=-1;
  if(stat("/mnt/data",&mnt)||stat("/mnt",&parent)||mnt.st_dev==parent.st_dev ||
    lstat(LAN_FILE,&st)||!S_ISREG(st.st_mode)||st.st_size>65536||!g_file_get_contents(LAN_FILE,&original,&len,NULL)||
    !g_key_file_load_from_data(kf,original,len,G_KEY_FILE_KEEP_COMMENTS,NULL))goto failed;
  const char *keys[]={"gateway_ip","mask","start_ip","end_ip"};const char *before[]={old.address,old.mask,old.start,old.end};const char *after[]={next.address,next.mask,next.start,next.end};
  for(int i=0;i<4;i++) { char *s=g_key_file_get_string(kf,"global",keys[i],NULL);int match=s&&!strcmp(s,before[i]);g_free(s);if(!match)goto failed;g_key_file_set_string(kf,"global",keys[i],after[i]); }
  if(!g_key_file_has_key(kf,"global","ttl",NULL)||g_key_file_get_integer(kf,"global","ttl",NULL)!=old.ttl)goto failed;
  dir=g_strdup(LAN_BACKUPS "/lan-address-XXXXXX");if(!mkdtemp(dir))goto failed;
  if(write(lock,dir,strlen(dir))!=(ssize_t)strlen(dir)||fsync(lock))goto failed;
  path=g_build_filename(dir,"dhcp_config.ini.before",NULL);int backed=atomic_file(path,original,len,&st);g_free(path);path=NULL;if(!backed)goto failed;
  if(!backup_dhcp_reload(dir))goto failed;
  char *v=config_string(&old);int ok=migration_file(dir,"before",v,0600);g_free(v);if(!ok)goto failed;
  v=config_string(&next);ok=migration_file(dir,"after",v,0600);g_free(v);if(!ok)goto failed;
  char *boot=NULL;if(!g_file_get_contents("/proc/sys/kernel/random/boot_id",&boot,NULL,NULL))goto failed;ok=migration_file(dir,"boot-id",boot,0600);g_free(boot);if(!ok)goto failed;
  long long deadline=monotonic_seconds()+120;char d[32];snprintf(d,sizeof(d),"%lld",deadline);if(!migration_file(dir,"deadline",d,0600))goto failed;
  script=migration_rollback_script(dir,&old,&next);if(!migration_file(dir,"rollback.sh",script,0700)||!migration_file(dir,"finalize.lock","",0600))goto failed;
  path=g_build_filename(dir,"rollback.sh",NULL);pid=fork();if(pid<0)goto failed;
  if(!pid) { setsid();int null=open("/dev/null",O_RDWR);if(null>=0) {dup2(null,0);dup2(null,1);dup2(null,2);if(null>2)close(null);}close(lock);execl("/bin/sh","sh",path,(char *)NULL);_exit(127); }
  prepared=1;g_free(path);path=NULL;struct stat private={.st_mode=0600,.st_uid=geteuid(),.st_gid=getegid()};
  if(!atomic_file(LAN_PENDING,dir,strlen(dir),&private))goto failed;pending=1;
  stage="persistent configuration";patched=g_key_file_to_data(kf,&plen,NULL);
  if(!patched||!atomic_file(LAN_FILE,patched,plen,&st)||!mark_dhcp_reload())goto failed;
  stage="ConnMan parameter save";if(!apply_live(&next))goto failed;
  stage="LAN address application";if(!install_lan_address(&next)||!ensure_recovery())goto failed;
  stage="LAN access and cellular NAT";
  if(!web_subnet_rule(&next,1)||!nat_subnet_rule(&next,1)||!nat_recovery_rule(&next)||!dhcp_rules_apply(0))goto failed;
  stage="configuration readback";if(!read_live(&actual)||!same_config(&actual,&next))goto failed;
  stage="LAN route verification";if(!lan_route_reaches_tether(&next))goto failed;
  stage="terminal address binding";if(!rebind_terminal())goto failed;
  stage="maintenance verification";if(!management_reachable(&next))goto failed;
  char *oldnet=address_cidr(&old,1),*newnet=address_cidr(&next,1);int same_net=!strcmp(oldnet,newnet);g_free(oldnet);g_free(newnet);
  stage="previous subnet policy cleanup";if(!same_net && (!web_subnet_rule(&old,0)||!nat_subnet_rule(&old,0)))goto failed;
  /* The independent watchdog keeps the lock until confirmation from new IP. */
  HTTP_JSON(c,200,"{\"Code\":0,\"Data\":\"Open the new IPv4 address and confirm within two minutes\"}");goto cleanup;
failed:
  if(pending) {
    migration_file(dir,"error-stage",stage,0600);
    int restored=migration_rollback(dir)&&read_live(&actual)&&same_config(&actual,&old)&&management_reachable(&old);
    char *message=g_strdup_printf("address change failed at %s; %s",stage,restored?"previous settings restored":"recovery remains armed; inspect rollback.sh");
    HTTP_ERROR(c,500,message);g_free(message);
  } else {unlink("/run/udx710-lan.lock");HTTP_ERROR(c,500,"address recovery preparation failed; no address change applied");}
cleanup:
  close(lock);g_key_file_free(kf);g_free(dir);g_free(original);g_free(patched);g_free(script);g_free(path);(void)prepared;
done:free(address);free(mask);free(start);free(end);free(version);
}

void lan_address_init(void) {
  AddressMigration m;int pending=read_migration(&m);
  if(pending==1 && (!m.same_boot || monotonic_seconds()>=m.deadline))migration_rollback(m.dir);
  LanConfig persisted;
  int reconciled=0;
  if(pending==0&&read_persisted(&persisted)) {
    LanConfig live;
    if(!read_live(&live)||!same_config(&live,&persisted))reconciled=1;
    if(reconciled&&!apply_live(&persisted))fprintf(stderr,"LAN ConnMan reconciliation failed\n");
    if(!lan_address_present(&persisted)) {
      reconciled=1;
      if(!install_lan_address(&persisted))
        fprintf(stderr,"LAN saved IPv4 could not be applied to tether\n");
    }
    int removed=remove_stale_lan_addresses(&persisted);
    if(removed<0)fprintf(stderr,"LAN stale kernel address cleanup failed\n");
    else if(removed>0)reconciled=1;
    if(!ensure_recovery()||!web_subnet_rule(&persisted,1)||
       !nat_subnet_rule(&persisted,1)||!nat_recovery_rule(&persisted))
      fprintf(stderr,"LAN maintenance address/policy restore failed\n");
    if(reconciled&&!schedule_tethering_reload())
      fprintf(stderr,"LAN DHCP runtime reload could not be scheduled\n");
  } else if(pending==0) {
    LanConfig v;
    if(read_live(&v)&&(!ensure_recovery()||!web_subnet_rule(&v,1)||
       !nat_subnet_rule(&v,1)||!nat_recovery_rule(&v)))
      fprintf(stderr,"LAN maintenance address/policy restore failed\n");
  }
}

static void lan_status(struct mg_connection *c) {
  LanConfig v;
  int supported = read_live(&v);
  JsonBuilder *j = json_new(); json_obj_open(j); json_add_int(j,"Code",0);
  json_key_obj_open(j,"Data"); json_add_bool(j,"supported",supported);
  DhcpState dhcp; int dhcp_ok=supported && dhcp_read(&dhcp);
  json_key_obj_open(j,"dhcp");json_add_bool(j,"supported",dhcp_ok);
  if (dhcp_ok) {
    json_add_bool(j,"enabled",dhcp.actual);json_add_bool(j,"saved_enabled",dhcp.desired);
    json_add_bool(j,"consistent",dhcp.actual==dhcp.desired);json_add_str(j,"version",dhcp.version);
    json_add_bool(j,"restart_required",dhcp.reload_required);
  }
  json_obj_close(j);
  if (supported) {
    json_add_str(j,"interface","tether"); json_add_str(j,"address",v.address); json_add_str(j,"netmask",v.mask);
    uint32_t mask; ipv4(v.mask,&mask); int prefix = 0; for (;mask;mask <<= 1) prefix++;
    json_add_int(j,"prefix",prefix); json_add_str(j,"pool_start",v.start); json_add_str(j,"pool_end",v.end);
    char *version = config_string(&v); json_add_str(j,"version",version); g_free(version);
    AddressMigration migration;int migration_state=read_migration(&migration);
    int kernel_consistent=lan_kernel_consistent(&v);
    json_add_bool(j,"kernel_consistent",kernel_consistent);
    int writable=kernel_consistent && migration_state==0 && access("/run/udx710-lan.lock",F_OK)!=0;
    json_add_bool(j,"pool_writable",writable && dhcp_ok && !dhcp.actual);
    json_add_bool(j,"address_writable",writable && dhcp_ok && !dhcp.actual && !access(LAN_RECOVERY_HELPER,R_OK) && !access("/usr/bin/flock",X_OK) && !access("/bin/arping",X_OK));
    json_add_str(j,"recovery_address",LAN_RECOVERY_IP);
    json_add_bool(j,"migration_error",migration_state<0);
    if(migration_state==1) {
      json_key_obj_open(j,"migration");json_add_str(j,"id",strrchr(migration.dir,'/')+1);
      json_add_str(j,"address",migration.next.address);json_add_str(j,"netmask",migration.next.mask);
      long long remaining=migration.same_boot?migration.deadline-monotonic_seconds():0;
      json_add_int(j,"seconds_left",remaining>0?(int)remaining:0);
      json_add_bool(j,"confirmable",migration.same_boot && remaining>0 && confirmed_address_socket(c,migration.next.address));json_obj_close(j);
    }
    json_arr_open(j,"writable_fields"); json_arr_add_str(j,"address");json_arr_add_str(j,"netmask"); json_arr_add_str(j,"pool_start"); json_arr_add_str(j,"pool_end"); json_arr_close(j);
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
    if (f) fclose(f);
    free(line); g_hash_table_destroy(seen); json_arr_close(j);
  }
  json_obj_close(j); json_obj_close(j); HTTP_OK_FREE(c,json_finish(j));
}

static void lan_save(struct mg_connection *c, struct mg_http_message *hm) {
  if (!valid_request(hm->body)) { HTTP_ERROR(c,400,"invalid or unsupported LAN fields"); return; }
  LanConfig old, next, actual;DhcpState dhcp_before;
  char *start = mg_json_get_str(hm->body,"$.pool_start"), *end = mg_json_get_str(hm->body,"$.pool_end");
  char *version = mg_json_get_str(hm->body,"$.version");
  bool confirm = false, validate = false;
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
  if(!dhcp_read(&dhcp_before)||dhcp_before.actual!=dhcp_before.desired) { HTTP_ERROR(c,409,"DHCP state unavailable or inconsistent");goto done; }
  if(dhcp_before.actual) { HTTP_ERROR(c,409,"turn off CW52 LAN DHCP before changing its address pool");goto done; }

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
  if(!backup_dhcp_reload(dir))goto failed;
  watcher = start_watchdog(dir,&old); if (watcher < 0) goto failed;
  prepared = 1;
  if (!atomic_file(LAN_FILE,patched,plen,&st) || !mark_dhcp_reload()) goto failed;
  if (!apply_live(&next) || !ensure_recovery() || !read_live(&actual) || !same_config(&actual,&next) || !management_reachable(&old)) goto failed;
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
    char *rollback=g_build_filename(dir,"rollback.sh",NULL),output[1024];
    int restored=!run_command(output,sizeof(output),"/bin/sh",rollback,"--now",NULL) &&
      read_live(&actual) && same_config(&actual,&old) && management_reachable(&old);g_free(rollback);
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
  else if (http_is_method(hm,"POST")) {
    char *action=mg_json_get_str(hm->body,"$.action");
    if (action && !strcmp(action,"dhcp")) dhcp_save(c,hm);
    else if(action && !strcmp(action,"address")) address_save(c,hm);
    else if(action && !strcmp(action,"confirm_address")) address_confirm(c,hm);
    else lan_save(c,hm);
    free(action);
  }
  else http_method_error(c);
}



