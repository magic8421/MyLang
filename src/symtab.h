#ifndef SYMTAB_H
#define SYMTAB_H

#include "ast.h"

#define MAX_FIELDS 32
#define MAX_IMPL  8
#define HASH_SIZE  64
#define MAX_GENERIC_PARAMS 8
#define MAX_CONSTRAINTS_PER_PARAM 4
#define MAX_PARAMS 16

typedef struct MethodInfo {
    char name[NAME_BUF_SIZE];
    Type return_type;
    int  param_count;
    char param_names[MAX_PARAMS][NAME_BUF_SIZE];
    Type param_types[MAX_PARAMS];
    /* Literal AST node for each parameter's default value, NULL when the
       parameter has none.  Nodes are shared with the declaring AST and are
       never freed (one-shot compiler). */
    AstNode* param_defaults[MAX_PARAMS];
    int  is_native;
    int  is_override;
    int  is_private;
    int  method_is_static;
    /* Declaration site of the method, used by diagnostics.  Zeroed for
       builtin registrations. */
    Token method_token;
    struct MethodInfo* next;
} MethodInfo;

typedef struct InterfaceMethodInfo {
    char name[NAME_BUF_SIZE];
    Type return_type;
    int  param_count;
    char param_names[MAX_PARAMS][NAME_BUF_SIZE];
    Type param_types[MAX_PARAMS];
    /* Literal AST node for each parameter's default value, NULL when none. */
    AstNode* param_defaults[MAX_PARAMS];
    AstNode* interface_method_default_body;
    int  interface_method_line;
} InterfaceMethodInfo;

#define MAX_IFACE_METHODS 32

typedef struct InterfaceInfo {
    char name[NAME_BUF_SIZE];
    int  type_id;
    int  method_count;
    InterfaceMethodInfo methods[MAX_IFACE_METHODS];
    struct InterfaceInfo* next;
} InterfaceInfo;

/* Property (C# style): `i32 Count { get { ... } set { ... } }` in a class
   body.  Accessors are synthesized as ordinary get_X/set_X methods at parse
   time and flow through the normal method machinery; this record drives the
   obj.X read/write dispatch in codegen.  Value types only. */
typedef struct PropertyInfo {
    char name[NAME_BUF_SIZE];
    Type prop_type;
    int  has_get;
    int  has_set;
    int  is_private;
    struct PropertyInfo* next;
} PropertyInfo;

typedef struct ClassInfo {
    char name[NAME_BUF_SIZE];
    int  type_id;
    int  field_count;
    char field_names[MAX_FIELDS][NAME_BUF_SIZE];
    Type field_types[MAX_FIELDS];
    int  field_private[MAX_FIELDS];
    MethodInfo* methods;
    int  impl_count;
    char impl_names[MAX_IMPL][NAME_BUF_SIZE];
    InterfaceInfo* impl_infos[MAX_IMPL];

    int  is_generic;
    int  generic_param_count;
    char generic_params[MAX_GENERIC_PARAMS][NAME_BUF_SIZE];
    int  generic_constraint_count[MAX_GENERIC_PARAMS];
    char generic_constraints[MAX_GENERIC_PARAMS][MAX_CONSTRAINTS_PER_PARAM][NAME_BUF_SIZE];
    int  generic_has_new[MAX_GENERIC_PARAMS];
    AstNode* generic_ast;

    int  is_instantiation;
    struct ClassInfo* generic_def;
    int  instantiation_arg_count;
    Type instantiation_args[MAX_GENERIC_PARAMS];
    char mangled_name[128];

    PropertyInfo* properties;

    /* Set once sema has walked this instantiation's substituted body
       (sema_check's generic-instantiation fixpoint); prevents re-walking
       self-referential instantiations. */
    int  sema_walked;

    struct ClassInfo* next;
} ClassInfo;

typedef struct StructInfo {
    char name[NAME_BUF_SIZE];
    int  type_id;
    int  field_count;
    char field_names[MAX_FIELDS][NAME_BUF_SIZE];
    Type field_types[MAX_FIELDS];
    MethodInfo* methods;
    int  has_ref_fields;     /* transitive: owns class/interface/weak shares */
    int  visit_state;        /* scratch marker for cycle detection */
    int  emitted_in_header;  /* set once the C definition has been emitted */
    struct StructInfo* next;
} StructInfo;

/* Simple enum (C++ enum class style): unit variants with i32 values.
   The per-variant field tables are modeled after StructInfo's field table so
   that payload enums (tagged unions) can be added later without changing the
   registry shape; v1 only allows zero-field variants, so has_payloads is
   always 0 and the field tables stay empty. */
typedef struct EnumInfo {
    char name[NAME_BUF_SIZE];
    int  variant_count;
    char variant_names[MAX_FIELDS][NAME_BUF_SIZE];
    long variant_values[MAX_FIELDS];   /* explicit or auto-incremented */
    /* Reserved for payload enums; always empty in v1: */
    int  variant_field_counts[MAX_FIELDS];
    Type variant_fields[MAX_FIELDS][MAX_FIELDS];
    int  has_payloads;                 /* always 0 in v1 */
    struct EnumInfo* next;
} EnumInfo;

