#include "types.h"

#include <string.h>

#include "util.h"

Type *ty_int;

static Type *new_type(TypeKind kind) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = kind;
    return t;
}

void types_init(void) { ty_int = new_type(TY_INT); }

bool type_equal(Type *a, Type *b) {
    // シングルトンなので、プリミティブ型どうしはここで済む
    if (a == b) return true;
    if (a->kind != b->kind) return false;

    // 第10章：list[T] なら要素型を再帰比較する
    // 第12章：class なら定義の同一性で比較する
    return true;
}

const char *type_name(Type *t) {
    switch (t->kind) {
        case TY_INT: return "int";
        default: UNREACHABLE();
    }
}

Type *type_from_name(const char *name) {
    if (strcmp(name, "int") == 0) return ty_int;
    return NULL;  // 未知の型名
}

const char *type_name_list(void) { return "int"; }
