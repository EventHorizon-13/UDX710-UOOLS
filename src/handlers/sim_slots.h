/* Read each SIM independently. This endpoint never powers a modem or switches a SIM. */
#include <gio/gio.h>
static int sim_iccid_valid(const char *s) {
  if(!s)return 0;size_t n=strlen(s);if(n<18 || n>22)return 0;
  for(size_t i=0;i<n;i++)if(s[i]<'0'||s[i]>'9')return 0;
  return 1;
}
static void sim_slot_json(JsonBuilder *j,int index,GVariant *dict) {
  char slot[12];snprintf(slot,sizeof(slot),"slot%d",index+1);
  gboolean present=FALSE,powered=FALSE;
  int present_known=dict && g_variant_lookup(dict,"Present","b",&present);
  int power_known=dict && g_variant_lookup(dict,"Powered","b",&powered);
  const char *id=NULL;
  if(dict)g_variant_lookup(dict,"CardIdentifier","&s",&id);
  /* Inactive unpowered slots cannot reliably distinguish absent from unreadable. */
  int has_id=(!present_known || present) && sim_iccid_valid(id);
  const char *state=has_id?"ready":present_known && !present && (!power_known || powered)?"absent":"unavailable";
  json_arr_obj_open(j);json_add_str(j,"slot",slot);
  json_add_str(j,"iccid",has_id?id:"");json_add_str(j,"status",state);json_obj_close(j);
}
static void handle_sim_slots(struct mg_connection *c,struct mg_http_message *hm) {
  HTTP_CHECK_GET(c,hm);
  GError *error=NULL;GDBusConnection *bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,&error);g_clear_error(&error);
  JsonBuilder *j=json_new();json_obj_open(j);json_add_int(j,"Code",0);json_key_obj_open(j,"Data");json_arr_open(j,"slots");
  for(int i=0;i<2;i++) {
    char path[16];snprintf(path,sizeof(path),"/ril_%d",i);
    GVariant *reply=bus?g_dbus_connection_call_sync(bus,"org.ofono",path,"org.ofono.SimManager",
      "GetProperties",NULL,G_VARIANT_TYPE("(a{sv})"),G_DBUS_CALL_FLAGS_NONE,1500,NULL,&error):NULL;
    g_clear_error(&error);GVariant *dict=reply?g_variant_get_child_value(reply,0):NULL;
    sim_slot_json(j,i,dict);if(dict)g_variant_unref(dict);if(reply)g_variant_unref(reply);
  }
  if(bus)g_object_unref(bus);
  json_arr_close(j);json_obj_close(j);json_obj_close(j);HTTP_OK_FREE(c,json_finish(j));
}