typedef struct SymEntry {
    char name[NAME_BUF_SIZE];
    Type type;
    struct SymEntry* next;
} SymEntry;

/* Top-level or static-class-member const declaration:
   `const u32 X = 1;` / `const string S = "...";` at top level, or
   `static const i32 MAX = 10;` inside a class body.
   Scalar consts emit a C `static const` with the literal; string consts emit
   a runtime-initialized global String* (see codegen).  Top-level consts are
   also inserted into the global scope so uses resolve as ordinary
   identifiers; class members are reached only via `Class.NAME` member
   access, and their C name is `Class_NAME`. */
typedef struct ConstInfo {
    char name[NAME_BUF_SIZE];
    char owner_class[NAME_BUF_SIZE];  /* empty = top-level const */
    Type const_type;
    int  const_is_string;
    int  is_private;                  /* class members only */
    /* C literal text for scalars (e.g. "1", "-3", "'a'"); decoded content
       for strings (same form as AST_STRING_LIT token text). */
    char const_literal[TOKEN_TEXT_SIZE];
    int  xor_str_id;             /* encryption table id under --xor-strings */
    struct ConstInfo* next;
} ConstInfo;

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
int         symtab_add_field(ClassInfo* info, const char* name, Type type, int is_private);

void        symtab_add_struct(const char* name, StructInfo* info);
StructInfo* symtab_find_struct(const char* name);
StructInfo* symtab_first_struct(void);
void        symtab_add_enum(const char* name, EnumInfo* info);
EnumInfo*   symtab_find_enum(const char* name);
EnumInfo*   symtab_first_enum(void);
void        symtab_add_const(const char* name, ConstInfo* info);
ConstInfo*  symtab_find_const(const char* name);
ConstInfo*  symtab_first_const(void);
/* Finds a static const member of a class by source name (owner match). */
ConstInfo*  symtab_find_class_const(const char* class_name, const char* name);
int         symtab_add_struct_field(StructInfo* info, const char* name, Type type);
void        symtab_add_struct_method(StructInfo* st, const char* name, Type ret_type,
                                     int pc, const char pn[][64], const Type pt[],
                                     AstNode* const pd[]);
MethodInfo* symtab_find_struct_method(StructInfo* st, const char* method_name);
void        symtab_add_method(ClassInfo* cls, const char* name, Type ret_type,
                              int pc, const char pn[][64], const Type pt[],
                              AstNode* const pd[],
                              int is_native, int is_override, int is_private,
                              int is_static, Token tok);
MethodInfo* symtab_find_method(const char* class_name, const char* method_name);

void         symtab_add_property(ClassInfo* cls, PropertyInfo* info);
PropertyInfo* symtab_find_property(ClassInfo* cls, const char* name);

typedef struct FuncInfo {
    char name[NAME_BUF_SIZE];
    Type return_type;
    int  param_count;
    char param_names[MAX_PARAMS][NAME_BUF_SIZE];
    Type param_types[MAX_PARAMS];
    /* Literal AST node for each parameter's default value, NULL when none. */
    AstNode* param_defaults[MAX_PARAMS];
    int  is_builtin;
    struct FuncInfo* next;
} FuncInfo;

void       symtab_add_func(const char* name, Type ret_type,
                           int pc, const char pn[][64], const Type pt[],
                           AstNode* const pd[], int is_builtin);
FuncInfo*  symtab_find_func(const char* name);
FuncInfo*  symtab_first_func(void);

void           symtab_add_interface(const char* name, InterfaceInfo* info);
InterfaceInfo* symtab_find_interface(const char* name);
int  symtab_add_interface_method(InterfaceInfo* iface, const char* name,
                                 Type ret_type, int pc,
                                 const char pn[][64], const Type pt[],
                                 AstNode* const pd[],
                                 AstNode* default_body, int line);
InterfaceMethodInfo* symtab_find_interface_method(InterfaceInfo* iface,
                                                   const char* method_name);

void symtab_add_class_impl(ClassInfo* cls, const char* iface_name);

int  symtab_validate_impls(void);
int  symtab_validate_structs(void);
int  symtab_validate_generic_method_calls(ClassInfo* generic_def);

int  symtab_next_type_id(void);
void symtab_mark_class_generic(ClassInfo* info, AstNode* ast, int param_count, const char* params[]);
ClassInfo* symtab_find_class_by_mangled(const char* mangled_name);
ClassInfo* symtab_add_class_instantiation(ClassInfo* generic_def, const Type** args, int arg_count);
ClassInfo* symtab_instantiate_class_from_type(Type* t);
MethodInfo* symtab_find_method_in_class(ClassInfo* cls, const char* method_name);

#endif
