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
   expression types into the ast_resolved_type cache.  Reports no diagnostics
   yet; check clusters migrate here from codegen incrementally (TODO_SEMA.md). */
void sema_check(AstNode* program);

#endif
