#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "traverse.h"
#include "instr.h"
#include "emit.h"
#include "gwion.h"
#include "object.h"
#include "operator.h"
#include "import.h"
#include "gwi.h"
#include "gwi.h"
#include "specialid.h"
#include "pass.h"

ANN void gwi_register_freearg(const Gwi gwi, const f_instr _exec, const f_freearg _free) {
  map_set(&gwi->gwion->data->freearg, (vtype)_exec, (vtype)_free);
}

ANN void gwi_register_pass(const Gwi gwi, const m_str name, const compilation_pass pass) {
  pass_register(gwi->gwion, name, pass);
}

ANN void gwi_reserve(const Gwi gwi, const m_str str) {
  vector_add(&gwi->gwion->data->reserved, (vtype)insert_symbol(gwi->gwion->st, str));
}

ANN void gwi_specialid(const Gwi gwi, const m_str id, const SpecialId spid) {
  struct SpecialId_ *a = mp_calloc(gwi->gwion->mp, SpecialId);
  memcpy(a, spid, sizeof(struct SpecialId_));
  map_set(&gwi->gwion->data->id, (vtype)insert_symbol(gwi->gwion->st, id), (vtype)a);
  gwi_reserve(gwi, id);
}

ANN void gwi_set_loc(const Gwi gwi, const m_str file, const uint line) {
  gwi->loc.first.line = gwi->loc.last.line = line;
  gwi->gwion->env->name = file;
}

ANN static m_bool mk_gack(MemPool p, const Type type, const f_gack d) {
  const VM_Code code = new_vmcode(p, NULL, SZ_INT, 1, "@gack");
  code->native_func = (m_uint)d;
  type->info->gack = code;
  return GW_OK;
}

ANN m_bool gwi_gack(const Gwi gwi, const Type type, const f_gack d) {
  return mk_gack(gwi->gwion->mp, type, d);
}

ANN VM* gwi_vm(const Gwi gwi) {
  return gwi->gwion->vm;
}
