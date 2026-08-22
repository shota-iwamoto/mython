#include "types.h"

#include <string.h>

#include "util.h"

Type *ty_int;
Type *ty_bool;
Type *ty_none;
Type *ty_str;
Type *ty_null;

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
    ty_null = new_type(TY_NULL);
}

// ── nullable（第15章）──────────────────────────────────────

bool type_can_be_opt(Type *t) {
    // ★ None はヌルポインタとして表すので、ポインタで表される型だけ。
    //   int を nullable にするには箱に入れる必要があり、そこから
    //   「int は値か参照か」という別の話が始まります（v1 では入れない）。
    return t->kind == TY_STR || t->kind == TY_LIST || t->kind == TY_CLASS;
}

Type *type_opt(Type *elem) {
    if (elem->kind == TY_OPT) return elem;  // (T | None) | None は T | None
    // ★ 同じ T には 1 個だけ作る（elem 側に覚えておく）
    if (elem->opt) return elem->opt;
    Type *t = new_type(TY_OPT);
    t->elem = elem;
    elem->opt = t;
    return t;
}

Type *type_strip_opt(Type *t) { return t->kind == TY_OPT ? t->elem : t; }

// 代入互換性（type-system.md 4 節）。
//
//   assignable(S → T) =
//       type_equal(S, T)                             (a) 完全一致
//       または (T が T2|None で S が None リテラル)     (b)
//       または (T が T2|None で assignable(S → T2))    (c) 広げる方向だけ許す
bool type_assignable(Type *from, Type *to) {
    if (type_equal(from, to)) return true;
    if (to->kind != TY_OPT) return false;
    if (from->kind == TY_NULL) return true;
    return type_assignable(from, to->elem);
}

Type *type_list(Type *elem) {
    Type *t = new_type(TY_LIST);
    t->elem = elem;
    return t;
}

Type *type_class(char *name, struct Class *cls) {
    Type *t = new_type(TY_CLASS);
    t->name = name;
    t->cls = cls;
    return t;
}

// ── サイズとアラインメント（第12章）──────────────────────────
//
// docs/design/memory-model.md 5 節の表のとおり。
// ★ 参照型（str / list / class）は「ポインタ 1 個」なので 8 バイトです。
//   指す先の大きさは関係ありません。
int type_size(Type *t) {
    switch (t->kind) {
        case TY_BOOL: return 1;  // メモリ上は i8（規約 R5）
        case TY_INT:
        case TY_STR:
        case TY_LIST:
        case TY_CLASS:
        case TY_OPT: return 8;  // 第15章：T | None もポインタ 1 個
        default: UNREACHABLE();  // None は値を持たない
    }
}

int type_align(Type *t) { return type_size(t); }

bool type_equal(Type *a, Type *b) {
    // シングルトンなので、プリミティブ型どうしはここで済む
    if (a == b) return true;
    if (a->kind != b->kind) return false;

    // ★ 第10章：複合型は中身まで見る。
    //   list[int] と list[str] はどちらも kind == TY_LIST なので、
    //   ここが無いと「同じ型」と判定されてしまいます
    //   （第5章のコメントで予告していた穴）。
    if (a->kind == TY_LIST) return type_equal(a->elem, b->elem);

    // ★ 第12章：クラスは「同じ定義か」で比べます。名前の一致ではありません。
    //   今は 1 ファイルなので同名クラスは 1 つだけですが、第13章で import が
    //   入ると lexer.Token と parser.Token が同時に存在しえます。
    //   定義ポインタで比べておけば、そのとき何も直さずに済みます。
    if (a->kind == TY_CLASS) return a->cls == b->cls;

    // ★ 第15章：T | None は中身どうしを比べる。
    //   type_equal を触るのは 4 度目です（ch5 → ch10 → ch12 → ch15）。
    if (a->kind == TY_OPT) return type_equal(a->elem, b->elem);

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
        case TY_CLASS: return t->name;  // 第12章
        case TY_OPT: {  // 第15章
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "%s | None", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_NULL: return "None";
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

const char *type_name_list(void) {
    return "int, bool, str, None, list[T], T | None";
}
