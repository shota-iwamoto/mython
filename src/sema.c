#include "sema.h"

#include <string.h>

#include "diag.h"
#include "types.h"
#include "util.h"

// ── シンボルテーブルとスコープ ──────────────────────────────
//
// 「名前 → 型」の対応表です。
//
// 🤔 なぜハッシュテーブルではなく線形リストなのか
//   1 つのスコープに宣言される変数は普通 10 個程度です。
//   線形探索で十分速く、コードは 5 行で済みます。
//   「まず動かす、測ってから直す」が原則（docs/spec/type-system.md 7.2）。

typedef struct VarEntry VarEntry;
struct VarEntry {
    char *name;
    Type *type;
    Token *decl_tok;  // 宣言された位置（再宣言エラーで「前の宣言はここ」を示す）
    VarEntry *next;
};

typedef struct Scope Scope;
struct Scope {
    Scope *parent;   // 外側のスコープ（グローバルなら NULL）
    VarEntry *vars;  // このスコープで宣言された変数
};

// 意味解析の状態。
// 第8章で「今どの関数を検査中か」（戻り型の検査に必要）が加わります。
typedef struct {
    Scope *scope;  // 現在のスコープ
} Sema;

static Scope *scope_push(Sema *s) {
    Scope *sc = xmalloc(sizeof(Scope));
    sc->parent = s->scope;
    s->scope = sc;
    return sc;
}

// 第7章（if / while のブロックスコープ）で使います。
// 今はトップレベルの 1 段だけなので、対になる pop は最後の 1 回だけです。
static void scope_pop(Sema *s) { s->scope = s->scope->parent; }

