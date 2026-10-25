#include <assert.h>
#include <stdio.h>
#include "../src/system/update.c"
int main(void) {
 assert(!strcmp(update_get_version(),"2.4.0-cw52.1"));
 assert(update_version_is_newer("2.4.0-cw52.2","2.4.0-cw52.1"));
 assert(update_version_is_newer("2.10.0-cw52.1","2.4.0-cw52.1"));
 assert(!update_version_is_newer("2.9.0-cw52.9","2.10.0-cw52.1"));
 assert(!update_version_is_newer("2.4.0-cw52.1","2.4.0-cw52.1"));
 assert(!update_version_is_newer("2.4.0-cw52.0","2.4.0-cw52.1"));
 assert(!update_version_is_newer("99.0.0","2.4.0-cw52.1"));
 assert(!update_version_is_newer("2.5.0-other.1","2.4.0-cw52.1"));
 assert(!update_version_is_newer("2.4.0-cw52.2x","2.4.0-cw52.1"));
 assert(!update_version_is_newer("-1.0.0-cw52.1","2.4.0-cw52.1"));
 assert(update_version_is_newer("2.4.0-cw52.1","2.3.5"));
 puts("CW52 version identity, numeric comparison and update-channel isolation passed");return 0;
}
