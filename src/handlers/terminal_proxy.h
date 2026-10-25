/* Same-origin ttyd transport. Only an authenticated session can reach ttyd. */
#include <arpa/inet.h>
#include <time.h>
#ifndef TERMINAL_CONFIG
#define TERMINAL_CONFIG "/var/mm/dhcp_config.ini"
#endif
#ifndef TERMINAL_PORT
#define TERMINAL_PORT 7681
#endif
typedef struct {
  struct mg_connection *peer;
  uint64_t deadline, auth_check;
  int upstream, received;
  char token[65];
} TerminalPipe;

static int terminal_token_valid(const char *token) {
  if (strlen(token) != 64) return 0;
  for (size_t i=0;i<64;i++)
    if (!((token[i]>='0' && token[i]<='9') || (token[i]>='a' && token[i]<='f'))) return 0;
  return auth_verify_token(token)==0;
}
static int terminal_same_origin(struct mg_http_message *hm) {
  struct mg_str *origin=mg_http_get_header(hm,"Origin"), *host=mg_http_get_header(hm,"Host");
  if (!origin) return 1; /* Non-browser API clients still need a valid token. */
  if (!host) return 0;
  size_t skip=0;
  if (origin->len>7 && !memcmp(origin->buf,"http://",7)) skip=7;
  else if (origin->len>8 && !memcmp(origin->buf,"https://",8)) skip=8;
  return skip && origin->len-skip==host->len && !memcmp(origin->buf+skip,host->buf,host->len);
}
static int terminal_cookie(struct mg_http_message *hm,char token[65]) {
  struct mg_str *cookie=mg_http_get_header(hm,"Cookie");
  if (!cookie) return 0;
  struct mg_str value=mg_http_get_header_var(*cookie,mg_str("cw52_terminal"));
  if (value.len!=64) return 0;
  memcpy(token,value.buf,64);token[64]=0;
  return terminal_token_valid(token);
}
static void terminal_pipe(struct mg_connection *c,int ev,void *ev_data) {
  (void)ev_data;
  TerminalPipe *p=c->fn_data;
  if (!p) return;
  if (ev==MG_EV_CONNECT) p->deadline=mg_millis()+3600000;
  if (ev==MG_EV_POLL) {
    uint64_t now=mg_millis();
    if(now>p->deadline) c->is_closing=1;
    if(now>p->auth_check) {
      p->auth_check=now+30000;
      if(!terminal_token_valid(p->token)) c->is_closing=1;
    }
  } else if(ev==MG_EV_READ && p->peer && c->recv.len) {
    p->received=1;
    if(p->peer->send.len+c->recv.len>262144 || !mg_send(p->peer,c->recv.buf,c->recv.len)) {
      c->is_closing=1;p->peer->is_closing=1;
    }
    mg_iobuf_del(&c->recv,0,c->recv.len);
  } else if(ev==MG_EV_ERROR) {
    if(p->upstream && p->peer && !p->received)
      mg_http_reply(p->peer,502,"Cache-Control: no-store\r\n","Terminal unavailable\n");
  } else if(ev==MG_EV_CLOSE) {
    if(p->peer) {
      TerminalPipe *other=p->peer->fn_data;
      if(other) other->peer=NULL;
      p->peer->is_draining=1;
    }
    free(p);c->fn_data=NULL;
  }
}
static void terminal_session(struct mg_connection *c,struct mg_http_message *hm) {
  if (mg_strcmp(hm->method,mg_str("POST")) || !terminal_same_origin(hm)) {
    mg_http_reply(c,403,"Cache-Control: no-store\r\n","");return;
  }
  struct mg_str *auth=mg_http_get_header(hm,"Authorization");char token[65]={0};
  if(auth && auth->len==71 && !memcmp(auth->buf,"Bearer ",7)) memcpy(token,auth->buf+7,64);
  if(!terminal_token_valid(token)) {mg_http_reply(c,401,"Cache-Control: no-store\r\n","");return;}
  char headers[320];
  snprintf(headers,sizeof(headers),"Cache-Control: no-store\r\nContent-Type: application/json\r\n"
    "Set-Cookie: cw52_terminal=%s; Path=/terminal/; HttpOnly; SameSite=Strict; Max-Age=1800\r\n",token);
  mg_http_reply(c,200,headers,"{\"ok\":true}");
}
static void terminal_proxy(struct mg_connection *c,struct mg_http_message *hm) {
  char token[65]={0};
  if(!terminal_same_origin(hm) || !terminal_cookie(hm,token)) {
    mg_http_reply(c,401,"Cache-Control: no-store\r\n","Please sign in to the dashboard\n");return;
  }
  if(mg_strcmp(hm->method,mg_str("GET")) || hm->body.len || hm->query.len ||
     (mg_strcmp(hm->uri,mg_str("/terminal/")) &&
      mg_strcmp(hm->uri,mg_str("/terminal/token")) &&
      mg_strcmp(hm->uri,mg_str("/terminal/ws")))) {
    mg_http_reply(c,404,"Cache-Control: no-store\r\n","");return;
  }
  /* The current ttyd listens on the configured LAN address. Never accept an upstream from the request. */
  char addr[64]="192.168.1.1",line[128],url[96];struct in_addr parsed;
  FILE *f=fopen(TERMINAL_CONFIG,"r");
  if(f){while(fgets(line,sizeof(line),f)) if(sscanf(line,"gateway_ip=%63s",addr)==1) break;fclose(f);}
  if(inet_pton(AF_INET,addr,&parsed)!=1) {mg_http_reply(c,502,"","Terminal unavailable\n");return;}
  snprintf(url,sizeof(url),"tcp://%s:%d",addr,TERMINAL_PORT);
  TerminalPipe *down=calloc(1,sizeof(*down)),*up=calloc(1,sizeof(*up));
  if(!down || !up){free(down);free(up);mg_http_reply(c,503,"","");return;}
  memcpy(down->token,token,65);memcpy(up->token,token,65);
  down->deadline=mg_millis()+3600000;up->deadline=mg_millis()+5000;
  down->auth_check=up->auth_check=mg_millis()+30000;up->upstream=1;
  struct mg_connection *peer=mg_connect(c->mgr,url,terminal_pipe,up);
  if(!peer){free(down);free(up);mg_http_reply(c,502,"","");return;}
  down->peer=peer;up->peer=c;
  c->fn=terminal_pipe;c->fn_data=down;c->pfn=NULL;
  /* HTTP parser removes this request after returning. No credentials or cookies go upstream. */
  struct mg_str *upgrade=mg_http_get_header(hm,"Upgrade");
  int ws=upgrade && !mg_strcasecmp(*upgrade,mg_str("websocket"));
  mg_printf(peer,"GET %.*s HTTP/1.1\r\nHost: %s:%d\r\nConnection: %s\r\n",
    (int)(hm->uri.len-9),hm->uri.buf+9,addr,TERMINAL_PORT,ws?"Upgrade":"close");
  const char *names[]={"Upgrade","Sec-WebSocket-Key","Sec-WebSocket-Version","Sec-WebSocket-Protocol"};
  if(ws) for(size_t i=0;i<4;i++) {struct mg_str *v=mg_http_get_header(hm,names[i]);if(v) mg_printf(peer,"%s: %.*s\r\n",names[i],(int)v->len,v->buf);}
  mg_printf(peer,"\r\n");
}
