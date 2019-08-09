#include <stdlib.h>
#include <string.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "gwion.h"
#include "type.h"
#include "nspc.h"
#include "value.h"
#include "instr.h"
#include "emit.h"
#include "object.h"
#include "array.h"
#include "func.h"
#include "traverse.h"
#include "template.h"
#include "escape.h"
#include "parse.h"
#include "memoize.h"
#include "operator.h"
#include "import.h"
#include "switch.h"
#include "parser.h"
#include "tuple.h"

#undef insert_symbol
#define insert_symbol(a) insert_symbol(emit->gwion->st, (a))

#undef ERR_B
#define ERR_B(a, b, ...) { env_err(emit->env, (a), (b), ## __VA_ARGS__); return GW_ERROR; }
#undef ERR_O
#define ERR_O(a, b, ...) { env_err(emit->env, (a), (b), ## __VA_ARGS__); return NULL; }

typedef struct Local_ {
  m_uint size;
  m_uint offset;
  m_bool is_obj;
} Local;

static inline void emit_pop_type(const Emitter emit) { nspc_pop_type(emit->gwion->mp, emit->env->curr); }
static inline void emit_pop(const Emitter emit, const m_uint scope) { env_pop(emit->env, scope); }
static inline m_uint emit_push(const Emitter emit, const Type type, const Nspc nspc) {
  return env_push(emit->env, type, nspc);
}
static inline m_uint emit_push_type(const Emitter emit, const Type type) {
  return env_push_type(emit->env, type);
}
static inline m_uint emit_push_global(const Emitter emit) {
  return env_push_global(emit->env);
}

ANEW static Frame* new_frame(MemPool p) {
  Frame* frame = mp_calloc(p, Frame);
  vector_init(&frame->stack);
  vector_add(&frame->stack, (vtype)NULL);
  return frame;
}

ANN static void free_frame(MemPool p, Frame* a) {
  LOOP_OPTIM
  for(vtype i = vector_size(&a->stack) + 1; --i;)
    if(vector_at(&a->stack, i - 1))
      mp_free(p, Local, (Local*)vector_at(&a->stack, i - 1));
  vector_release(&a->stack);
  mp_free(p, Frame, a);
}

ANN static Local* new_local(MemPool p, const m_uint size, const m_bool is_obj) {
  Local* local  = mp_calloc(p, Local);
  local->size   = size;
  local->is_obj = is_obj;
  return local;
}

ANN static m_uint frame_local(MemPool p, Frame* frame, const m_uint size, const m_bool is_obj) {
  Local* local = new_local(p, size, is_obj);
  local->offset = frame->curr_offset;
  frame->curr_offset += size;
  vector_add(&frame->stack, (vtype)local);
  return local->offset;
}

ANN static inline void frame_push(Frame* frame) {
  vector_add(&frame->stack, (vtype)NULL);
}

ANN static m_int frame_pop(Frame* frame) {
  DECL_OB(const Local*, l, = (Local*)vector_pop(&frame->stack))
  frame->curr_offset -= l->size;
  return l->is_obj ? (m_int)l->offset : frame_pop(frame);
}

ANN static m_bool emit_exp(const Emitter emit, Exp exp, const m_bool add_ref);
ANN static m_bool emit_stmt(const Emitter emit, const Stmt stmt, const m_bool pop);
ANN static m_bool emit_stmt_list(const Emitter emit, Stmt_List list);
ANN static m_bool emit_exp_dot(const Emitter emit, const Exp_Dot* member);
ANN static m_bool emit_func_def(const Emitter emit, const Func_Def func_def);

ANEW static Code* new_code(const Emitter emit, const m_str name) {
  Code* code = mp_calloc(emit->gwion->mp, Code);
  code->name = code_name_set(emit->gwion->mp, name, emit->env->name);
  vector_init(&code->instr);
  vector_init(&code->stack_break);
  vector_init(&code->stack_cont);
  vector_init(&code->stack_return);
  code->frame = new_frame(emit->gwion->mp);
  return code;
}

ANN static void free_code(MemPool p, Code* code) {
  vector_release(&code->instr);
  vector_release(&code->stack_break);
  vector_release(&code->stack_cont);
  vector_release(&code->stack_return);
  free_frame(p, code->frame);
  free_mstr(p, code->name);
  mp_free(p, Code, code);
}

ANN static void emit_pop_scope(const Emitter emit) {
  m_int offset;
  while((offset = frame_pop(emit->code->frame)) > -1) {
    Instr instr = emit_add_instr(emit, ObjectRelease);
    instr->m_val = (m_uint)offset;
  }
  vector_pop(&emit->pure);
}

ANN static inline void emit_push_code(const Emitter emit, const m_str name) {
  vector_add(&emit->stack, (vtype)emit->code);
  emit->code = new_code(emit, name);
}

ANN static inline void emit_pop_code(const Emitter emit)   {
  emit->code = (Code*)vector_pop(&emit->stack);
}

ANN static inline void emit_push_scope(const Emitter emit) {
  frame_push(emit->code->frame);
  vector_add(&emit->pure, 0);
}

ANN static inline m_uint emit_code_size(const Emitter emit) {
  return vector_size(&emit->code->instr);
}

ANN /*static inline */m_uint emit_code_offset(const Emitter emit) {
  return emit->code->frame->curr_offset;
}

ANN /*static inline */m_uint emit_local(const Emitter emit, const m_uint size, const m_bool is_obj) {
  return frame_local(emit->gwion->mp, emit->code->frame, size, is_obj);
}

ANN static void emit_pre_ctor(const Emitter emit, const Type type) {
  if(type->e->parent)
    emit_pre_ctor(emit, type->e->parent);
  if(type->nspc->pre_ctor)
    emit_ext_ctor(emit, type->nspc->pre_ctor);
  if(GET_FLAG(type, template) && GET_FLAG(type, builtin)) {
    const Type t = template_parent(emit->env, type);
    if(t->nspc->pre_ctor)
      emit_ext_ctor(emit, t->nspc->pre_ctor);
  }
}

#define regxxx(name, instr) \
ANN static inline Instr reg##name(const Emitter emit, const m_uint sz) { \
  const Instr instr = emit_add_instr(emit, Reg##instr); \
  instr->m_val = sz; \
  return instr; \
}
regxxx(pop, Pop)
regxxx(pushi, PushImm)
regxxx(seti, SetImm)

ANN static void emit_pre_constructor_array(const Emitter emit, const Type type) {
  const m_uint start_index = emit_code_size(emit);
  const Instr top = emit_add_instr(emit, ArrayTop);
  top->m_val2 = (m_uint)type;
  emit_pre_ctor(emit, type);
  const Instr bottom = emit_add_instr(emit, ArrayBottom);
  regpop(emit, SZ_INT);
  const Instr pc = emit_add_instr(emit, Goto);
  pc->m_val = start_index;
  top->m_val = emit_code_size(emit);
  bottom->m_val = start_index;
  regpop(emit, SZ_INT*3);
  emit_add_instr(emit, ArrayPost);
}

ANN ArrayInfo* emit_array_extend_inner(const Emitter emit, const Type t, const Exp e) {
  CHECK_BO(emit_exp(emit, e, 0))
  const Type base = array_base(t);
  ArrayInfo* info = mp_calloc(emit->gwion->mp, ArrayInfo);
  vector_init(&info->type);
  for(m_uint i = 1; i < t->array_depth; ++i)
    vector_add(&info->type, (vtype)array_type(emit->env, base, i));
  vector_add(&info->type, (vtype)t);
  info->depth = (m_int)t->array_depth;
  info->base = base;
  const Instr alloc = emit_add_instr(emit, ArrayAlloc);
  alloc->m_val = (m_uint)info;
  if(isa(base, t_object) > 0 && !GET_FLAG(base, abstract)) {
    emit_pre_constructor_array(emit, base);
    info->is_obj = 1;
  }
  return info;
}

ANN void emit_ext_ctor(const Emitter emit, const VM_Code code) {
  const Instr cpy = emit_add_instr(emit, Reg2Reg);
  cpy->m_val2 = -SZ_INT;
  const Instr set_code = regseti(emit, (m_uint)code);
  set_code->m_val2 = SZ_INT;
  const m_uint offset = emit_code_offset(emit);
  const Instr regset = regseti(emit, offset);
  regset->m_val2 = SZ_INT *2;
  const Instr push = emit_add_instr(emit, RegPush);
  push->m_val = SZ_INT *2;
  const Instr prelude = emit_add_instr(emit, !GET_FLAG(code, builtin) ? FuncUsr : FuncMember);
  prelude->m_val2 = 2;
  prelude->m_val = SZ_INT;
  emit_add_instr(emit, Reg2Mem);
  const Instr next = emit_add_instr(emit, Overflow);
  next->m_val2 = offset;
}

ANN m_bool emit_array_extend(const Emitter emit, const Type t, const Exp e) {
  CHECK_OB(emit_array_extend_inner(emit, t, e))
  emit_add_instr(emit, PopArrayClass);
  return GW_OK;
}

ANN static inline void emit_notpure(const Emitter emit) {
  ++VPTR(&emit->pure, VLEN(&emit->pure));
}

ANN static Array_Sub instantiate_typedef(MemPool p, const m_uint depth) {
  const Exp base = new_exp_prim_int(p, 0, new_loc(p, __LINE__));
  Exp e = base;
  for(m_uint i = 0; i < depth; ++i)
     e = (e->next = new_exp_prim_int(p, 0, new_loc(p, __LINE__)));
  return new_array_sub(p, base);
}

ANN2(1,2) m_bool emit_instantiate_object(const Emitter emit, const Type type,
      const Array_Sub arr, const m_bool is_ref) {
  if(type->array_depth) {
    assert(!arr || arr->depth == type->array_depth);
    const Array_Sub array = arr ?: instantiate_typedef(emit->gwion->mp, type->array_depth);
    assert(array->exp);
    DECL_OB(ArrayInfo*, info, = emit_array_extend_inner(emit, type, array->exp))
    info->is_ref = is_ref;
    if(!arr)
      free_array_sub(emit->gwion->mp, array);
  } else if(!is_ref) {
    const Instr instr = emit_add_instr(emit, ObjectInstantiate);
    instr->m_val2 = (m_uint)type;
    emit_pre_ctor(emit, type);
  }
  emit_notpure(emit);
  return GW_OK;
}

