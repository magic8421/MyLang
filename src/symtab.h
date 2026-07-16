#ifndef SYMTAB_H
#define SYMTAB_H

#include "ast.h"

#define MAX_FIELDS 32
#define MAX_IMPL  8
#define HASH_SIZE  64
#define MAX_GENERIC_PARAMS 8
#define MAX_CONSTRAINTS_PER_PARAM 4

typedef struct MethodInfo {
    char name[64];
    Type return_type;
    int  param_count;
    char param_names[16][64];
    Type param_types[16];
    int  is_native;
    struct MethodInfo* next;
} MethodInfo;

typedef struct InterfaceMethodInfo {
    char name[64];
    Type return_type;
    int  param_count;
    char param_names[16][64];
    Type param_types[16];
    AstNode* interface_method_default_body;
    int  interface_method_line;
} InterfaceMethodInfo;

#define MAX_IFACE_METHODS 32

typedef struct InterfaceInfo {
    char name[64];
    int  type_id;
    int  method_count;
    InterfaceMethodInfo methods[MAX_IFACE_METHODS];
    struct InterfaceInfo* next;
} InterfaceInfo;

typedef struct ClassInfo {
    char name[64];
    int  type_id;
    int  field_count;
    char field_names[MAX_FIELDS][64];
    Type field_types[MAX_FIELDS];
    MethodInfo* methods;
    int  impl_count;
    char impl_names[MAX_IMPL][64];
    InterfaceInfo* impl_infos[MAX_IMPL];

    int  is_generic;
    int  generic_param_count;
    char generic_params[MAX_GENERIC_PARAMS][64];
    int  generic_constraint_count[MAX_GENERIC_PARAMS];
    char generic_constraints[MAX_GENERIC_PARAMS][MAX_CONSTRAINTS_PER_PARAM][64];
    int  generic_has_new[MAX_GENERIC_PARAMS];
    AstNode* generic_ast;

    int  is_instantiation;
    struct ClassInfo* generic_def;
    int  instantiation_arg_count;
    Type instantiation_args[MAX_GENERIC_PARAMS];
    char mangled_name[128];

    struct ClassInfo* next;
} ClassInfo;

typedef struct StructInfo {
    char name[64];
    int  type_id;
    int  field_count;
    char field_names[MAX_FIELDS][64];
    Type field_types[MAX_FIELDS];
    struct StructInfo* next;
} StructInfo;

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

void        symtab_add_struct(const char* name, StructInfo* info);
StructInfo* symtab_find_struct(const char* name);
StructInfo* symtab_first_struct(void);
int         symtab_add_struct_field(StructInfo* info, const char* name, Type type);
void        symtab_add_method(ClassInfo* cls, const char* name, Type ret_type,
                              int pc, const char pn[][64], const Type pt[],
                              int is_native);
MethodInfo* symtab_find_method(const char* class_name, const char* method_name);

typedef struct FuncInfo {
    char name[64];
    Type return_type;
    int  param_count;
    char param_names[16][64];
    Type param_types[16];
    int  is_builtin;
    struct FuncInfo* next;
} FuncInfo;

void       symtab_add_func(const char* name, Type ret_type,
                           int pc, const char pn[][64], const Type pt[],
                           int is_builtin);
FuncInfo*  symtab_find_func(const char* name);
FuncInfo*  symtab_first_func(void);

void           symtab_add_interface(const char* name, InterfaceInfo* info);
InterfaceInfo* symtab_find_interface(const char* name);
void           symtab_add_interface_method(InterfaceInfo* iface, const char* name,
                                           Type ret_type, int pc,
                                           const char pn[][64], const Type pt[],
                                           AstNode* default_body, int line);
InterfaceMethodInfo* symtab_find_interface_method(InterfaceInfo* iface,
                                                   const char* method_name);

void symtab_add_class_impl(ClassInfo* cls, const char* iface_name);

int  symtab_validate_impls(void);
int  symtab_validate_generic_method_calls(ClassInfo* generic_def);

int  symtab_next_type_id(void);
void symtab_mark_class_generic(ClassInfo* info, AstNode* ast, int param_count, const char* params[]);
ClassInfo* symtab_find_class_by_mangled(const char* mangled_name);
ClassInfo* symtab_add_class_instantiation(ClassInfo* generic_def, const Type** args, int arg_count);
ClassInfo* symtab_instantiate_class_from_type(Type* t);
MethodInfo* symtab_find_method_in_class(ClassInfo* cls, const char* method_name);

#endif
