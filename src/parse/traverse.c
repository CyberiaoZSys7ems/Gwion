#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "env.h"
#include "traverse.h"

ANN m_bool scan0_ast(const Env env, Ast ast);
ANN m_bool scan1_ast(const Env env, Ast ast);
ANN m_bool scan2_ast(const Env env, Ast ast);
ANN m_bool check_ast(const Env env, Ast ast);

ANN m_bool scan1_exp_decl(const Env env, const Exp_Decl* decl);
ANN m_bool scan2_exp_decl(const Env env, const Exp_Decl* decl);
ANN Type   check_exp_decl(const Env env, const Exp_Decl* decl);

ANN m_bool scan1_func_def(const Env env, const Func_Def def);
ANN m_bool scan2_func_def(const Env env, const Func_Def def);
ANN m_bool check_func_def(const Env env, const Func_Def def);

ANN m_bool scan0_stmt_fptr(const Env env, const Stmt_Fptr def);
ANN m_bool scan1_stmt_fptr(const Env env, const Stmt_Fptr def);
ANN m_bool scan2_stmt_fptr(const Env env, const Stmt_Fptr def);
//ANN m_bool check_stmt_fptr(const Env env, const Stmt_Fptr def);

ANN m_bool scan1_stmt_union(const Env env, const Stmt_Union def);
ANN m_bool scan2_stmt_union(const Env env, const Stmt_Union def);
ANN m_bool check_stmt_union(const Env env, const Stmt_Union def);

ANN m_bool scan0_stmt_enum(const Env env, const Stmt_Enum def);
ANN m_bool scan1_stmt_enum(const Env env, const Stmt_Enum def);
//ANN m_bool scan2_stmt_enum(const Env env, const Stmt_Enum def);
ANN m_bool check_stmt_enum(const Env env, const Stmt_Enum def);

ANN m_bool scan1_class_def(const Env env, const Class_Def def);
ANN m_bool scan2_class_def(const Env env, const Class_Def def);
ANN m_bool check_class_def(const Env env, const Class_Def def);

ANN m_bool traverse_ast(const Env env, const Ast ast) {
  CHECK_BB(scan0_ast(env, ast))
  CHECK_BB(scan1_ast(env, ast))
  CHECK_BB(scan2_ast(env, ast))
  return check_ast(env, ast);
}

ANN m_bool traverse_decl(const Env env, const Exp_Decl* decl) {
 CHECK_BB(scan1_exp_decl(env, decl))
 CHECK_BB(scan2_exp_decl(env, decl))
 return check_exp_decl(env, decl) ? 1 : -1;
}

ANN m_bool traverse_func_def(const Env env, const Func_Def def) {
  CHECK_BB(scan1_func_def(env, def))
  CHECK_BB(scan2_func_def(env, def))
  return check_func_def(env, def);
}

ANN m_bool traverse_stmt_union(const Env env, const Stmt_Union def) {
  CHECK_BB(scan1_stmt_union(env, def))
  CHECK_BB(scan2_stmt_union(env, def))
  return check_stmt_union(env, def);
}

ANN m_bool traverse_stmt_enum(const Env env, const Stmt_Enum def) {
  CHECK_BB(scan0_stmt_enum(env, def))
  CHECK_BB(scan1_stmt_enum(env, def))
//  CHECK_BBscan2_stmt_enum(env, def))
  return check_stmt_enum(env, def);
}

ANN m_bool traverse_stmt_fptr(const Env env, const Stmt_Fptr def) {
  CHECK_BB(scan0_stmt_fptr(env, def))
  CHECK_BB(scan1_stmt_fptr(env, def))
  return scan2_stmt_fptr(env, def);
// CHECK_BB(check_stmt_fptr(env, def))
}

ANN m_bool traverse_class_def(const Env env, const Class_Def def) {
  CHECK_BB(scan1_class_def(env, def))
  CHECK_BB(scan2_class_def(env, def))
  return check_class_def(env, def);
}