static inline enum Kind kindof(const m_uint size, const uint emit_var) {
  if(emit_var)
    return KIND_ADDR;
  return size == SZ_INT ? KIND_INT : size == SZ_FLOAT ? KIND_FLOAT : KIND_OTHER;
}

ANN static Instr emit_kind(Emitter emit, const m_uint size, const uint addr, const f_instr func[]) {
  const enum Kind kind = kindof(size, addr);
  const Instr instr = emit_add_instr(emit, func[kind]);
  instr->m_val2 = size;
  return instr;
}

static const f_instr regpushimm[] = { RegPushImm, RegPushImm2, RegPushImm3, RegPushImm4 };
static const f_instr regpushmem[] = { RegPushMem, RegPushMem2, RegPushMem3, RegPushMem4 };
static const f_instr regpushbase[] = { RegPushBase, RegPushBase2, RegPushBase3, RegPushBase4 };
static const f_instr dotstatic[]  = { DotStatic, DotStatic2, DotStatic3, RegPushImm };
static const f_instr dotmember[]  = { DotMember, DotMember2, DotMember3, DotMember4 };
static const f_instr allocmember[]  = { RegPushImm, RegPushImm2, RegPushImm3, AllocMember4 };
static const f_instr allocword[]  = { AllocWord, AllocWord2, AllocWord3, RegPushMem4 };

ANN static inline Exp this_exp(const Emitter emit, const Type t, const loc_t pos) {
  const Exp exp = new_exp_prim_id(emit->gwion->mp, insert_symbol("this"),
    loc_cpy(emit->gwion->mp, pos));
  exp->type = t;
  return exp;
}

ANN static inline Exp dot_this_exp(const Emitter emit, const Exp_Primary* prim, const Type t) {
  const Exp exp = this_exp(emit, t, exp_self(prim)->pos);
  const Exp dot = new_exp_dot(emit->gwion->mp, exp, prim->d.var);
  dot->d.exp_dot.t_base = t;
  return dot;
}

ANN static inline Exp dot_static_exp(const Emitter emit, const Exp_Primary* prim, const Type t) {
  const Symbol s = insert_symbol(t->name);
  const Exp    e = new_exp_prim_id(emit->gwion->mp, s,
    loc_cpy(emit->gwion->mp, exp_self(prim)->pos));
  const Value  val = nspc_lookup_value1(t->nspc->parent, s);
  const Exp dot = new_exp_dot(emit->gwion->mp, e, prim->d.var);
  dot->d.exp_dot.t_base = val->type;
  return dot;
}

ANN static m_bool emit_symbol_owned(const Emitter emit, const Exp_Primary* prim) {
  const Value v = prim->value;
  const Exp dot = (!GET_FLAG(v, static) ? dot_this_exp : dot_static_exp)(emit, prim, v->owner_class);
  dot->type = exp_self(prim)->type;
  dot->emit_var = exp_self(prim)->emit_var;
  const m_bool ret = emit_exp_dot(emit, &dot->d.exp_dot);
  free_exp(emit->gwion->mp, dot);
  return ret;
}

ANN static m_bool emit_symbol_builtin(const Emitter emit, const Exp_Primary* prim) {
  const Value v = prim->value;
  if(GET_FLAG(v, func)) {
    regpushi(emit, (m_uint)v->d.func_ref);
    return GW_OK;
  }
  if(GET_FLAG(v, union)) {
    const m_uint size = v->type->size;
    const Instr instr = emit_kind(emit, size, exp_self(prim)->emit_var, dotstatic);
    instr->m_val = (m_uint)v->d.ptr;
  } else {
    const m_uint size = v->type->size;
    const Instr instr = emit_kind(emit, size, exp_self(prim)->emit_var, regpushimm);
    if(!exp_self(prim)->emit_var && size == SZ_INT) {
      if(isa(v->type, t_object) > 0) {
        instr->opcode = eRegPushImm;
        instr->m_val = (m_uint)v->d.ptr;
      } else if(v->d.ptr)
        instr->m_val = *(m_uint*)v->d.ptr;
    } else if(v->d.ptr)
      memcpy(&instr->m_val, v->d.ptr, v->type->size);
    else
      instr->m_val = (m_uint)v->d.ptr;
    instr->m_val2 = size;
  }
  return GW_OK;
}

ANN static m_bool emit_symbol(const Emitter emit, const Exp_Primary* prim) {
  const Value v = prim->value;
  if(v->owner_class)
    return emit_symbol_owned(emit, prim);
  if(GET_FLAG(v, builtin) || GET_FLAG(v, union) || GET_FLAG(v, enum))
    return emit_symbol_builtin(emit, prim);
  const m_uint size = v->type->size;
  const Instr instr = emit_kind(emit, size, exp_self(prim)->emit_var, !GET_FLAG(v, global) ? regpushmem : regpushbase);
  instr->m_val  = v->offset;
  if(isa(v->type, t_function) > 0 && isa(v->type, t_fptr) < 0)
    instr->m_val = exp_self(prim)->type->e->d.func->value_ref->offset;
  return GW_OK;
}

ANEW ANN VM_Code emit_code(const Emitter emit) {
  Code* c = emit->code;
  const VM_Code code = new_vm_code(emit->gwion->mp, &c->instr, c->stack_depth,
      c->flag, c->name);
  free_code(emit->gwion->mp, c);
  return code;
}

ANN static VM_Code finalyze(const Emitter emit) {
  emit_add_instr(emit, EOC);
  const VM_Code code = emit_code(emit);
  emit_pop_code(emit);
  return code;
}

ANN static m_bool prim_array(const Emitter emit, const Exp_Primary * primary) {
  const Array_Sub array = primary->d.array;
  Exp e = array->exp;
  CHECK_BB(emit_exp(emit, e, 0))
  m_uint count = 0;
  do ++count;
  while((e = e->next));
  const Type type = array->type;
  regseti(emit, count);
  const Instr instr = emit_add_instr(emit, ArrayInit);
  instr->m_val = (m_uint)type;
  instr->m_val2 = type->array_depth == 1 ? array_base(type)->size : SZ_INT;
  emit_add_instr(emit, GcAdd);
  emit_notpure(emit);
  return GW_OK;
}

ANN static m_bool emit_exp_array(const Emitter emit, const Exp_Array* array) {
  const m_uint is_var = exp_self(array)->emit_var;
  CHECK_BB(emit_exp(emit, array->base, 0))
  const m_uint depth = get_depth(array->base->type) - get_depth(exp_self(array)->type);

const Type t_base = array->base->type;
Type type = array_base(t_base) ?: t_base;
if(isa(type, t_tuple) > 0 && isa(exp_self(array)->type, t_tuple) < 0) {
  Exp e = array->array->exp;
  while(e->next && e->next->next)
    e = e->next;
  const Exp next = e->next;
  if(!next) {
      emit_add_instr(emit, GWOP_EXCEPT);
      const Instr instr = emit_add_instr(emit, TupleMember);
      instr->m_val = array->array->exp->d.exp_primary.d.num;
      instr->m_val2 = is_var;
      emit_add_instr(emit, DotMember); // just a place holder.
      return GW_OK;
    }
    emit_add_instr(emit, GcAdd);
    e->next = NULL;
    CHECK_BB(emit_exp(emit, array->array->exp, 0))
    e->next = next;
    regpop(emit, (depth) * SZ_INT);
    emit_add_instr(emit, GWOP_EXCEPT);
assert(depth);
    if(depth)
    for(m_uint i = 0; i < depth-1; ++i) {
      const Instr access = emit_add_instr(emit, ArrayAccess);
      access->m_val = i;
      const Instr get = emit_add_instr(emit, ArrayGet);
      get->m_val = i;
      get->m_val2 = -SZ_INT;
      emit_add_instr(emit, GWOP_EXCEPT);
    }
    regpop(emit, SZ_INT);
    const Instr access = emit_add_instr(emit, ArrayAccess);
    access->m_val = depth;
    const Instr get = emit_add_instr(emit, ArrayGet);
    get->m_val = depth;
    const Instr push = emit_add_instr(emit, ArrayValid);
    push->m_val = SZ_INT;
    emit_add_instr(emit, GWOP_EXCEPT);
    const Instr instr = emit_add_instr(emit, TupleMember);
    instr->m_val = next->d.exp_primary.d.num;
    instr->m_val2 = is_var;
    emit_add_instr(emit, DotMember); // just a place holder.
    return GW_OK;
  }
  emit_add_instr(emit, GcAdd);
  CHECK_BB(emit_exp(emit, array->array->exp, 0))
  regpop(emit, depth * SZ_INT);
  emit_add_instr(emit, GWOP_EXCEPT);
  for(m_uint i = 0; i < depth - 1; ++i) {
    const Instr access = emit_add_instr(emit, ArrayAccess);
    access->m_val = i;
    const Instr get = emit_add_instr(emit, ArrayGet);
    get->m_val = i;
    get->m_val2 = -SZ_INT;
    emit_add_instr(emit, GWOP_EXCEPT);
  }
  regpop(emit, SZ_INT);
  const Instr access = emit_add_instr(emit, ArrayAccess);
  access->m_val = depth;
  const Instr get = emit_add_instr(emit, is_var ? ArrayAddr : ArrayGet);
  get->m_val = depth;
  const Instr push = emit_add_instr(emit, ArrayValid);
  push->m_val = is_var ? SZ_INT : exp_self(array)->type->size;
  return GW_OK;
}

ANN static m_bool prim_vec(const Emitter emit, const Exp_Primary * primary) {
  const Vec * vec = &primary->d.vec;
  const ae_prim_t t = primary->primary_type;
  CHECK_BB(emit_exp(emit, vec->exp, 0));
  m_int n = (m_int)((t == ae_primary_vec ? 3 : 2) - vec->dim + 1);
  while(--n > 0)
    emit_add_instr(emit, RegPushImm2);
  if(exp_self(primary)->emit_var) {
    emit_local(emit, exp_self(primary)->type->size, 0);
    const m_uint offset = emit_local(emit, SZ_INT, 0);
    const Instr cpy = emit_add_instr(emit, VecCpy);
    cpy->m_val = offset;
    cpy->m_val2 = exp_self(primary)->type->size;
    const Instr instr = emit_add_instr(emit, RegPushMem);
    instr->m_val = offset;
  }
  return GW_OK;
}

