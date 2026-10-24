#ifndef CW52_LAN_H
#define CW52_LAN_H
#include "mongoose.h"
void lan_dhcp_init(void);
void lan_address_init(void);
void handle_lan(struct mg_connection *c, struct mg_http_message *hm);
#endif
