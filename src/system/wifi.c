/**
 * @file wifi.c
 * @brief Read and control the device APs through hostapd's Unix control socket.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exec_utils.h"
#include "http_utils.h"
#include "json_builder.h"
#include "wifi.h"

#define HOSTAPD_CLI "/usr/sbin/hostapd_cli"
#define HOSTAPD_CTRL "/var/run/hostapd"

typedef struct {
  const char *band;
  const char *iface;
  char state[32];
  char ssid[128];
  char channel[16];
  char clients[4096];
  int available;
} WifiBand;

static int run_hostapd(const char *iface, const char *command, char *out,
                       size_t out_size) {
  return run_command(out, out_size, HOSTAPD_CLI, "-p", HOSTAPD_CTRL, "-i",
                     (char *)iface, (char *)command, NULL);
}

static void parse_status(WifiBand *band, const char *status) {
  char copy[4096];
  snprintf(copy, sizeof(copy), "%s", status);
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *value = strchr(line, '=');
    if (!value) continue;
    *value++ = '\0';
    if (strcmp(line, "state") == 0)
      snprintf(band->state, sizeof(band->state), "%s", value);
    else if (strcmp(line, "ssid[0]") == 0 || strcmp(line, "ssid") == 0)
      snprintf(band->ssid, sizeof(band->ssid), "%s", value);
    else if (strcmp(line, "channel") == 0)
      snprintf(band->channel, sizeof(band->channel), "%s", value);
  }
}

static int read_band(WifiBand *band) {
  char status[4096] = {0};
  char clients[4096] = {0};
  band->available = 0;
  if (run_hostapd(band->iface, "status", status, sizeof(status)) != 0 ||
      strstr(status, "FAIL")) return -1;
  parse_status(band, status);
  if (band->state[0] == '\0') return -1;
  if (run_hostapd(band->iface, "list_sta", clients, sizeof(clients)) == 0 &&
      !strstr(clients, "FAIL"))
    snprintf(band->clients, sizeof(band->clients), "%s", clients);
  band->available = 1;
  return 0;
}

static void add_band_json(JsonBuilder *j, const WifiBand *band) {
  json_arr_obj_open(j);
  json_add_str(j, "band", band->band);
  json_add_str(j, "interface", band->iface);
  json_add_bool(j, "available", band->available);
  json_add_bool(j, "enabled", strcmp(band->state, "ENABLED") == 0);
  json_add_str(j, "state", band->state);
  json_add_str(j, "ssid", band->ssid);
  json_add_str(j, "channel", band->channel);
  json_arr_open(j, "clients");
  char copy[sizeof(band->clients)];
  snprintf(copy, sizeof(copy), "%s", band->clients);
  char *save = NULL;
  for (char *line = strtok_r(copy, "\r\n", &save); line;
       line = strtok_r(NULL, "\r\n", &save)) {
    if (strstr(line, "Selected interface") || strstr(line, "FAIL") ||
        strlen(line) < 17) continue;
    json_arr_add_str(j, line);
  }
  json_arr_close(j);
  json_obj_close(j);
}

static void handle_wifi_status(struct mg_connection *c) {
  WifiBand bands[] = {
      {.band = "2.4 GHz", .iface = "wlan1"},
      {.band = "5 GHz", .iface = "wlan0"},
  };
  int count = 0;
  for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++)
    if (read_band(&bands[i]) == 0) count++;

  JsonBuilder *j = json_new();
  json_obj_open(j);
  json_add_int(j, "Code", 0);
  json_add_str(j, "Error", "");
  json_key_obj_open(j, "Data");
  json_add_bool(j, "supported", count > 0);
  json_arr_open(j, "bands");
  for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++)
    add_band_json(j, &bands[i]);
  json_arr_close(j);
  json_obj_close(j);
  json_obj_close(j);
  HTTP_OK_FREE(c, json_finish(j));
}

static void handle_wifi_control(struct mg_connection *c,
                                struct mg_http_message *hm) {
  char *band = mg_json_get_str(hm->body, "$.band");
  int enabled = -1;
  int value = 0;
  if (mg_json_get_bool(hm->body, "$.enabled", &value)) enabled = value;
  if (!band || (strcmp(band, "2g") != 0 && strcmp(band, "5g") != 0) ||
      enabled < 0) {
    free(band);
    HTTP_ERROR(c, 400, "band must be 2g or 5g and enabled must be boolean");
    return;
  }

  const char *iface = strcmp(band, "2g") == 0 ? "wlan1" : "wlan0";
  char output[1024] = {0};
  int result = run_hostapd(iface, enabled ? "enable" : "disable",
                           output, sizeof(output));
  free(band);
  if (result != 0 || strstr(output, "FAIL")) {
    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 1);
    json_add_str(j, "Error", "hostapd rejected the radio change");
    json_add_str(j, "Data", output);
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
    return;
  }
  JsonBuilder *j = json_new();
  json_obj_open(j);
  json_add_int(j, "Code", 0);
  json_add_str(j, "Error", "");
  json_add_str(j, "Data", enabled ? "热点已开启" : "热点已关闭");
  json_obj_close(j);
  HTTP_OK_FREE(c, json_finish(j));
}

void handle_wifi(struct mg_connection *c, struct mg_http_message *hm) {
  HTTP_CHECK_ANY(c, hm);
  if (http_is_method(hm, "GET")) {
    handle_wifi_status(c);
  } else if (http_is_method(hm, "POST")) {
    handle_wifi_control(c, hm);
  } else {
    http_method_error(c);
  }
}