ANN static m_bool prim_id(const Emitter emit, const Exp_Primary* prim) {
  if(prim->d.var == insert_symbol("this"))
    emit_add_instr(emit, RegPushMem);
  else if(prim->d.var == insert_symbol("me"))
    emit_add_instr(emit, RegPushMe);
  else if(prim->d.var == insert_symbol("now"))
    emit_add_instr(emit, RegPushNow);
  else if(prim->d.var == insert_symbol("maybe"))
    emit_add_instr(emit, RegPushMaybe);
  else
    emit_symbol(emit, prim);
  return GW_OK;
}

ANN static m_bool prim_tuple(const Emitter emit, const Exp_Primary * primary) {
  CHECK_BB(emit_exp(emit, primary->d.tuple.exp, 1))
  const Instr instr = emit_add_instr(emit, TupleCtor);
  instr->m_val = (m_uint)exp_self(primary)->type;
  return GW_OK;
}

ANN static m_bool prim_num(const Emitter emit, const Exp_Primary * primary) {
  regpushi(emit, primary->d.num);
  return GW_OK;
}

ANN static m_bool prim_float(const Emitter emit, const Exp_Primary* primary) {
  const Instr instr = emit_add_instr(emit, RegPushImm2);
  instr->f = primary->d.fnum;
  return GW_OK;
}

ANN static m_bool prim_char(const Emitter emit, const Exp_Primary* prim) {
  DECL_BB(const m_int, c, = str2char(emit, prim->d.chr, exp_self(prim)->pos))
  regpushi(emit, c);
  return GW_OK;
}

ANN static m_bool prim_str(const Emitter emit, const Exp_Primary* prim) {
  char c[strlen(prim->d.str) + 1];
  if(strlen(prim->d.str)) {
    strcpy(c, prim->d.str);
    CHECK_BB(escape_str(emit, c, exp_self(prim)->pos));
  } else c[0] = '\0';
  const Value v = prim->value;
  const Symbol sym = insert_symbol(c);
  if(!v->d.ptr)
    v->d.ptr = (m_uint*)new_string2(emit->gwion->mp, NULL, s_name(sym));
  regpushi(emit, (m_uint)v->d.ptr);
  emit_add_instr(emit, RegAddRef);
  return GW_OK;
}

ANN static m_bool prim_gack(const Emitter emit, const Exp_Primary* primary) {
  const Exp exp = primary->d.exp;
  const Vector v = new_vector(emit->gwion->mp);
  m_uint offset = 0;
  Exp e = exp;
  do {
    vector_add(v, (vtype)e->type);
    offset += e->type->size;
  } while((e = e->next));
  if(emit_exp(emit, exp, 0) < 0) {
    free_vector(emit->gwion->mp, v);
    ERR_B(exp->pos, _("  ... in 'gack' expression."))
  }
  const Instr instr = emit_add_instr(emit, Gack);
  instr->m_val = offset;
  instr->m_val2 = (m_uint)v;
  return GW_OK;
}

#define prim_unpack dummy_func
static const _exp_func prim_func[] = {
  (_exp_func)prim_id, (_exp_func)prim_num, (_exp_func)prim_float, (_exp_func)prim_str,
  (_exp_func)prim_array, (_exp_func)prim_gack, (_exp_func)prim_vec, (_exp_func)prim_vec,
  (_exp_func)prim_vec, (_exp_func)prim_tuple, (_exp_func)prim_unpack,
  (_exp_func)prim_char, (_exp_func)dummy_func,
};

ANN static m_bool emit_exp_primary(const Emitter emit, const Exp_Primary* prim) {
  return prim_func[prim->primary_type](emit, prim);
}

ANN static m_bool emit_dot_static_data(const Emitter emit, const Value v, const uint emit_var) {
  const m_uint size = v->type->size;
  if(isa(v->type, t_class) < 0) {
    const Instr instr = emit_kind(emit, size, emit_var, dotstatic);
    instr->m_val = (m_uint)(v->owner->info->class_data + v->offset);
    instr->m_val2 = size;
  } else
    regpushi(emit, (m_uint)v->type);
  return GW_OK;
}

ANN static m_bool decl_static(const Emitter emit, const Var_Decl var_decl, const uint is_ref) {
  const Value v = var_decl->value;
  Code* code = emit->code;
  emit->code = (Code*)vector_back(&emit->stack);
  CHECK_BB(emit_instantiate_object(emit, v->type, var_decl->array, is_ref))
  CHECK_BB(emit_dot_static_data(emit, v, 1))
  emit_add_instr(emit, Assign);
  emit->code = code;
  return GW_OK;
}

ANN static m_bool emit_exp_decl_static(const Emitter emit, const Var_Decl var_decl, const uint is_ref, const uint emit_addr) {
  const Value value = var_decl->value;
  if(isa(value->type, t_object) > 0 && !is_ref)
    CHECK_BB(decl_static(emit, var_decl, 0))
  return emit_dot_static_data(emit, value, emit_addr);
}

ANN static m_bool emit_exp_decl_non_static(const Emitter emit, const Var_Decl var_decl,
  const uint is_ref, const uint emit_var) {
  const Value v = var_decl->value;
  const Type type = v->type;
  const Array_Sub array = var_decl->array;
  const m_bool is_array = array && array->exp;
  const m_bool is_obj = isa(type, t_object) > 0;
  const uint emit_addr = ((is_ref && !array) || isa(type, t_object) < 0) ?
    emit_var : 1;
  if(is_obj && (is_array || !is_ref)/* && !GET_FLAG(var_decl->value, ref)*/)
    CHECK_BB(emit_instantiate_object(emit, type, array, is_ref))
  f_instr *exec = (f_instr*)allocmember;
  if(!GET_FLAG(v, member)) {
    v->offset = emit_local(emit, v->type->size, is_obj);
    exec = (f_instr*)(allocword);
    if(GET_FLAG(var_decl->value, ref)) {
      const Instr clean = emit_add_instr(emit, MemSetImm);
      clean->m_val = v->offset;
    }
  }
  const Instr instr = emit_kind(emit, v->type->size, emit_addr, exec);
  instr->m_val = v->offset;
  instr->m_val2 = v->type->size;
  if(is_obj && (is_array || !is_ref)) {
    emit_add_instr(emit, Assign);
    const size_t missing_depth = type->array_depth - (array ? array->depth : 0);
    if(missing_depth) {
      const Instr push = emit_add_instr(emit, Reg2Reg);
      push->m_val = -(1 + missing_depth) * SZ_INT;
      regpop(emit, (missing_depth) * SZ_INT);
    }
  }
  return GW_OK;
}

ANN static m_bool emit_exp_decl_global(const Emitter emit, const Var_Decl var_decl,
  const uint is_ref, const uint emit_var) {
  const Value v = var_decl->value;
  const Type type = v->type;
  const Array_Sub array = var_decl->array;
  const m_bool is_array = array && array->exp;
  const m_bool is_obj = isa(type, t_object) > 0;
  const uint emit_addr = ((is_ref && !array) || isa(type, t_object) < 0) ?
    emit_var : 1;
  if(is_obj && (is_array || !is_ref))
    CHECK_BB(emit_instantiate_object(emit, type, array, is_ref))
  const Instr instr = emit_kind(emit, v->type->size, emit_addr, dotstatic);
  v->d.ptr = mp_calloc2(emit->gwion->mp, v->type->size);
  SET_FLAG(v, union);
  instr->m_val = (m_uint)v->d.ptr;
  instr->m_val2 = v->type->size;
  if(is_obj && (is_array || !is_ref)) {
    const Instr assign = emit_add_instr(emit, Assign);
    assign->m_val = emit_var;
    if(isa(type, t_fork) < 0) { // beware fork
      const Instr instr = emit_add_instr(emit, RegAddRef);
      instr->m_val = emit_var;
    }
  }
  return GW_OK;
}

ANN static m_bool emit_class_def(const Emitter, const Class_Def);
ANN static m_bool emit_cdef(const Emitter, const Class_Def);

ANN static inline m_bool emit_exp_decl_template(const Emitter emit, const Exp_Decl* decl) {
  const Type t = decl->type;
  return !GET_FLAG(t, emit) ? emit_cdef(emit, t->e->def) : GW_OK;
}

ANN static m_bool emit_exp_decl(const Emitter emit, const Exp_Decl* decl) {
  Var_Decl_List list = decl->list;
  const uint ref = GET_FLAG(decl->td, ref) || type_ref(decl->type);
  const uint var = exp_self(decl)->emit_var;
  if(GET_FLAG(decl->type, template))
    CHECK_BB(emit_exp_decl_template(emit, decl))
  const m_bool global = GET_FLAG(decl->td, global);
  const m_uint scope = !global ? emit->env->scope->depth : emit_push_global(emit);
  do {
    const uint r = (list->self->array && list->self->array->exp && ref) ? 0 : (uint)(GET_FLAG(list->self->value, ref) + ref);
//    if(!GET_FLAG(list->self->value, used))
//      continue;
    if(GET_FLAG(decl->td, static))
      CHECK_BB(emit_exp_decl_static(emit, list->self, r, var))
    else if(!global)
      CHECK_BB(emit_exp_decl_non_static(emit, list->self, r, var))
    else
      CHECK_BB(emit_exp_decl_global(emit, list->self, r, var))
    if(GET_FLAG(list->self->value->type, nonnull))
      emit_add_instr(emit, GWOP_EXCEPT);
  } while((list = list->next));
  if(global)
    emit_pop(emit, scope);
  return GW_OK;
}

ANN static m_uint vararg_size(const Exp_Call* exp_call, const Vector kinds) {
  Exp e = exp_call->args;
  Arg_List l = exp_call->m_func->def->base->args;
  m_uint size = 0;
  while(e) {
    if(!l) {
      size += e->type->size;
      vector_add(kinds, e->type->size);
    } else
      l = l->next;
    e = e->next;
  }
  return size;
}

ANN static void emit_func_arg_vararg(const Emitter emit, const Exp_Call* exp_call) {
  const Instr instr = emit_add_instr(emit, VarargIni);
  const Vector kinds = new_vector(emit->gwion->mp);
  if((instr->m_val = vararg_size(exp_call, kinds)))
    instr->m_val2 = (m_uint)kinds;
  else {
    instr->opcode = eRegPushImm;
    free_vector(emit->gwion->mp, kinds);
  }
}

