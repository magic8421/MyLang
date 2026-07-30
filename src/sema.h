#ifndef SEMA_H
#define SEMA_H

#include "ast.h"
#include "symtab.h"

/* Resolves the type of an expression node and caches the result in
   node->ast_resolved_type (guarded by node->ast_is_resolved).  Later calls
   return the cached type; nodes created or rewritten after the pass (generic
   instantiations, codegen-time lowering) resolve on demand as before.
   Reports no diagnostics: an unresolvable node yields a zeroed type, same as
   the old codegen-time resolver. */
Type sema_resolve_type(AstNode* node);

/* Resolves the static method targeted by a class-name call 'ClassName.m(...)'.
   Returns NULL when the callee is not a member access on a bare identifier,
   the identifier names an in-scope variable (instance call), the class does
   not exist, or the class has no such method.  Generic class definitions are
   rejected: static calls on generic classes are not supported. */
MethodInfo* sema_static_call_method(AstNode* callee, ClassInfo** out_ci);

/* Forward semantic pass over the whole program.  Walks every non-generic
   function/method body mirroring codegen's scope discipline and pre-resolves
   expression types into the ast_resolved_type cache.  Migrated check clusters
   report diagnostics here; the rest still report from codegen (TODO_SEMA.md). */
void sema_check(AstNode* program);

/* Non-zero once sema has reported an error; main skips codegen in that case
   so a migrated diagnostic is never reported twice. */
int sema_had_error(void);

/* Compound-assignment operator predicates (shared with codegen). */
int is_compound_assign_op(TokenKind k);
int is_bit_compound_op(TokenKind k);

/* Array builtin method names (shared with codegen). */
int is_array_method_name(const char* s);

/* Interface-implementation predicate (shared with codegen). */
int class_implements(ClassInfo* ci, const char* iname);

/* Class lookup that also materialises generic instantiations (shared with
   codegen). */
ClassInfo* class_info_for_type(Type* t);

/* Element type of a MyArray value type (shared with codegen). */
Type array_elem_type(const Type* arr_type);

/* If node is a member access that resolves to a class property, returns the
   PropertyInfo and optionally the owning class.  Used by the sema assignment
   checks and by codegen's prepare-time property lowering, the expr_is_owned
   safety net, and the property read/write/increment dispatch paths. */
PropertyInfo* member_access_property(AstNode* node, ClassInfo** out_ci);

/* True when the expression is a lock() call on an unowned reference. */
int expr_is_unowned_lock(AstNode* node);

/* Structs that (transitively) own reference-counted shares get compiler-
   generated _mylang_retain_S / _mylang_release_S hooks: copies retain each
   reference field and scope exit releases them. */
int struct_has_ref_fields(const Type* t);

/* Arrays of such structs are rejected: MyArray is type-erased and cannot run
   per-element retain/release hooks yet. */
int type_is_ref_struct_array(const Type* t);

#endif
