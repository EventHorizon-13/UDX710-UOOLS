/* Opt-in startup diagnostics. No persistent logging unless explicitly requested. */
#ifndef CW52_STARTUP_TRACE_H
#define CW52_STARTUP_TRACE_H
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void startup_trace(const char *stage) {
 const char *path=getenv("CW52_STARTUP_TRACE");if(!path || !*path)return;
 FILE *f=fopen(path,"a");if(!f)return;
 fprintf(f,"%ld %s\n",(long)time(NULL),stage);fclose(f);
}
#endif