ANN static m_bool emit_func_args(const Emitter emit, const Exp_Call* exp_call) {
  if(exp_call->args)
    CHECK_BB(emit_exp(emit, exp_call->args, 1))
  if(GET_FLAG(exp_call->m_func->def, variadic))
    emit_func_arg_vararg(emit, exp_call);
  return GW_OK;
}

ANN static m_bool prepare_call(const Emitter emit, const Exp_Call* exp_call) {
  CHECK_BB(emit_func_args(emit, exp_call))
  return emit_exp(emit, exp_call->func, 0);
}

ANN static m_bool emit_exp_call(const Emitter emit, const Exp_Call* exp_call) {
  CHECK_BB(prepare_call(emit, exp_call))
  return emit_exp_call1(emit, exp_call->m_func);
}

ANN static m_bool emit_exp_binary(const Emitter emit, const Exp_Binary* bin) {
  const Exp lhs = bin->lhs;
  const Exp rhs = bin->rhs;
  struct Op_Import opi = { .op=bin->op, .lhs=lhs->type, .rhs=rhs->type, .pos=exp_self(bin)->pos, .data = (uintptr_t)bin };
  CHECK_BB(emit_exp(emit, lhs, 1))
  CHECK_BB(emit_exp(emit, rhs, 1))
  return op_emit(emit, &opi);
}

ANN static m_bool emit_exp_cast(const Emitter emit, const Exp_Cast* cast) {
  struct Op_Import opi = { .op=insert_symbol("$"), .lhs=cast->exp->type, .rhs=exp_self(cast)->type, .data=(uintptr_t)cast};
  CHECK_BB(emit_exp(emit, cast->exp, 0))
  (void)op_emit(emit, &opi);
  return GW_OK;
}

ANN static m_bool emit_exp_post(const Emitter emit, const Exp_Postfix* post) {
  struct Op_Import opi = { .op=post->op, .lhs=post->exp->type, .data=(uintptr_t)post };
  CHECK_BB(emit_exp(emit, post->exp, 1))
  return op_emit(emit, &opi);
}

ANN static m_bool is_special(const Type t) {
  if(isa(t, t_complex) > 0 || isa(t, t_polar) > 0 ||
     isa(t, t_vec3)    > 0 || isa(t, t_vec4)  > 0 ||
     isa(t, t_vararg)  > 0)
    return GW_OK;
  return GW_ERROR;
}

ANN static Type_List tmpl_tl(const Env env, const m_str name) {
  const m_str start = strchr(name, '<');
  const m_str end = strchr(name, '@');
  char c[strlen(name)];
  strcpy(c, start + 1);
  c[strlen(start) - strlen(end) - 2] = '\0';
  m_uint depth;
  return str2tl(env, c, &depth);
}

ANN static inline m_bool traverse_emit_func_def(const Emitter emit, const Func_Def fdef) {
  if(!fdef->base->ret_type)
    CHECK_BB(traverse_func_def(emit->env, fdef))
  return emit_func_def(emit, fdef);
}

ANN m_bool traverse_dot_tmpl(const Emitter emit, const struct dottmpl_ *dt) {
  const m_uint scope = emit_push(emit, dt->owner_class, dt->owner);
  const m_bool ret = traverse_emit_func_def(emit, dt->def);
  emit_pop(emit, scope);
  return ret;
}

static inline m_bool push_func_code(const Emitter emit, const Func f) {
  if(!vector_size(&emit->code->instr))
    return GW_OK;
  const Instr instr = (Instr)vector_back(&emit->code->instr);
  if(instr->opcode == eDotTmplVal) {
    size_t len = strlen(f->name);
    size_t sz = len - strlen(f->value_ref->owner_class->name);
    char c[sz + 1];
    memcpy(c, f->name, sz);
    c[sz] = '\0';
    struct dottmpl_ *dt = mp_calloc(emit->gwion->mp, dottmpl);
    dt->name = s_name(insert_symbol(c));
    dt->vt_index = f->def->base->tmpl->base;
    dt->tl = tmpl_tl(emit->env, c);
    dt->base = f->def;
    instr->opcode = eOP_MAX;
    instr->m_val = (m_uint)dt;
    instr->m_val2 = strlen(c);
    instr->execute = DotTmpl;
    return GW_OK;
  }
  instr->opcode = eRegPushImm;
  instr->m_val = (m_uint)f->code;
  return GW_OK;
}

ANN static m_bool emit_template_code(const Emitter emit, const Func f) {
  const Value v = f->value_ref;
  size_t scope = emit_push(emit, v->owner_class, v->owner);
  const m_bool ret = emit_func_def(emit, f->def);
  emit_pop(emit, scope);
  return ret > 0 ? push_func_code(emit, f) : GW_ERROR;
}

ANN static Instr get_prelude(const Emitter emit, const Func f) {
  Instr instr;
  const Type t = actual_type(f->value_ref->type);
  if(isa(t, t_fptr) < 0)
    instr = emit_add_instr(emit, !GET_FLAG(f, builtin) ? FuncUsr : SetCode);
  else {
    emit_add_instr(emit, GWOP_EXCEPT);
    if(f->def->base->tmpl) { // TODO: put in func
      struct dottmpl_ *dt = (struct dottmpl_*)mp_calloc(emit->gwion->mp, dottmpl);
      size_t len = strlen(f->name);
      size_t slen = strlen(f->value_ref->owner->name);
      assert(len > slen);
      size_t sz = len - slen;
      char c[sz + 1];
      memcpy(c, f->name, sz);
      c[sz] = '\0';
      dt->tl = tmpl_tl(emit->env, c);
      dt->name = s_name(insert_symbol(c));
      dt->vt_index = f->def->base->tmpl->base;
      dt->base = f->def;
      dt->owner = f->value_ref->owner;
      dt->owner_class = f->value_ref->owner_class;
      const Instr gtmpl = emit_add_instr(emit, GTmpl);
      gtmpl->m_val = (m_uint)dt;
      gtmpl->m_val2 = strlen(c);
    }
    instr = emit_add_instr(emit, FuncPtr);
  }
  instr->m_val2 = 1;
  return instr;
}

ANN static void emit_args(const Emitter emit, const Func f) {
  const m_uint member = GET_FLAG(f, member) ? SZ_INT : 0;
  if((f->def->stack_depth - member) == SZ_INT) {
    const Instr instr = emit_add_instr(emit, Reg2Mem);
    instr->m_val = member;
  } else {
    const Instr instr = emit_add_instr(emit, Reg2Mem4);
    instr->m_val = member;
    instr->m_val2 = f->def->stack_depth - member;
  }
}

ANN static Instr emit_call(const Emitter emit, const Func f) {
  const Instr memoize = !(emit->memoize && GET_FLAG(f, pure)) ? NULL : emit_add_instr(emit, MemoizeCall);
  const Instr prelude = get_prelude(emit, f);
  prelude->m_val = f->def->stack_depth;
  const m_uint member = GET_FLAG(f, member) ? SZ_INT : 0;
  if(member) {
    const Instr instr = emit_add_instr(emit, Reg2Mem);
    instr->m_val2 = f->def->stack_depth - SZ_INT;
    ++prelude->m_val2;
  }
  if(f->def->stack_depth - member) {
    emit_args(emit, f);
    ++prelude->m_val2;
  }
  if(memoize)
    memoize->m_val = prelude->m_val2 + 1;
  return emit_add_instr(emit, Overflow);
}

ANN m_bool emit_exp_call1(const Emitter emit, const Func f) {
  if(!f->code || (GET_FLAG(f, ref) && !GET_FLAG(f, builtin))) {
    if(GET_FLAG(f, template) && !is_fptr(f->value_ref->type)) {
      if(emit->env->func != f)
        CHECK_BB(emit_template_code(emit, f))
      else {
        const Instr back = (Instr)vector_back(&emit->code->instr);
        back->opcode = ePushStaticCode;
      }
    }
  } else if((f->value_ref->owner_class && is_special(f->value_ref->owner_class) > 0) ||
        !f->value_ref->owner_class || (GET_FLAG(f, template) &&
        isa(f->value_ref->type, t_fptr) < 0))
    push_func_code(emit, f);
  else if(vector_size(&emit->code->instr)){
    const Instr back = (Instr)vector_back(&emit->code->instr);
    if((f_instr)(m_uint)back->opcode == DotFunc || (f_instr)(m_uint)back->opcode == DotStaticFunc)
      back->m_val = f->vt_index;
  }
  if(vector_size(&emit->code->instr) && GET_FLAG(f, member) &&
        is_fptr(f->value_ref->type)) {
    const Instr back = (Instr)vector_back(&emit->code->instr);
    m_bit exec = back->opcode;
    m_uint val = back->m_val;
    m_uint val2 = back->m_val2;
    back->opcode = eReg2Reg;
    back->m_val = SZ_INT;
    const Instr push = emit_add_instr(emit, RegPush);
    push->m_val = SZ_INT;
    const Instr instr = emit_add_instr(emit, (f_instr)(m_uint)exec);
    instr->m_val = val;
    instr->m_val2 = val2;
  }
  const m_uint offset = emit_code_offset(emit);
  regseti(emit, offset);
  const Instr instr = emit_call(emit, f);
  instr->m_val = f->def->base->ret_type->size;
  instr->m_val2 = offset;
  return GW_OK;
}

ANN static void emit_exp_spork_finish(const Emitter emit, const m_uint depth) {
  regpop(emit, depth);
  const Instr spork = emit_add_instr(emit, SporkFunc);
  spork->m_val = depth + SZ_INT;
  spork->m_val2 = -SZ_INT;
}

static inline void stack_alloc(const Emitter emit) {
  emit_local(emit, SZ_INT, 0);
  emit->code->stack_depth += SZ_INT;
}

static inline void stack_alloc_this(const Emitter emit) {
  SET_FLAG(emit->code, member);
  stack_alloc(emit);
}

static m_bool scoped_stmt(const Emitter emit, const Stmt stmt, const m_bool pop) {
  ++emit->env->scope->depth;
  emit_push_scope(emit);
  const m_bool pure = !vector_back(&emit->pure);
  if(!pure)
    emit_add_instr(emit, GcIni);
  CHECK_BB(emit_stmt(emit, stmt, pop))
  if(!pure)
    emit_add_instr(emit, GcEnd);
  emit_pop_scope(emit);
  --emit->env->scope->depth;
  return GW_OK;
}

