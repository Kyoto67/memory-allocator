#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

_Noreturn void err( const char* msg, ... ) {
    (void) msg;
//  va_list args;
//  va_start (args, msg);
//  vfprintf(stderr, msg, args);
//  fprintf(stderr, "%s%s%s",msg, msg2,"\n");
//  va_end (args);
  abort();
}

void error( const char* msg, const char* msg2 ) {
//  va_list args;
//  va_start (args, msg);
//  vfprintf(stderr, msg, args);
  fprintf(stderr, "%s%s%s",msg, msg2,"\n");
//  va_end (args);
    abort();
}

void sucs( const char* testName, const char* successMessage ) {
//    va_list args;
//    va_start (args, testName);
    fprintf(stdout, "%s%s%s", testName, successMessage,"\n");
//    va_end (args);
//    abort();
}

extern inline size_t size_max( size_t x, size_t y );
