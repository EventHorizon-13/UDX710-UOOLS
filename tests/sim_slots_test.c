#include "mongoose.h"
#include "http_utils.h"
#include "json_builder.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../src/handlers/sim_slots.h"
static GVariant *properties(int present,int powered,const char *id) {
 GVariantBuilder b;g_variant_builder_init(&b,G_VARIANT_TYPE("a{sv}"));
 g_variant_builder_add(&b,"{sv}","Present",g_variant_new_boolean(present));
 g_variant_builder_add(&b,"{sv}","Powered",g_variant_new_boolean(powered));
 if(id)g_variant_builder_add(&b,"{sv}","CardIdentifier",g_variant_new_string(id));
 return g_variant_ref_sink(g_variant_builder_end(&b));
}
int main(void) {
 const char *fake="89860000000000000001";assert(sim_iccid_valid(fake));assert(!sim_iccid_valid("unknown"));assert(!sim_iccid_valid("123"));assert(!sim_iccid_valid("8986000000000000000F"));
 JsonBuilder *j=json_new();json_obj_open(j);json_arr_open(j,"slots");
 GVariant *a=properties(1,1,fake),*b=properties(0,0,fake),*c=properties(0,1,NULL);
 sim_slot_json(j,0,a);sim_slot_json(j,1,b);sim_slot_json(j,2,c);sim_slot_json(j,3,NULL);json_arr_close(j);json_obj_close(j);char *out=json_finish(j);
 char *first=strstr(out,fake);assert(first && !strstr(first+strlen(fake),fake));
 assert(strstr(out,"\"status\":\"ready\"") && strstr(out,"\"status\":\"absent\"") && strstr(out,"\"status\":\"unavailable\""));
 free(out);g_variant_unref(a);g_variant_unref(b);g_variant_unref(c);
 puts("SIM per-slot ICCID, unpowered/absent/unavailable states and stale-identifier isolation passed");return 0;
}