#define SPORK_FUNC_PREFIX "spork~func:%i"
#define FORK_FUNC_PREFIX "fork~func:%i"
#define SPORK_CODE_PREFIX "spork~code:%i"
#define FORK_CODE_PREFIX  "fork~code:%i"

static void push_spork_code(const Emitter emit, const m_str prefix, const loc_t pos) {
  char c[strlen(SPORK_FUNC_PREFIX) + num_digit(pos->first_line) + 1];
  sprintf(c, prefix, pos->first_line);
  emit_push_code(emit, c);
}

ANN static m_bool spork_func(const Emitter emit, const Exp_Call* exp) {
  if(GET_FLAG(exp->m_func, member))
    SET_FLAG(emit->code, member);
  return emit_exp_call1(emit, exp->m_func);
}

ANN m_bool emit_exp_spork(const Emitter emit, const Exp_Unary* unary) {
  const m_bool is_spork = unary->op == insert_symbol("spork");
  const Func f = !unary->code ? unary->exp->d.exp_call.m_func : NULL;
  if(unary->code) {
    emit_add_instr(emit, RegPushImm);
    push_spork_code(emit, is_spork ? SPORK_CODE_PREFIX : FORK_CODE_PREFIX, unary->code->pos);
    if(!SAFE_FLAG(emit->env->func, member))
      stack_alloc_this(emit);
    CHECK_BB(scoped_stmt(emit, unary->code, 0))
  } else {
    CHECK_BB(prepare_call(emit, &unary->exp->d.exp_call))
    push_spork_code(emit, is_spork ? SPORK_FUNC_PREFIX : FORK_CODE_PREFIX, unary->exp->pos);
    CHECK_BB(spork_func(emit, &unary->exp->d.exp_call))
  }
  const VM_Code code = finalyze(emit);
  const Instr ini = emit_add_instr(emit, unary->op == insert_symbol("spork") ? SporkIni : ForkIni);
  ini->m_val = (m_uint)code;
  ini->m_val2 = is_spork;
  if(!f) {
    if(!is_spork) {
      const Instr push = emit_add_instr(emit, RegPush);
      push->m_val = SZ_INT;
    }
    const Instr spork = emit_add_instr(emit, is_spork ? SporkExp : ForkEnd);
    spork->m_val = emit->code->stack_depth;
  } else {
    if(GET_FLAG(f, member) && is_fptr(f->value_ref->type)) {
      const m_uint depth = f->def->stack_depth;
      regpop(emit, depth -SZ_INT);
      const Instr spork = emit_add_instr(emit, SporkMemberFptr);
      spork->m_val = depth + SZ_INT;
      spork->m_val2 = -SZ_INT*2;
    } else
      emit_exp_spork_finish(emit, f->def->stack_depth);
    const Instr end = emit_add_instr(emit, is_spork ? SporkEnd : ForkEnd);
    end->m_val2 = f->def->base->ret_type->size;
  }
  return GW_OK;
}

ANN static m_bool emit_exp_unary(const Emitter emit, const Exp_Unary* unary) {
  struct Op_Import opi = { .op=unary->op, .data=(uintptr_t)unary };
  if(unary->op != insert_symbol("spork") && unary->op != insert_symbol("fork") && unary->exp) {
    CHECK_BB(emit_exp(emit, unary->exp, 1))
    opi.rhs = unary->exp->type;
  }
  return op_emit(emit, &opi);
}

ANN static m_bool emit_implicit_cast(const Emitter emit,
    const restrict Exp  from, const restrict Type to) {
  const struct Implicit imp = { from, to, from->pos };
  struct Op_Import opi = { .op=insert_symbol("@implicit"), .lhs=from->type, .rhs=to, .data=(m_uint)&imp };
  return op_emit(emit, &opi);
}

ANN static Instr emit_flow(const Emitter emit, const Type type,
    const f_instr f1, const f_instr f2) {
  if(isa(type, t_float) > 0 || isa(type, t_dur) > 0 || isa(type, t_time) > 0)
    return emit_add_instr(emit, f2);
  return emit_add_instr(emit, f1);
}

ANN static m_bool emit_exp_if(const Emitter emit, const Exp_If* exp_if) {
  CHECK_BB(emit_exp(emit, exp_if->cond, 0))
  const Instr op = emit_flow(emit, exp_if->cond->type, BranchEqInt, BranchEqFloat);
  CHECK_BB(emit_exp(emit, exp_if->if_exp ?: exp_if->cond, 0))
  const Instr op2 = emit_add_instr(emit, Goto);
  op->m_val = emit_code_size(emit);
  const m_bool ret = emit_exp(emit, exp_if->else_exp, 0);
  op2->m_val = emit_code_size(emit);
  return ret;
}

ANN static m_bool emit_exp_lambda(const Emitter emit, const Exp_Lambda * lambda) {
  if(lambda->def) {
    const m_uint scope = !lambda->owner ?
      emit->env->scope->depth : emit_push_type(emit, lambda->owner);
    CHECK_BB(emit_func_def(emit, lambda->def))
    if(GET_FLAG(lambda->def, member))
      emit_add_instr(emit, RegPushMem);
    regpushi(emit, (m_uint)lambda->def->base->func->code);
    if(lambda->owner)
      emit_pop(emit, scope);
  } else
    emit_add_instr(emit, RegPushImm);
  return GW_OK;
}

ANN static m_bool emit_exp_typeof(const Emitter emit, const Exp_Typeof *exp) {
  regpushi(emit, (m_uint)exp->exp->type);
  return GW_OK;
}

DECL_EXP_FUNC(emit)

ANN2(1) static m_bool emit_exp(const Emitter emit, Exp exp, const m_bool ref) {
  do {
    CHECK_BB(exp_func[exp->exp_type](emit, &exp->d))
    if(exp->cast_to)
      CHECK_BB(emit_implicit_cast(emit, exp, exp->cast_to))
    if(ref && isa(exp->type, t_object) > 0 && isa(exp->type, t_shred) < 0 ) { // beware fork
      const Instr instr = emit_add_instr(emit, RegAddRef);
      instr->m_val = exp->emit_var;
    }
    if(emit->env->func && isa(exp->type, t_lambda) < 0 && isa(exp->type, t_function) > 0 &&
        !GET_FLAG(exp->type->e->d.func->value_ref->d.func_ref, pure))
      UNSET_FLAG(emit->env->func, pure);
  } while((exp = exp->next));
  return GW_OK;
}

ANN static m_bool emit_stmt_if(const Emitter emit, const Stmt_If stmt) {
  emit_push_scope(emit);
  CHECK_BB(emit_exp(emit, stmt->cond, 0))
  DECL_OB(const Instr, op, = emit_flow(emit, isa(stmt->cond->type, t_object) > 0 ?
      t_int : stmt->cond->type, BranchEqInt, BranchEqFloat))
  CHECK_BB(scoped_stmt(emit, stmt->if_body, 1))
  const Instr op2 = emit_add_instr(emit, Goto);
  op->m_val = emit_code_size(emit);
  if(stmt->else_body)
    CHECK_BB(scoped_stmt(emit, stmt->else_body, 1))
  op2->m_val = emit_code_size(emit);
  emit_pop_scope(emit);
  return GW_OK;
}

ANN static m_bool emit_stmt_code(const Emitter emit, const Stmt_Code stmt) {
  ++emit->env->scope->depth;
  const m_bool ret = stmt->stmt_list ? emit_stmt_list(emit, stmt->stmt_list) : 1;
  --emit->env->scope->depth;
  return ret;
}

ANN static m_bool optimize_taill_call(const Emitter emit, const Exp_Call* e) {
  if(e->args) {
    CHECK_BB(emit_exp(emit, e->args, 0))
    regpop(emit, SZ_INT);
    emit_args(emit, e->m_func);
  }
  emit_add_instr(emit, Goto);
  return GW_OK;
}


ANN static m_bool emit_stmt_return(const Emitter emit, const Stmt_Exp stmt) {
  if(stmt->val) {
    if(stmt->val->exp_type == ae_exp_call && emit->env->func == stmt->val->d.exp_call.m_func)
      return optimize_taill_call(emit, &stmt->val->d.exp_call);
    CHECK_BB(emit_exp(emit, stmt->val, 0))
    if(isa(stmt->val->type, t_object) > 0 && isa(stmt->val->type , t_shred) < 0) // beware shred
      emit_add_instr(emit, RegAddRef);
  }
  vector_add(&emit->code->stack_return, (vtype)emit_add_instr(emit, Goto));
  return GW_OK;
}

ANN static inline m_bool emit_stmt_continue(const Emitter emit, const Stmt stmt NUSED) {
  vector_add(&emit->code->stack_cont, (vtype)emit_add_instr(emit, Goto));
  return GW_OK;
}

ANN static inline m_bool emit_stmt_break(const Emitter emit, const Stmt stmt NUSED) {
  vector_add(&emit->code->stack_break, (vtype)emit_add_instr(emit, Goto));
  return GW_OK;
}

ANN static inline void emit_push_stack(const Emitter emit) {
  emit_push_scope(emit);
  vector_add(&emit->code->stack_cont, (vtype)NULL);
  vector_add(&emit->code->stack_break, (vtype)NULL);
}

ANN static void pop_vector(Vector v, const  m_uint pc) {
  Instr instr;
  while((instr = (Instr)vector_pop(v)))
    instr->m_val = pc;
}

ANN static void emit_pop_stack(const Emitter emit, const m_uint index) {
  pop_vector(&emit->code->stack_cont, index);
  pop_vector(&emit->code->stack_break, emit_code_size(emit));
  emit_pop_scope(emit);
}

ANN static m_uint get_decl_size(Var_Decl_List a) {
  m_uint size = 0;
  do //if(GET_FLAG(a->self->value, used))
    size += a->self->value->type->size;
  while((a = a->next));
  return size;
}

ANN static m_uint pop_exp_size(const Emitter emit, Exp e) {
  m_uint size = 0;
  do {
    if(e->exp_type == ae_exp_primary &&
        e->d.exp_primary.primary_type == ae_primary_hack) {
      size += pop_exp_size(emit, e->d.exp_primary.d.exp);
      continue;
    }
    size += (e->exp_type == ae_exp_decl ?
        get_decl_size(e->d.exp_decl.list) : e->type->size);
  } while((e = e->next));
  return size;
}

