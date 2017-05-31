#include "dl.h"
#include "type.h"

void dl_return_push(const DL_Return retval, VM_Shred shred, int kind) {
  if(kind == Kindof_Int) {
    *(m_uint*)shred->reg = retval.d.v_uint;
    PUSH_REG(shred, SZ_INT);
  } else if(kind == Kindof_Float) {
    *(m_float*)shred->reg = retval.d.v_float;
    PUSH_REG(shred, SZ_FLOAT);
  } else if(kind == Kindof_Complex) {
    *(m_complex*)shred->reg = retval.d.v_complex;
    PUSH_REG(shred, SZ_COMPLEX);
  } else if(kind == Kindof_Vec3) {
    *(VEC3_T*)shred->reg = retval.d.v_vec3;
    PUSH_REG(shred, SZ_VEC3);
  } else if(kind == Kindof_Vec4) {
    *(VEC4_T*)shred->reg = retval.d.v_vec4;
    PUSH_REG(shred, SZ_VEC4);
  }
  return;
}

DL_Func* new_dl_func(const m_str t, const m_str n, m_uint addr) {
  DL_Func* a = malloc(sizeof(DL_Func));
  a->name = n;
  a->type = t;
  a->d.addr = addr;
  a->args = new_vector();
  return a;
}

void free_dl_func(DL_Func* a) {
  free_vector(a->args);
  free(a);
}

static DL_Value* new_DL_Value(const m_str t, const m_str  n, m_bool c, void* addr) {
  DL_Value* a = malloc(sizeof(DL_Value));
  a->name = n;
  a->type = t;
  a->is_const  = c;
  a->static_addr = addr;
  a->doc = NULL;
  return a;
}

DL_Value* dl_func_add_arg(DL_Func* a, const m_str t, const m_str  n) {
  DL_Value* v = new_DL_Value(t, n, 0, NULL);
  vector_append(a->args, (vtype)v);
  return v;
}
