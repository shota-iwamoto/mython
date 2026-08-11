#include "types.h"

#include <string.h>

#include "util.h"

Type *ty_int;
Type *ty_bool;
Type *ty_none;
Type *ty_str;

static Type *new_type(TypeKind kind) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = kind;
    return t;
}

void types_init(void) {
    ty_int = new_type(TY_INT);
    ty_bool = new_type(TY_BOOL);
    ty_none = new_type(TY_NONE);
    ty_str = new_type(TY_STR);
}

Type *type_list(Type *elem) {
    Type *t = new_type(TY_LIST);
    t->elem = elem;
    return t;
}

bool type_equal(Type *a, Type *b) {
    // シングルトンなので、プリミティブ型どうしはここで済む
    if (a == b) return true;
    if (a->kind != b->kind) return false;

    // ★ 第10章：複合型は中身まで見る。
    //   list[int] と list[str] はどちらも kind == TY_LIST なので、
    //   ここが無いと「同じ型」と判定されてしまいます
    //   （第5章のコメントで予告していた穴）。
    if (a->kind == TY_LIST) return type_equal(a->elem, b->elem);

    // 第12章：class なら定義の同一性で比較する
    return true;
}

const char *type_name(Type *t) {
    switch (t->kind) {
        case TY_INT: return "int";
        case TY_BOOL: return "bool";
        case TY_NONE: return "None";
        case TY_STR: return "str";
        case TY_LIST: {
            // ⚠️ 動的に組み立てるので、返り値は毎回新しい文字列になります。
            //    解放しない方針（メモリモデル 3 節）なので問題ありません。
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "list[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
        default: UNREACHABLE();
    }
}

Type *type_from_name(const char *name) {
    if (strcmp(name, "int") == 0) return ty_int;
    if (strcmp(name, "bool") == 0) return ty_bool;
    if (strcmp(name, "None") == 0) return ty_none;
    if (strcmp(name, "str") == 0) return ty_str;
    return NULL;  // 未知の型名
}

Type *type_from_kind(int kind) {
    switch (kind) {
        case TY_INT: return ty_int;
        case TY_BOOL: return ty_bool;
        case TY_NONE: return ty_none;
        case TY_STR: return ty_str;
        default: UNREACHABLE();
    }
}

const char *type_name_list(void) { return "int, bool, str, None, list[T]"; }