ANN static inline void pop_exp(const Emitter emit, Exp e) {
  const m_uint size = pop_exp_size(emit, e);
  if(size)
   regpop(emit, size);
}

ANN static inline m_bool emit_exp_pop_next(const Emitter emit, Exp e) {
  CHECK_BB(emit_exp(emit, e, 0))
  if(e->next)
    pop_exp(emit, e->next);
  return GW_OK;
}

ANN static Instr _flow(const Emitter emit, const Exp e, const m_bool b) {
  CHECK_BO(emit_exp_pop_next(emit, e))
  const f_instr instr_i = b ? BranchEqInt : BranchNeqInt;
  const f_instr instr_f = b ? BranchEqFloat : BranchNeqFloat;
  return emit_flow(emit, e->type, instr_i, instr_f);
}

ANN static m_bool emit_stmt_flow(const Emitter emit, const Stmt_Flow stmt) {
  const m_uint index = emit_code_size(emit);
  Instr op = NULL;
  emit_push_stack(emit);
  if(!stmt->is_do)
    op = _flow(emit, stmt->cond, stmt_self(stmt)->stmt_type == ae_stmt_while);
  CHECK_BB(scoped_stmt(emit, stmt->body, 1))
  if(stmt->is_do) {
    CHECK_OB((op = _flow(emit, stmt->cond, stmt_self(stmt)->stmt_type != ae_stmt_while)))
    op->m_val = index;
  } else {
    const Instr goto_ = emit_add_instr(emit, Goto);
    goto_->m_val = index;
    op->m_val = emit_code_size(emit);
  }
  emit_pop_stack(emit, index);
  return GW_OK;
}

ANN static m_bool emit_stmt_for(const Emitter emit, const Stmt_For stmt) {
  emit_push_stack(emit);
  CHECK_BB(emit_stmt(emit, stmt->c1, 1))
  const m_uint index = emit_code_size(emit);
  if(stmt->c2->stmt_type == ae_stmt_exp)
    emit_exp_pop_next(emit, stmt->c2->d.stmt_exp.val);
  else
    CHECK_BB(emit_stmt(emit, stmt->c2, 0))
  const Instr op = emit_flow(emit, stmt->c2->d.stmt_exp.val->type,
    BranchEqInt, BranchEqFloat);
  CHECK_BB(scoped_stmt(emit, stmt->body, 1))
  const m_uint action_index = emit_code_size(emit);
  if(stmt->c3) {
    CHECK_BB(emit_exp(emit, stmt->c3, 0))
    pop_exp(emit, stmt->c3);
  }
  const Instr _goto = emit_add_instr(emit, Goto);
  _goto->m_val = index;
  op->m_val = emit_code_size(emit);
  emit_pop_stack(emit, action_index);
  return GW_OK;
}

ANN static Instr emit_stmt_autoptr_init(const Emitter emit, const Type type) {
  const Instr new_obj = emit_add_instr(emit, ObjectInstantiate);
  new_obj->m_val2 = (m_uint)type;
  regpop(emit, SZ_INT);
  return emit_add_instr(emit, Reg2Mem);
}

ANN static m_bool emit_stmt_auto(const Emitter emit, const Stmt_Auto stmt) {
  CHECK_BB(emit_exp(emit, stmt->exp, 0))
  const Instr s1 = emit_add_instr(emit, MemSetImm);
  emit_push_stack(emit);
  Instr cpy = stmt->is_ptr ? emit_stmt_autoptr_init(emit, stmt->v->type) : NULL;
  const m_uint ini_pc  = emit_code_size(emit);
  emit_add_instr(emit, GWOP_EXCEPT);
  const Instr loop = emit_add_instr(emit, stmt->is_ptr ? AutoLoopPtr : AutoLoop);
  const Instr end = emit_add_instr(emit, BranchEqInt);
  const m_uint offset = emit_local(emit, SZ_INT + stmt->v->type->size, 0);
  stmt->v->offset = offset + SZ_INT;
  CHECK_BB(emit_stmt(emit, stmt->body, 1))
  const m_uint end_pc = emit_code_size(emit);
  if(stmt->is_ptr) {
    loop->m_val2 = (m_uint)stmt->v->type;
    cpy->m_val = stmt->v->offset;
  }
  const Instr tgt = emit_add_instr(emit, Goto);
  end->m_val = emit_code_size(emit);
  tgt->m_val = ini_pc;
  s1->m_val = loop->m_val = offset;
  regpop(emit, SZ_INT);
  emit_pop_stack(emit, end_pc);
  return GW_OK;
}

ANN static m_bool emit_stmt_loop(const Emitter emit, const Stmt_Loop stmt) {
  emit_push_stack(emit);
  CHECK_BB(emit_exp_pop_next(emit, stmt->cond))
  const m_uint index = emit_code_size(emit);
  const Instr cpy = emit_add_instr(emit, Reg2RegAddr);
  cpy->m_val2 = -SZ_INT;
  emit_add_instr(emit, int_post_dec);
  const Instr op = emit_add_instr(emit, BranchEqInt);
  CHECK_BB(scoped_stmt(emit, stmt->body, 1))
  const Instr _goto = emit_add_instr(emit, Goto);
  _goto->m_val = index;
  op->m_val = emit_code_size(emit);
  emit_pop_stack(emit, index);
  regpop(emit, SZ_INT);
  return GW_OK;
}

ANN static m_bool emit_stmt_jump(const Emitter emit, const Stmt_Jump stmt) {
  if(!stmt->is_label)
    stmt->data.instr = emit_add_instr(emit, Goto);
  else {
    if(!strcmp(s_name(stmt->name), "default") && switch_inside(emit->env, stmt_self(stmt)->pos) > 0)
      return switch_default(emit->env, emit_code_size(emit), stmt_self(stmt)->pos);
    if(!stmt->data.v.ptr)
      ERR_B(stmt_self(stmt)->pos, _("illegal case"))
    const m_uint size = vector_size(&stmt->data.v);
    if(!size)
      ERR_B(stmt_self(stmt)->pos, _("label '%s' defined but not used."), s_name(stmt->name))
    LOOP_OPTIM
    for(m_uint i = size + 1; --i;) {
      const Stmt_Jump label = (Stmt_Jump)vector_at(&stmt->data.v, i - 1);
      if(!label->data.instr)
        ERR_B(stmt_self(label)->pos, _("you are trying to use a upper label."))
      label->data.instr->m_val = emit_code_size(emit);
    }
  }
  return GW_OK;
}

ANN static m_bool emit_switch_instr(const Emitter emit, Instr *instr) {
  const m_uint dyn = switch_dyn(emit->env);
  if(dyn) {
    Exp e;
    while((e = switch_expget(emit->env)))
      CHECK_BB(emit_exp(emit, e, 0))
    instr[0] = emit_add_instr(emit, RegPop);
    instr[1] = emit_add_instr(emit, SwitchIni);
    instr[2] = emit_add_instr(emit, RegSetImm);
  } else
    regseti(emit, (m_uint)switch_map(emit->env));
  return GW_OK;
}

ANN static Map emit_switch_map(MemPool p, const Map map) {
  const Map m = new_map(p);
  for(m_uint i = map_size(map) + 1; --i;)
    map_set(m, VKEY(map, i-1), VVAL(map, i -1));
  return m;
}

ANN static m_bool emit_stmt_switch(const Emitter emit, const Stmt_Switch stmt) {
  switch_get(emit->env, stmt);
  Instr push[3] = { NULL, NULL, NULL};
  CHECK_BB(emit_exp(emit, stmt->val, 0))
  CHECK_BB(emit_switch_instr(emit, push))
  vector_add(&emit->code->stack_break, (vtype)NULL);
  const Instr instr = emit_add_instr(emit, SwitchBranch);
  instr->m_val2 = (m_uint)switch_map(emit->env);
  CHECK_BB(emit_stmt(emit, stmt->stmt, 1))
  instr->m_val = switch_idx(emit->env) ?: emit_code_size(emit);
  if(push[0]) {
    push[2]->m_val = push[1]->m_val2 = (m_uint) emit_switch_map(emit->gwion->mp, (Map)instr->m_val2);
    Vector v = (Vector)(push[1]->m_val = (m_uint)switch_vec(emit->env));
    push[0]->m_val = vector_size(v) * SZ_INT;
  }
  CHECK_BB(switch_end(emit->env, stmt->stmt->pos))
  pop_vector(&emit->code->stack_break, emit_code_size(emit));
  return GW_OK;
}

ANN m_bool value_value(const Value v, m_int *value) {
  if((!GET_FLAG(v, builtin) && !GET_FLAG(v, enum)) || GET_FLAG(v, member))
     return 0;
  *value = v->d.ptr ? *(m_int*)v->d.ptr : v->owner->info->class_data[v->offset];
  return GW_OK;
}

ANN Value case_value(const Exp exp) {
  if(exp->exp_type == ae_exp_primary) {
    const Exp_Primary* prim = &exp->d.exp_primary;
    if(prim->primary_type == ae_primary_num)
      return NULL;
    return prim->value;
  }
  const Exp_Dot* dot = &exp->d.exp_dot;
  return find_value(actual_type(dot->t_base), dot->xid);
}

ANN static m_bool prim_value(const Exp exp, m_int *value) {
  *value = (m_int)exp->d.exp_primary.d.num;
  return GW_OK;
}

ANN static m_bool emit_stmt_case(const Emitter emit, const Stmt_Exp stmt) {
  CHECK_BB(switch_inside(emit->env, stmt_self(stmt)->pos))
  m_int value = 0;
  const Value v = case_value(stmt->val);
  if((!v && prim_value(stmt->val, &value)) || value_value(v, &value)) {
    CHECK_BB(switch_dup(emit->env, value, stmt->val->pos))
    switch_dynpc(emit->env, value, emit_code_size(emit));
  } else
    switch_pc(emit->env, emit_code_size(emit));
  return GW_OK;
}

ANN static m_bool emit_type_def(const Emitter emit, const Type_Def tdef) {
  return tdef->type->e->def ? emit_class_def(emit, tdef->type->e->def) : 1;
}

