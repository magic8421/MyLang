#ifndef SYMTAB_H
#define SYMTAB_H

#include "ast.h"

#define MAX_FIELDS 32
#define HASH_SIZE  64

typedef struct ClassInfo {
    char name[64];
    int  field_count;
    char field_names[MAX_FIELDS][64];
    Type field_types[MAX_FIELDS];
    struct ClassInfo* next;
} ClassInfo;

typedef struct SymEntry {
    char name[64];
    Type type;
    struct SymEntry* next;
} SymEntry;

typedef struct Scope {
    SymEntry*     table[HASH_SIZE];
    struct Scope* parent;
    int           level;
} Scope;

void        symtab_init(void);
void        symtab_enter_scope(void);
void        symtab_exit_scope(void);
Scope*      symtab_current_scope(void);

void        symtab_insert(const char* name, Type type);
SymEntry*   symtab_lookup(const char* name);
SymEntry*   symtab_lookup_current(const char* name);

void        symtab_add_class(const char* name, ClassInfo* info);
ClassInfo*  symtab_find_class(const char* name);
int         symtab_add_field(ClassInfo* info, const char* name, Type type);

typedef struct FuncInfo {
    char name[64];
    Type return_type;
    struct FuncInfo* next;
} FuncInfo;

void       symtab_add_func(const char* name, Type ret_type);
FuncInfo*  symtab_find_func(const char* name);

#endif
