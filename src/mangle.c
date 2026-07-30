#include "mangle.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static void mangle_type_into(Type* t, char* out, size_t out_size);

const char* mangle_type(Type* t) {
    if (t->mangled_name[0]) return t->mangled_name;
    mangle_type_into(t, t->mangled_name, sizeof(t->mangled_name));
    return t->mangled_name;
}

static void append_mangled(char* out, size_t out_size, const char* s) {
    size_t len = strlen(out);
    if (len >= out_size) return;
    CHECK_STRSCPY(strscpy(out + len, s, out_size - len), "mangled name segment too long");
}

static void mangle_type_into(Type* t, char* out, size_t out_size) {
    if (t->type_kind == TYPE_TYPE_PARAM) {
        CHECK_STRSCPY(strscpy(out, t->class_name, out_size), "type param name too long");
        return;
    }

    if (t->is_weak) {
        Type inner = *t;
        char inner_buf[256];
        int n;
        inner.is_weak = 0;
        inner.mangled_name[0] = '\0';
        mangle_type_into(&inner, inner_buf, sizeof(inner_buf));
        n = snprintf(out, out_size, "Weak_%s", inner_buf);
        CHECK_SNPRINTF(n, out_size, "weak mangled name too long");
        return;
    }

    if (t->is_unowned) {
        Type inner = *t;
        char inner_buf[256];
        int n;
        inner.is_unowned = 0;
        inner.mangled_name[0] = '\0';
        mangle_type_into(&inner, inner_buf, sizeof(inner_buf));
        n = snprintf(out, out_size, "Unowned_%s", inner_buf);
        CHECK_SNPRINTF(n, out_size, "unowned mangled name too long");
        return;
    }

    if (t->is_array) {
        Type inner = *t;
        char inner_buf[256];
        int n;
        inner.is_array = 0;
        inner.array_size = 0;
        inner.mangled_name[0] = '\0';
        mangle_type_into(&inner, inner_buf, sizeof(inner_buf));
        if (t->array_size > 0) {
            n = snprintf(out, out_size, "Arr%d_%s", t->array_size, inner_buf);
        } else {
            n = snprintf(out, out_size, "Arr_%s", inner_buf);
        }
        CHECK_SNPRINTF(n, out_size, "array mangled name too long");
        return;
    }

    switch (t->type_kind) {
        case TYPE_CLASS:
        case TYPE_STRUCT:
        case TYPE_INTERFACE:
        case TYPE_ENUM: {
            CHECK_STRSCPY(strscpy(out, t->class_name, out_size), "class name too long");
            int i;
            for (i = 0; i < t->type_arg_count && i < MAX_TYPE_ARGS; i++) {
                char arg_buf[256];
                mangle_type_into(t->type_args[i], arg_buf, sizeof(arg_buf));
                append_mangled(out, out_size, "_");
                append_mangled(out, out_size, arg_buf);
            }
            break;
        }
        default: {
            const char* name = type_name(t);
            CHECK_STRSCPY(strscpy(out, name, out_size), "primitive name too long");
            break;
        }
    }
}