ANN static m_bool emit_enum_def(const Emitter emit, const Enum_Def edef) {
  LOOP_OPTIM
  for(m_uint i = 0; i < vector_size(&edef->values); ++i) {
    const Value v = (Value)vector_at(&edef->values, i);
    if(!emit->env->class_def) {
      ALLOC_PTR(emit->gwion->mp, addr, m_uint, i);
      v->offset = emit_local(emit, SZ_INT, 0);
      v->d.ptr = addr;
    } else
      *(m_bit*)(emit->env->class_def->nspc->info->class_data + v->offset) = i;
  }
  return GW_OK;
}

ANN void emit_union_offset(Decl_List l, const m_uint o) {
  do {
    Var_Decl_List v = l->self->d.exp_decl.list;
    do v->self->value->offset = o;
    while((v = v->next));
  } while((l = l->next));
}

ANN static inline void union_allocdata(MemPool mp, const Union_Def udef) {
  const Nspc nspc = (udef->xid ? udef->value->type : udef->type)->nspc;
  nspc_allocdata(mp, nspc);
  nspc->info->offset = udef->s;
}

ANN static m_bool emit_union_def(const Emitter emit, const Union_Def udef) {
  if(tmpl_base(udef->tmpl))
    return GW_OK;
  Decl_List l = udef->l;
  m_uint scope = emit->env->scope->depth;
  const m_bool global = GET_FLAG(udef, global);
  if(udef->xid) {
    union_allocdata(emit->gwion->mp, udef);
    Type_Decl *type_decl = new_type_decl(emit->gwion->mp,
        new_id_list(emit->gwion->mp, udef->xid, udef->pos));
    type_decl->flag = udef->flag;
    const Var_Decl var_decl = new_var_decl(emit->gwion->mp, udef->xid, NULL, loc_cpy(emit->gwion->mp, udef->pos));
    const Var_Decl_List var_decl_list = new_var_decl_list(emit->gwion->mp, var_decl, NULL);
    const Exp exp = new_exp_decl(emit->gwion->mp, type_decl, var_decl_list);
    exp->d.exp_decl.type = udef->value->type;
    var_decl->value = udef->value;
    const m_bool ret = emit_exp_decl(emit, &exp->d.exp_decl);
    free_exp(emit->gwion->mp, exp);
    CHECK_BB(ret)
    if(global) {
      const M_Object o = new_object(emit->gwion->mp, NULL, udef->value->type);
      udef->value->d.ptr = (m_uint*)o;
      SET_FLAG(udef->value, builtin);
      SET_FLAG(udef->value, global);
    }
    scope = emit_push_type(emit, udef->value->type);
  } else if(udef->type_xid) {
    union_allocdata(emit->gwion->mp, udef);
    scope = emit_push_type(emit, udef->type);
  } else if(emit->env->class_def) {
    if(!GET_FLAG(l->self->d.exp_decl.list->self->value, member))
      udef->o = emit_local(emit, udef->s, 0);
  } else if(global) {
    void* ptr = (void*)xcalloc(1, udef->s);
    l = udef->l;
    do {
      Var_Decl_List list = l->self->d.exp_decl.list;
      list->self->value->d.ptr = ptr;
      SET_FLAG(list->self->value, union);
    } while((l = l->next));
    SET_FLAG(udef->l->self->d.exp_decl.list->self->value, enum);
  }
  if(udef->xid)
    regpop(emit, !GET_FLAG(udef, static) ? SZ_INT : SZ_INT*2);
  emit_union_offset(udef->l, udef->o);
  if(udef->xid || udef->type_xid || global)
    emit_pop(emit, scope);
  SET_FLAG(udef->xid ? udef->value->type : udef->type, emit);
  return GW_OK;
}

ANN static m_bool emit_stmt_exp(const Emitter emit, const struct Stmt_Exp_* exp) {
  return exp->val ? emit_exp(emit, exp->val, 0) : 1;
}

static const _exp_func stmt_func[] = {
  (_exp_func)emit_stmt_exp,   (_exp_func)emit_stmt_flow,     (_exp_func)emit_stmt_flow,
  (_exp_func)emit_stmt_for,   (_exp_func)emit_stmt_auto,     (_exp_func)emit_stmt_loop,
  (_exp_func)emit_stmt_if,    (_exp_func)emit_stmt_code,     (_exp_func)emit_stmt_switch,
  (_exp_func)emit_stmt_break, (_exp_func)emit_stmt_continue, (_exp_func)emit_stmt_return,
  (_exp_func)emit_stmt_case,  (_exp_func)emit_stmt_jump,
};

ANN static m_bool emit_stmt(const Emitter emit, const Stmt stmt, const m_bool pop) {
  CHECK_BB(stmt_func[stmt->stmt_type](emit, &stmt->d))
  if(pop && stmt->stmt_type == ae_stmt_exp && stmt->d.stmt_exp.val)
    pop_exp(emit, stmt->d.stmt_exp.val);
  return GW_OK;
}

ANN static m_bool emit_stmt_list(const Emitter emit, Stmt_List l) {
  do CHECK_BB(emit_stmt(emit, l->stmt, 1))
  while((l = l->next));
  return GW_OK;
}

ANN static m_bool emit_dot_static_import_data(const Emitter emit, const Value v, const uint emit_addr) {
  if(v->d.ptr && GET_FLAG(v, builtin)) {
    if(GET_FLAG(v, enum))
      regpushi(emit, (m_uint)v->d.ptr);
    else {
      const m_uint size = v->type->size;
      const Instr instr = emit_kind(emit, size, emit_addr, regpushimm);
      instr->m_val = (isa(v->type, t_object) > 0 ?
        (m_uint)&v->d.ptr : (m_uint)v->d.ptr);
      instr->m_val2 = size;
    }
    return GW_OK;
  }
  return emit_dot_static_data(emit, v, emit_addr);
}

ANN static m_bool emit_complex_member(const Emitter emit, const Exp_Dot* member) {
  const Exp base = member->base;
  base->emit_var = 1;
  CHECK_BB(emit_exp(emit, base, 0))
  const m_bool is_complex = !strcmp((isa(base->type, t_complex) > 0  ? "re" : "phase") ,
        s_name(member->xid));
  if(is_complex && exp_self(member)->emit_var)
    return GW_OK;
  const Instr instr = emit_add_instr(emit, is_complex ? ComplexReal : ComplexImag);
  instr->m_val = exp_self(member)->emit_var;
  return GW_OK;
}

ANN static m_bool emit_VecMember(const Emitter emit, const Exp_Dot* member) {
  member->base->emit_var = 1;
  CHECK_BB(emit_exp(emit, member->base, 0))
  const Value v = find_value(member->base->type, member->xid);
  if(GET_FLAG(v, func)) {
    regpushi(emit, (m_uint)v->d.func_ref->code);
    return GW_OK;
  }
  if(!v->offset && exp_self(member)->emit_var)
    return GW_OK;
  const Instr instr = emit_add_instr(emit, VecMember);
  instr->m_val2 = v->offset;
  instr->m_val = exp_self(member)->emit_var;
  return GW_OK;
}

ANN static m_bool emit_vararg_start(const Emitter emit, const m_uint offset) {
  const Instr instr = emit_add_instr(emit, VarargTop);
  instr->m_val = offset;
  instr->m_val2 = emit_code_size(emit);
  vector_set(&emit->variadic, vector_size(&emit->variadic) -1, (vtype)instr);
  return GW_OK;
}
ANN static inline Instr get_variadic(const Emitter emit) {
  return (Instr)vector_back(&emit->variadic);
}

ANN static void emit_vararg_end(const Emitter emit, const m_uint offset) {
  const Instr instr = emit_add_instr(emit, VarargEnd),
    variadic = get_variadic(emit);
  instr->m_val = offset;
  instr->m_val2 = variadic->m_val2;
  variadic->m_val2 = emit_code_size(emit);
  SET_FLAG(emit->env->func, empty);// mark vararg func as complete
}

ANN static m_bool emit_vararg(const Emitter emit, const Exp_Dot* member) {
  m_uint offset = emit->env->class_def ? SZ_INT : 0;
  Arg_List l = emit->env->func->def->base->args;
  const m_str str = s_name(member->xid);
  while(l) {
    offset += l->type->size;
    l = l->next;
  }
  if(!strcmp(str, "start")) {
    if(get_variadic(emit))
      ERR_B(exp_self(member)->pos, _("vararg.start already used. this is an error"))
    emit_vararg_start(emit, offset);
    return GW_OK;
  }
  if(!strcmp(str, "end")) {
    if(!get_variadic(emit))
      ERR_B(exp_self(member)->pos, _("vararg.start not used before vararg.end. this is an error"))
    emit_vararg_end(emit, offset);
    return GW_OK;
  }
  if(!get_variadic(emit))
      ERR_B(exp_self(member)->pos, _("vararg.%s used before vararg.start. this is an error"), s_name(member->xid))
  if(GET_FLAG(emit->env->func, empty))
    ERR_B(exp_self(member)->pos, _("vararg.%s used after vararg.end. this is an error"), s_name(member->xid))
  const Instr instr = emit_add_instr(emit, VarargMember);
  instr->m_val = offset;
  instr->m_val2 = exp_self(member)->type->size;
  return GW_OK;
}

ANN static m_bool emit_exp_dot_special(const Emitter emit, const Exp_Dot* member) {
  const Type t = member->t_base;
  if(isa(t, t_complex) > 0 || isa(t, t_polar) > 0)
    return emit_complex_member(emit, member);
  else if(isa(t, t_vec3) > 0 || isa(t, t_vec4) > 0)
    return emit_VecMember(emit, member);
  return emit_vararg(emit, member);
}

ANN static m_bool emit_member_func(const Emitter emit, const Exp_Dot* member, const Func func) {
  if(isa(member->t_base, t_class) > 0 || GET_FLAG(member->base->type, force)) {
    const Instr func_i = emit_add_instr(emit, func->code ? RegPushImm : PushStaticCode);
    func_i->m_val = (m_uint)(func->code ?: (VM_Code)func);
    return GW_OK;
  }
  if(func->def->base->tmpl)
    emit_add_instr(emit, DotTmplVal);
  else {
    const Instr instr = emit_add_instr(emit, GET_FLAG(func, member) ? DotFunc : DotStaticFunc);
    instr->m_val = exp_self(member)->type->e->d.func->vt_index;
  }
  return GW_OK;
}