// 現在のスコープだけを探す（再宣言の検査用）
static VarEntry *lookup_local(Sema *s, const char *name) {
    for (VarEntry *v = s->scope->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

// 内側から外側へ順に探す（名前解決）
static VarEntry *lookup(Sema *s, const char *name) {
    for (Scope *sc = s->scope; sc; sc = sc->parent)
        for (VarEntry *v = sc->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

static VarEntry *declare(Sema *s, char *name, Type *type, Token *tok) {
    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = name;
    v->type = type;
    v->decl_tok = tok;
    v->next = s->scope->vars;
    s->scope->vars = v;
    return v;
}

// ── 式の検査 ────────────────────────────────────────────────
//
// check_expr の約束：
//   「式を検査し、n->type を埋めて、その型を返す」
//
// gen_expr（コード生成）と対になる構造です。
static Type *check_expr(Sema *s, Node *n);

// 二項演算子が、その型に適用できるか
static bool op_supports(OpKind op, Type *t) {
    if (t->kind == TY_INT) {
        // 言語仕様 4.2：int に '/' は使えない（'//' を使う）
        return op != OP_TRUEDIV;
    }
    return false;
}

static Type *check_binop(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);
    Type *r = check_expr(s, n->rhs);

    // ★ 検査は 2 段構え（docs/spec/type-system.md 5.3）
    //   ① 両辺の型が等しいか
    //   ② その型がその演算子を支持するか
    //   この順にするとコードが短くなり、エラーメッセージも的確になります。
    if (!type_equal(l, r)) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' と '%s' に演算子 '%s' は適用できません",
                             type_name(l), type_name(r), op_symbol(n->op));
        d.primary.tok = n->tok;
        d.primary.label = "この演算子の両辺の型が違います";
        d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
        diag_fail(&d);
    }

    if (!op_supports(n->op, l)) {
        if (n->op == OP_TRUEDIV) {
            // 第2章では codegen で弾いていた検査を、本来の担当である
            // 意味解析パスに移しました。
            error_at_hint(n->tok,
                          "切り捨て除算の '//' を使ってください"
                          "（Mython には暗黙の型変換がないため、'/' は float 専用です）",
                          "整数の除算に '/' は使えません");
        }
        error_at(n->tok, "型 '%s' に演算子 '%s' は適用できません", type_name(l),
                 op_symbol(n->op));
    }

    // 0 除算のうち、右辺がリテラル 0 の場合はここで弾く。
    // （右辺が式の場合は実行時 SIGFPE。第9章で実行時チェックを入れます）
    if ((n->op == OP_FLOORDIV || n->op == OP_MOD) && n->rhs->kind == ND_INT &&
        n->rhs->ival == 0) {
        Diag d = {0};
        d.message = "0 で除算しています";
        d.primary.tok = n->rhs->tok;
        d.primary.label = "この 0 で割ろうとしています";
        d.related.tok = n->tok;
        d.related.label = diag_fmt("演算子 '%s' はここです", op_symbol(n->op));
        diag_fail(&d);
    }

    return l;  // 算術演算は両辺と同じ型を返す
}

static Type *check_unary(Sema *s, Node *n) {
    Type *t = check_expr(s, n->lhs);
    if (t->kind != TY_INT)
        error_at(n->tok, "型 '%s' に単項演算子 '%s' は適用できません", type_name(t),
                 op_symbol(n->op));
    return t;
}

static Type *check_var(Sema *s, Node *n) {
    VarEntry *v = lookup(s, n->name);
    if (!v) {
        Diag d = {0};
        d.message = diag_fmt("未定義の名前 '%s' です", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この名前は宣言されていません";
        d.hint = diag_fmt("使う前に宣言してください（例: %s: int = 0）", n->name);
        diag_fail(&d);
    }
    return v->type;
}

static Type *check_expr(Sema *s, Node *n) {
    Type *t;
    switch (n->kind) {
        case ND_INT: t = ty_int; break;
        case ND_VAR: t = check_var(s, n); break;
        case ND_BINOP: t = check_binop(s, n); break;
        case ND_UNARY: t = check_unary(s, n); break;
        default: UNREACHABLE();
    }
    n->type = t;  // ★ コード生成器はこれを見る
    return t;
}

// ── 文の検査 ────────────────────────────────────────────────

static void check_stmt(Sema *s, Node *n);

static void check_vardecl(Sema *s, Node *n) {
    // ① 型注釈の名前を解決する
    Type *declared = type_from_name(n->type_name);
    if (!declared) {
        Diag d = {0};
        d.message = diag_fmt("未知の型名 '%s' です", n->type_name);
        d.primary.tok = n->tok;
        d.primary.label = "この型は存在しません";
        d.hint = diag_fmt("現在使える型: %s", type_name_list());
        diag_fail(&d);
    }

    // ② 同じスコープでの再宣言を禁止（言語仕様 5.1）
    VarEntry *prev = lookup_local(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("変数 '%s' は既に宣言されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再宣言されています";
        d.related.tok = prev->decl_tok;
        d.related.label = "最初の宣言はここです";
        d.hint = "既存の変数に代入するなら型注釈を外してください（例: x = 1）";
        diag_fail(&d);
    }

    // ③ 初期化式の型が宣言した型に代入できるか
    Type *actual = check_expr(s, n->rhs);
    if (!type_equal(actual, declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label =
            diag_fmt("変数 '%s' は '%s' 型として宣言されています", n->name,
                     type_name(declared));
        diag_fail(&d);
    }

    // ④ スコープに登録する。
    //    ★ 順序が重要：初期化式を検査した「後」に登録します。
    //      そうしないと `x: int = x` が自分自身を参照できてしまいます。
    declare(s, n->name, declared, n->tok);
    n->type = declared;
}

static void check_assign(Sema *s, Node *n) {
    Node *target = n->lhs;
    if (target->kind != ND_VAR) UNREACHABLE();  // parser が保証している

    VarEntry *v = lookup(s, target->name);
    if (!v) {
        Diag d = {0};
        d.message = diag_fmt("未定義の名前 '%s' に代入しています", target->name);
        d.primary.tok = target->tok;
        d.primary.label = "この名前は宣言されていません";
        d.hint = diag_fmt("初めて使うときは型注釈が必要です（例: %s: int = 0）",
                          target->name);
        diag_fail(&d);
    }
    target->type = v->type;

    Type *actual = check_expr(s, n->rhs);
    if (!type_equal(actual, v->type)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = v->decl_tok;
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   v->name, type_name(v->type));
        diag_fail(&d);
    }
    n->type = v->type;
}

static void check_stmt(Sema *s, Node *n) {
    switch (n->kind) {
        case ND_VARDECL: check_vardecl(s, n); break;
        case ND_ASSIGN: check_assign(s, n); break;
        default: check_expr(s, n); break;  // 式文
    }
}

// ── 入口 ───────────────────────────────────────────────────

void sema(Node *ast) {
    if (ast->kind != ND_BLOCK) UNREACHABLE();

    Sema s = {0};
    scope_push(&s);  // トップレベルのスコープ

    Node *last = NULL;
    for (Node *stmt = ast->body; stmt; stmt = stmt->next) {
        check_stmt(&s, stmt);
        last = stmt;
    }

    // ⚠️ 暫定仕様：プログラムの値は最後の文の値なので、
    //    最後の文は「値を持つ式」でなければなりません。
    //    第8章で `def main() -> int:` と `return` に置き換わります。
    if (last->kind == ND_VARDECL || last->kind == ND_ASSIGN) {
        Diag d = {0};
        d.message = "プログラムの最後は式でなければなりません";
        d.primary.tok = last->tok;
        d.primary.label = "この文は値を持ちません";
        d.hint = "最後に値となる式を書いてください"
                 "（第8章で `def main() -> int:` と return に置き換わります）";
        diag_fail(&d);
    }

    ast->type = last->type;
    scope_pop(&s);
}
