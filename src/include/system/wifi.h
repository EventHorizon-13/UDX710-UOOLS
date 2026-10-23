#ifndef WIFI_H
#define WIFI_H

#include "mongoose.h"

void handle_wifi(struct mg_connection *c, struct mg_http_message *hm);

#endif