ANN static inline m_bool emit_member(const Emitter emit, const Value v, const uint emit_addr) {
  const m_uint size = v->type->size;
  const Instr instr = emit_kind(emit, size, emit_addr, dotmember);
  instr->m_val = v->offset;
  instr->m_val2 = size;
  return GW_OK;
}

ANN static m_bool emit_exp_dot(const Emitter emit, const Exp_Dot* member) {
  if(member->xid == insert_symbol("vararg")) { // TODO prohibit this?
    emit_add_instr(emit, RegPushImm);
    return GW_OK;
  }
  if(is_special(member->t_base) > 0)
    return emit_exp_dot_special(emit, member);
  const Value value = find_value(actual_type(member->t_base), member->xid);
  if(isa(member->t_base, t_class) < 0 && (GET_FLAG(value, member) ||
(isa(exp_self(member)->type, t_function) > 0 && isa(exp_self(member)->type, t_fptr) < 0))
) {
    CHECK_BB(emit_exp(emit, member->base, 0))
    emit_add_instr(emit, GWOP_EXCEPT);
  }
  if(isa(exp_self(member)->type, t_function) > 0 && isa(exp_self(member)->type, t_fptr) < 0)
    return emit_member_func(emit, member, value->d.func_ref);
  return (GET_FLAG(value, member) ? emit_member : emit_dot_static_import_data) (emit, value, exp_self(member)->emit_var);
}

ANN static inline void emit_func_def_global(const Emitter emit, const Value value) {
  const Instr set_mem = emit_add_instr(emit, MemSetImm);
  set_mem->m_val = value->offset;
  set_mem->m_val2 = (m_uint)value->d.func_ref->code;
}

ANN static void emit_func_def_init(const Emitter emit, const Func func) {
  const Type t = emit->env->class_def;
  char c[(t ? strlen(t->name) + 1 : 0) + strlen(func->name) + 6];
  sprintf(c, "%s%s%s(...)", t ? t->name : "", t ? "." : "", func->name);
  emit_push_code(emit, c);
}

ANN static void emit_func_def_args(const Emitter emit, Arg_List a) {
  do {
    const Value  value = a->var_decl->value;
    const m_uint size = value->type->size;
    const m_bool obj = isa(value->type, t_object) > 0;
    emit->code->stack_depth += size;
    value->offset = emit_local(emit, size, obj);
  } while((a = a->next));
}

ANN static void emit_func_def_ensure(const Emitter emit, const Func_Def fdef) {
  const m_uint size = fdef->base->ret_type->size;
  if(size) {
    const Instr instr = emit_kind(emit, size, 0, regpushimm);
    instr->m_val2 = size;
  }
  vector_add(&emit->code->stack_return, (vtype)emit_add_instr(emit, Goto));
}

ANN static void emit_func_def_return(const Emitter emit) {
  const m_uint val = emit_code_size(emit);
  LOOP_OPTIM
  for(m_uint i = vector_size(&emit->code->stack_return) + 1; --i; ) {
    const Instr instr = (Instr)vector_at(&emit->code->stack_return, i-1);
    instr->m_val = val;
  }
  vector_clear(&emit->code->stack_return);
  emit_pop_scope(emit);
  if(emit->memoize && GET_FLAG(emit->env->func, pure))
    emit_add_instr(emit, MemoizeStore);
  emit_add_instr(emit, FuncReturn);
}

ANN static void emit_func_def_code(const Emitter emit, const Func func) {
  if(GET_FLAG(func->def, dtor)) {
    Instr instr = (Instr)vector_back(&emit->code->instr);
    instr->opcode = eOP_MAX;
    instr->execute = DTOR_EOC;
    instr->m_val = (m_uint)emit->gwion->mp;
  }
  func->code = emit_code(emit);
  if(GET_FLAG(func->def, dtor)) {
    emit->env->class_def->nspc->dtor = func->code;
    ADD_REF(func->code)
  }
}

ANN static m_bool _fdef_body(const Emitter emit, const Func_Def fdef) {
  if(fdef->base->args)
    emit_func_def_args(emit, fdef->base->args);
  if(GET_FLAG(fdef, variadic))
    stack_alloc(emit);
  if(fdef->d.code)
    CHECK_BB(emit_stmt_code(emit, &fdef->d.code->d.stmt_code))
  emit_func_def_ensure(emit, fdef);
  return GW_OK;
}

ANN static m_bool emit_func_def_body(const Emitter emit, const Func_Def fdef) {
  vector_add(&emit->variadic, 0);
  CHECK_BB(_fdef_body(emit, fdef))
  if(GET_FLAG(fdef, variadic)) {
    if(!get_variadic(emit))
      ERR_B(fdef->pos, _("invalid variadic use"))
    if(!GET_FLAG(fdef->base->func, empty))
      ERR_B(fdef->pos, _("invalid variadic use"))
  }
  return GW_OK;
}

ANN static m_bool tmpl_rettype(const Emitter emit, const Func_Def fdef) {
  CHECK_BB(template_push_types(emit->env, fdef->base->tmpl))
  const m_bool ret = emit_cdef(emit, fdef->base->ret_type->e->def);
  emit_pop_type(emit);
  return ret;
}

ANN static m_bool emit_func_def(const Emitter emit, const Func_Def fdef) {
  const Func func = get_func(emit->env, fdef);
  const Func former = emit->env->func;
  if(tmpl_base(fdef->base->tmpl))
    return GW_OK;
  if(GET_FLAG(fdef->base->ret_type, template) && !GET_FLAG(fdef->base->ret_type, emit))
    CHECK_BB(tmpl_rettype(emit, fdef))
  if(SAFE_FLAG(emit->env->class_def, builtin) && GET_FLAG(emit->env->class_def, template))
    return GW_OK;
  if(!emit->env->class_def && !GET_FLAG(fdef, global) && !fdef->base->tmpl && !emit->env->scope->depth)
    func->value_ref->offset = emit_local(emit, SZ_INT, 0);
  emit_func_def_init(emit, func);
  if(GET_FLAG(func, member))
    stack_alloc_this(emit);
  emit->env->func = func;
  emit_push_scope(emit);
  if(fdef->base->tmpl)
    CHECK_BB(template_push_types(emit->env, fdef->base->tmpl))
  CHECK_BB(emit_func_def_body(emit, fdef))
  emit_func_def_return(emit);
  emit_func_def_code(emit, func);
  if(fdef->base->tmpl)
    emit_pop_type(emit);
  emit_pop_code(emit);
  emit->env->func = former;
  if(!emit->env->class_def && !GET_FLAG(fdef, global) && !fdef->base->tmpl)
    emit_func_def_global(emit, func->value_ref);
  if(emit->memoize && GET_FLAG(func, pure))
    func->code->memoize = memoize_ini(emit, func,
      kindof(func->def->base->ret_type->size, !func->def->base->ret_type->size));
  return GW_OK;
}

#define emit_fptr_def dummy_func
DECL_SECTION_FUNC(emit)

ANN Code* emit_class_code(const Emitter emit, const m_str name) {
  const m_uint len = strlen(name) + 7;
  char c[len];
  snprintf(c, len, "class %s", name);
  emit_push_code(emit, c);
  stack_alloc_this(emit);
  return emit->code;
}

ANN inline void emit_class_finish(const Emitter emit, const Nspc nspc) {
  emit_add_instr(emit, FuncReturn);
  nspc->pre_ctor = emit_code(emit);
  SET_FLAG(nspc->pre_ctor, ctor);
}

ANN static m_bool emit_parent(const Emitter emit, const Class_Def cdef) {
  const Type parent = cdef->base.type->e->parent;
  const Type base = parent->e->d.base_type;
  if(base && !GET_FLAG(base, emit))
    CHECK_BB(emit_cdef(emit, base->e->def))
  return !GET_FLAG(parent, emit) ? emit_cdef(emit, parent->e->def) : GW_OK;
}

ANN static inline m_bool emit_cdef(const Emitter emit, const Class_Def cdef) {
  return scanx_cdef(emit->env, emit, cdef,
      (_exp_func)emit_class_def, (_exp_func)emit_union_def);
}

ANN static m_bool emit_class_def(const Emitter emit, const Class_Def cdef) {
  if(tmpl_base(cdef->base.tmpl))
    return GW_OK;
  const Type type = cdef->base.type;
  const Nspc nspc = type->nspc;
  if(cdef->base.ext && cdef->base.ext->types)
    CHECK_BB(scanx_ext(emit->env, cdef, emit_parent, emit))
  SET_FLAG(type, emit);
  nspc_allocdata(emit->gwion->mp, nspc);
  emit_class_code(emit, type->name);
  if(cdef->base.ext && cdef->base.ext->array)
    CHECK_BB(emit_array_extend(emit, type->e->parent, cdef->base.ext->array->exp))
  if(cdef->body)
    CHECK_BB(scanx_body(emit->env, cdef, (_exp_func)emit_section, emit))
  emit_class_finish(emit, nspc);
  emit_pop_code(emit);
  SET_FLAG(type, emit);
  return GW_OK;
}

ANN static void emit_free_code(const Emitter emit, Code* code) {
  if(vector_size(&code->instr))
    free_code_instr(&code->instr, emit->gwion);
  free_code(emit->gwion->mp, code);
}

ANN static VM_Code emit_free_stack(const Emitter emit) {
  LOOP_OPTIM
  for(m_uint i = vector_size(&emit->stack) + 1; --i;)
    emit_free_code(emit, (Code*)vector_at(&emit->stack, i - 1));
  vector_clear(&emit->stack);
  vector_clear(&emit->pure);
  emit_free_code(emit, emit->code);
  return NULL;
}

ANN static inline m_bool emit_ast_inner(const Emitter emit, Ast ast) {
  do CHECK_BB(emit_section(emit, ast->section))
  while((ast = ast->next));
  return GW_OK;
}

ANN VM_Code emit_ast(const Emitter emit, Ast ast) {
  emit->code = new_code(emit, emit->env->name);
  emit_push_scope(emit);
  const m_bool ret = emit_ast_inner(emit, ast);
  emit_pop_scope(emit);
  return ret > 0 ? finalyze(emit) : emit_free_stack(emit);
}
