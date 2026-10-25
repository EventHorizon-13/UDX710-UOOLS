#include "mongoose.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define TERMINAL_CONFIG "/tmp/terminal-proxy-fixture.ini"
#define TERMINAL_PORT 17681
static int auth_verify_token(const char *s) {
  return strlen(s)==64 && s[0]=='a' && !getenv("REVOKED") ? 0 : -1;
}
#include "../src/handlers/terminal_proxy.h"
static void handler(struct mg_connection *c,int ev,void *data) {
  if(ev!=MG_EV_HTTP_MSG)return;
  struct mg_http_message *hm=data;
  if(!mg_strcmp(hm->uri,mg_str("/api/terminal/session")))terminal_session(c,hm);
  else terminal_proxy(c,hm);
}
int main(void){struct mg_mgr mgr;mg_mgr_init(&mgr);if(!mg_http_listen(&mgr,"http://127.0.0.1:16677",handler,NULL))return 1;for(;;)mg_mgr_poll(&mgr,10);}
