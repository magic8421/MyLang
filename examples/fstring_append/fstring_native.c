#include "fstring_append.h"
#include "runtime.h"
#include <stdio.h>

void Logger_puts(Logger* thiz, String* s) {
    (void)thiz;
    mylang_print_string(s);
}

void Logger_puti(Logger* thiz, int32_t v) {
    (void)thiz;
    mylang_print_i32(v);
}
