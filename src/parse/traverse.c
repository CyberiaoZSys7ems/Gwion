#include "absyn.h"

m_bool traverse_ast(Env env, Ast ast) {
  if(scan0_Ast(env, ast) < 0 ||
     scan1_ast(env, ast) < 0 ||
     scan2_ast(env, ast) < 0 ||
     check_ast(env, ast) < 0)
    return -1;
  return 1;
}

m_bool traverse_decl(Env env, Exp_Decl* decl) {
  if(scan1_exp_decl(env, decl) < 0 ||
     scan2_exp_decl(env, decl) < 0 ||
     check_exp_decl(env, decl) ? 0:1)
    return -1;
  return 1;
}

m_bool traverse_def(Env env, Func_Def def) {
  if(scan1_func_def(env, def) < 0 ||
     scan2_func_def(env, def) < 0 ||
     check_func_def(env, def) < 0)
    return -1;
  return 1;
}
