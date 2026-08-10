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
    char *name;      // Mython 上の名前（エラーメッセージ用）
    char *ir_name;   // LLVM 上の名前。★ 記号（% / @）まで含めた完全な形
                     //   ローカル : %x, %x.1
                     //   グローバル: @g.x（第8章）
    bool is_global;  // 第8章
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
// これまでに使った IR 名の記録（衝突を避けるため）
typedef struct UsedName UsedName;
struct UsedName {
    char *name;
    UsedName *next;
};

// 関数のシグネチャ表（第8章）。
//
// ★ 本体を見る前に、全部の関数をここに登録します（8.5 節）。
//   前方参照と再帰が自然に通るようになります。
typedef struct FuncSig FuncSig;
struct FuncSig {
    char *name;
    Type *ret;
    Type **params;  // 引数の型
    char **pnames;  // 引数名（エラーメッセージ用）
    int nparams;
    Token *tok;     // 定義位置（「この関数はここで定義されています」用）
    FuncSig *next;
};

typedef struct {
    Scope *scope;      // 現在のスコープ
    int loop_depth;    // 今いるループの深さ（break / continue の検査用）
    UsedName *used;    // 割り当て済みの IR 名（関数ごとにリセット）
    FuncSig *funcs;    // 関数表（第8章）
    FuncSig *cur_func; // 今どの関数を検査中か（return の検査に必要）
} Sema;

static FuncSig *lookup_func(Sema *s, const char *name) {
    for (FuncSig *f = s->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

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

// ── IR 名の割り当て（第7章）──────────────────────────────
//
// ★ 第5章の「シャドーイング禁止なので変数名がそのまま一意」という前提は、
//   ブロックスコープが入ると崩れます。兄弟スコープが同じ名前を使えるからです。
//
//       if a:
//           x: int = 1     ← %x
//       if b:
//           x: int = 2     ← %x（衝突！どちらも相手を隠していない）
//
//   衝突したら連番を足します。これは名前修飾（mangling）の入口で、
//   第12章（メソッド）と第13章（モジュール）で本格的に必要になります。
//
// 🤔 なぜ sema がやるのか
//   parser はスコープを知らず、codegen は宣言と参照を結びつける情報を
//   持っていません。シンボルテーブルを持つ sema だけが両方できます。
static bool name_used(Sema *s, const char *name) {
    for (UsedName *u = s->used; u; u = u->next)
        if (strcmp(u->name, name) == 0) return true;
    return false;
}

static void remember_name(Sema *s, char *name) {
    UsedName *u = xmalloc(sizeof(UsedName));
    u->name = name;
    u->next = s->used;
    s->used = u;
}

static char *unique_ir_name(Sema *s, char *name) {
    if (!name_used(s, name)) {
        remember_name(s, name);
        return name;
    }
    for (int i = 1;; i++) {
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "%s.%d", name, i);
        char *cand = sb_str(&sb);
        if (!name_used(s, cand)) {
            remember_name(s, cand);
            return cand;
        }
    }
}

// ローカル変数として登録する（IR 名は %x 形式）
static VarEntry *declare(Sema *s, char *name, Type *type, Token *tok) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%%%s", unique_ir_name(s, name));

    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = name;
    v->ir_name = sb_str(&sb);
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

static Type *check_call(Sema *s, Node *n);

// 二項演算子が、その型に適用できるか
static bool op_supports(OpKind op, Type *t) {
    // 比較は int どうし・bool どうしのどちらでも使える。
    // （両辺の型が等しいことは呼び出し側で検査済み）
    // 言語仕様 4.3 / docs/spec/type-system.md 5.5
    if (is_compare(op)) return true;

    if (t->kind == TY_INT) {
        // 言語仕様 4.2：int に '/' は使えない（'//' を使う）
        return op != OP_TRUEDIV;
    }
    // str に許すのは連結の '+' だけ（言語仕様 4.2 の表）。
    // ⚠️ Python の "ab" * 3 は便利だが、v1 では採用しない。
    if (t->kind == TY_STR) return op == OP_ADD;
    return false;  // ★ bool に算術・ビット演算は使えない
}

// 「ここには bool が必要」というエラー。
// and の左辺・and の右辺・not の 3 か所で同じ形になるので関数にまとめます
// （第2章の span_token、第4章の advance_newline と同じ「3 回目でまとめる」判断）。
// ★ 第7章で一般化：if / while の条件からも呼ばれるようになったので、
//   「どこで bool が必要なのか」を文字列で受け取る形に変えました。
//   ND_IF には op が無いため、op_symbol() を使う形のままでは書けません。
//   最初から汎用に作らず、2 つ目の利用者が現れてから一般化する。
static Type *bool_required(const char *message, const char *where_label,
                           Token *where_tok, Node *operand, Type *actual) {
    Diag d = {0};
    d.message = message;
    d.primary.tok = operand->tok;
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(actual));
    d.related.tok = where_tok;
    d.related.label = where_label;
    d.hint = "Mython は int を真偽値として扱いません（言語仕様 4.4）。"
             "比較を書いてください（例: x != 0）";
    diag_fail(&d);
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

    // ★ 比較は bool を返す。算術は両辺と同じ型を返す。
    return is_compare(n->op) ? ty_bool : l;
}

// and / or は両辺が bool のみ（言語仕様 4.4）。
// Python と違い int を真偽値として扱いません（truthiness を採用しない）。
//
// 🤔 なぜ「最後に評価した値」を返さないのか
//   1 and "hello" のような式の型が一意に決まらなくなるからです。
//   bool に固定すれば and / or の型は常に bool です。
static Type *check_logical(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);
    Type *r = check_expr(s, n->rhs);

    char *msg = diag_fmt("演算子 '%s' には bool が必要です", op_symbol(n->op));
    char *lbl = diag_fmt("演算子 '%s' はここです", op_symbol(n->op));
    if (l->kind != TY_BOOL) return bool_required(msg, lbl, n->tok, n->lhs, l);
    if (r->kind != TY_BOOL) return bool_required(msg, lbl, n->tok, n->rhs, r);
    return ty_bool;
}

static Type *check_unary(Sema *s, Node *n) {
    Type *t = check_expr(s, n->lhs);

    // not は bool を取り bool を返す（言語仕様 4.4）
    if (n->op == OP_NOT) {
        if (t->kind != TY_BOOL)
            return bool_required(
                diag_fmt("演算子 '%s' には bool が必要です", op_symbol(n->op)),
                diag_fmt("演算子 '%s' はここです", op_symbol(n->op)), n->tok, n->lhs,
                t);
        return ty_bool;
    }

    // - + ~ は int のみ
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
    n->ir_name = v->ir_name;  // ★ codegen はこれを使う
    return v->type;
}

static Type *check_expr(Sema *s, Node *n) {
    Type *t;
    switch (n->kind) {
        case ND_INT: t = ty_int; break;
        case ND_BOOL: t = ty_bool; break;
        case ND_STR: t = ty_str; break;
        case ND_VAR: t = check_var(s, n); break;
        case ND_BINOP: t = check_binop(s, n); break;
        case ND_LOGICAL: t = check_logical(s, n); break;
        case ND_CALL: t = check_call(s, n); break;
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

    // ②' 外側のスコープの変数を隠していないか（シャドーイング禁止：言語仕様 5.1）
    //
    // ★ 第5章で lookup と lookup_local を分けておいた判断が、ここで報われます。
    //   同じスコープの再宣言（上）と、外側を隠す宣言（ここ）とで
    //   別々の診断を出せます。1 つの関数で済ませていたら同じ文言でした。
    VarEntry *outer = lookup(s, n->name);
    if (outer) {
        Diag d = {0};
        d.message = diag_fmt("変数 '%s' は外側のスコープの変数を隠しています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "シャドーイングは禁止されています（言語仕様 5.1）";
        d.related.tok = outer->decl_tok;
        d.related.label = "外側の宣言はここです";
        d.hint = "別の名前にするか、型注釈を外して既存の変数に代入してください"
                 "（例: x = 1）";
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
    VarEntry *v = declare(s, n->name, declared, n->tok);
    n->ir_name = v->ir_name;  // ★ codegen が alloca / store に使う名前
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
    target->ir_name = v->ir_name;

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

// 条件式は bool でなければならない（言語仕様 5.3 / 5.4）
static void check_cond(Sema *s, const char *where, Node *stmt_node, Node *cond) {
    Type *t = check_expr(s, cond);
    if (t->kind != TY_BOOL)
        bool_required(diag_fmt("%sには bool が必要です", where),
                      diag_fmt("%sはここです", where), stmt_node->tok, cond, t);
}

// ブロックは新しいスコープを作る。
// ★ 第5章で作った scope_push / scope_pop が、ここで初めて入れ子で対になります。
static void check_block(Sema *s, Node *n) {
    scope_push(s);
    for (Node *st = n->body; st; st = st->next) check_stmt(s, st);
    scope_pop(s);
}

// ── 組み込み関数の表（第9章）──────────────────────────────
//
// ★ 名前 + 引数型 で 1 つの候補を表します。
//   sema は「型が合う候補があるか」を、codegen は「どの C 関数を呼ぶか」を
//   同じ表から引きます。
//
// 🤔 なぜ print だけオーバーロードを許すのか（言語仕様 7 節）
//   ユーザー定義関数のオーバーロードは許しません（名前解決が複雑になる）。
//   組み込みは表を引くだけで解決できるので、「言語機能」ではなく
//   「表のエントリ」として扱えます。実装が増えません。
const Builtin BUILTINS[] = {
    // 名前     引数型     戻り型    呼び出す C 関数
    {"print", TY_INT, TY_NONE, "my_print_int"},
    {"print", TY_STR, TY_NONE, "my_print_str"},
    {"print", TY_BOOL, TY_NONE, "my_print_bool"},
    {"len", TY_STR, TY_INT, "my_str_len"},
    {"str", TY_INT, TY_STR, "my_str_from_int"},
    {"str", TY_BOOL, TY_STR, "my_str_from_bool"},
    {"int", TY_STR, TY_INT, "my_str_to_int"},
    {"ord", TY_STR, TY_INT, "my_ord"},
    {"chr", TY_INT, TY_STR, "my_chr"},
    {"exit", TY_INT, TY_NONE, "my_exit"},
    {"panic", TY_STR, TY_NONE, "my_panic"},
    {NULL, 0, 0, NULL},
};

// その名前の組み込みが 1 つでもあるか
bool is_builtin_name(const char *name) {
    for (int i = 0; BUILTINS[i].name; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return true;
    return false;
}

// 受け取れる型の一覧（エラーメッセージ用）。第5章の type_name_list と同じ発想。
static const char *builtin_arg_types(const char *name) {
    StrBuf sb;
    sb_init(&sb);
    bool first = true;
    for (int i = 0; BUILTINS[i].name; i++) {
        if (strcmp(BUILTINS[i].name, name) != 0) continue;
        Type *t = type_from_kind(BUILTINS[i].arg);
        sb_printf(&sb, "%s%s", first ? "" : ", ", type_name(t));
        first = false;
    }
    return sb_str(&sb);
}

// 組み込み関数の呼び出しを検査し、使う候補を n->builtin に記録する。
static Type *check_builtin_call(Sema *s, Node *n) {
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != 1) {
        Diag d = {0};
        d.message = diag_fmt("%s は 1 個の引数を取りますが、%d 個渡されました",
                             n->name, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "引数の個数が違います";
        d.hint = diag_fmt("%s(値) の形で使ってください", n->name);
        diag_fail(&d);
    }

    Type *at = check_expr(s, n->args);
    for (int i = 0; BUILTINS[i].name; i++) {
        if (strcmp(BUILTINS[i].name, n->name) != 0) continue;
        if (BUILTINS[i].arg != (int)at->kind) continue;
        n->builtin = &BUILTINS[i];  // ★ codegen はこれを見る
        return type_from_kind(BUILTINS[i].ret);
    }

    Diag d = {0};
    d.message = diag_fmt("%s は '%s' 型を受け取れません", n->name, type_name(at));
    d.primary.tok = n->args->tok;
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
    d.hint = diag_fmt("%s が受け取れるのは %s です", n->name,
                      builtin_arg_types(n->name));
    diag_fail(&d);
}

// 関数呼び出しの検査（docs/spec/type-system.md 5.7 の順序に従う）
static Type *check_call(Sema *s, Node *n) {
    if (is_builtin_name(n->name)) return check_builtin_call(s, n);

    // ① 定義されているか
    FuncSig *f = lookup_func(s, n->name);
    if (!f) {
        Diag d = {0};
        d.message = diag_fmt("未定義の関数 '%s' です", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この関数は定義されていません";
        d.hint = "関数名の綴りを確認してください"
                 "（定義の順序は問いません。後ろで定義した関数も呼べます）";
        diag_fail(&d);
    }

    // ③ 引数の個数（② の「呼び出し可能か」は構文が保証している）
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams) {
        Diag d = {0};
        d.message = diag_fmt("関数 '%s' は %d 個の引数を取りますが、%d 個渡されました",
                             f->name, f->nparams, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "呼び出しの引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "この関数はここで定義されています";
        diag_fail(&d);
    }

    // ④ 各引数の型
    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        Type *at = check_expr(s, a);
        if (!type_equal(at, f->params[i])) {
            Diag d = {0};
            d.message = diag_fmt("関数 '%s' の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 f->name, i + 1, type_name(at),
                                 type_name(f->params[i]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i],
                                       type_name(f->params[i]));
            d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
            diag_fail(&d);
        }
    }
    return f->ret;
}

// return の検査
static void check_return(Sema *s, Node *n) {
    Type *want = s->cur_func->ret;

    if (!n->lhs) {  // return（値なし）
        if (want->kind != TY_NONE) {
            Diag d = {0};
            d.message = diag_fmt("関数 '%s' は '%s' を返さなければなりません",
                                 s->cur_func->name, type_name(want));
            d.primary.tok = n->tok;
            d.primary.label = "この return には値がありません";
            d.related.tok = s->cur_func->tok;
            d.related.label = "戻り型はここで宣言されています";
            diag_fail(&d);
        }
        return;
    }

    Type *got = check_expr(s, n->lhs);
    if (want->kind == TY_NONE) {
        Diag d = {0};
        d.message = diag_fmt("戻り型が None の関数 '%s' は値を返せません",
                             s->cur_func->name);
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = "戻り型はここで宣言されています";
        diag_fail(&d);
    }
    if (!type_equal(got, want)) {
        Diag d = {0};
        d.message = "return の型が戻り型と一致しません";
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = diag_fmt("関数 '%s' の戻り型は '%s' です", s->cur_func->name,
                                   type_name(want));
        d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
        diag_fail(&d);
    }
}

static void check_stmt(Sema *s, Node *n) {
    switch (n->kind) {
        case ND_VARDECL: check_vardecl(s, n); break;
        case ND_ASSIGN: check_assign(s, n); break;
        case ND_BLOCK: check_block(s, n); break;
        case ND_RETURN: check_return(s, n); break;
        case ND_PASS: break;  // 何もしない

        case ND_IF:
            check_cond(s, "if の条件", n, n->lhs);
            check_block(s, n->body);
            // els は ND_BLOCK（else）か ND_IF（elif の脱糖結果）
            if (n->els) check_stmt(s, n->els);
            break;

        case ND_WHILE:
            check_cond(s, "while の条件", n, n->lhs);
            s->loop_depth++;
            check_block(s, n->body);
            s->loop_depth--;
            break;

        case ND_BREAK:
        case ND_CONTINUE: {
            // ★ ここで弾いておけば、codegen は「飛び先が必ずある」と仮定できます。
            //   （第5章で確立した「codegen は検査済みの AST だけを受け取る」）
            if (s->loop_depth > 0) break;
            const char *kw = n->kind == ND_BREAK ? "break" : "continue";
            Diag d = {0};
            d.message = diag_fmt("'%s' はループの外では使えません", kw);
            d.primary.tok = n->tok;
            d.primary.label = diag_fmt("この '%s' を囲む while がありません", kw);
            d.hint = diag_fmt("'%s' は while の中でだけ使えます", kw);
            diag_fail(&d);
            break;
        }

        default: check_expr(s, n); break;  // 式文
    }
}

// ── 入口 ───────────────────────────────────────────────────

// この文を実行したら、必ず関数から抜けるか（型システム 6.1）。
//
// ⚠️ 保守的に判定します。「実際には到達しない」経路でも return を要求します。
//    コンパイラが人間より賢くなろうとすると必ず破綻します。
//
// 📖 codegen の e->terminated と同じことを、別の場所でやっています。
//    こちらは AST の上（構造を見る／ユーザーに教えるため）、
//    あちらは命令列の上（出力を見る／正しい IR を出すため）。
static bool always_returns(Node *n) {
    if (!n) return false;

    switch (n->kind) {
        case ND_RETURN:
            return true;

        case ND_IF:
            // else が無ければ、条件が偽のときに素通りする
            return n->els && always_returns(n->body) && always_returns(n->els);

        case ND_BLOCK:
            // 1 つでも「必ず抜ける」文があればよい（その後ろは到達不能）
            for (Node *st = n->body; st; st = st->next)
                if (always_returns(st)) return true;
            return false;

        // while True: は break が無ければ抜けないが、v1 では判定しない
        default:
            return false;
    }
}

// ── パス 1：宣言の登録 ─────────────────────────────────────

static void declare_func(Sema *s, Node *n) {
    if (is_builtin_name(n->name))
        error_at_hint(n->tok, diag_fmt("%s は組み込み関数です。別の名前を使ってください",
                                       n->name),
                      "'%s' は再定義できません", n->name);

    FuncSig *prev = lookup_func(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("関数 '%s' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再定義されています";
        d.related.tok = prev->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }

    Type *ret = type_from_name(n->type_name);
    if (!ret) {
        Diag d = {0};
        d.message = diag_fmt("未知の型名 '%s' です", n->type_name);
        d.primary.tok = n->tok;
        d.primary.label = "この戻り型は存在しません";
        d.hint = diag_fmt("現在使える型: %s", type_name_list());
        diag_fail(&d);
    }

    int nparams = 0;
    for (Node *pm = n->params; pm; pm = pm->next) nparams++;

    FuncSig *f = xmalloc(sizeof(FuncSig));
    f->name = n->name;
    f->ret = ret;
    f->nparams = nparams;
    f->params = nparams ? xmalloc(sizeof(Type *) * (size_t)nparams) : NULL;
    f->pnames = nparams ? xmalloc(sizeof(char *) * (size_t)nparams) : NULL;
    f->tok = n->tok;

    int i = 0;
    for (Node *pm = n->params; pm; pm = pm->next, i++) {
        Type *pt = type_from_name(pm->type_name);
        if (!pt) {
            Diag d = {0};
            d.message = diag_fmt("未知の型名 '%s' です", pm->type_name);
            d.primary.tok = pm->tok;
            d.primary.label = "この型は存在しません";
            d.hint = diag_fmt("現在使える型: %s", type_name_list());
            diag_fail(&d);
        }
        if (pt->kind == TY_NONE)
            error_at_hint(pm->tok, "None 型の値は存在しないので引数にできません",
                          "引数の型に None は使えません");
        f->params[i] = pt;
        f->pnames[i] = pm->name;
        pm->type = pt;
    }

    f->next = s->funcs;
    s->funcs = f;
    n->type = ret;
}

// グローバル変数の登録（言語仕様 6.2）
static void declare_global(Sema *s, Node *n) {
    Type *declared = type_from_name(n->type_name);
    if (!declared) {
        Diag d = {0};
        d.message = diag_fmt("未知の型名 '%s' です", n->type_name);
        d.primary.tok = n->tok;
        d.primary.label = "この型は存在しません";
        d.hint = diag_fmt("現在使える型: %s", type_name_list());
        diag_fail(&d);
    }
    if (declared->kind == TY_NONE)
        error_at_hint(n->tok, "None 型の値は存在しないので変数にできません",
                      "変数の型に None は使えません");

    VarEntry *prev = lookup_local(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("変数 '%s' は既に宣言されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再宣言されています";
        d.related.tok = prev->decl_tok;
        d.related.label = "最初の宣言はここです";
        diag_fail(&d);
    }

    // ⚠️ 初期化式はコンパイル時定数のみ（言語仕様 6.2 の v1 制限）。
    //    計算を許すと「どちらを先に初期化するか」という初期化順序問題が起きます。
    if (n->rhs->kind != ND_INT && n->rhs->kind != ND_BOOL &&
        n->rhs->kind != ND_STR) {
        Diag d = {0};
        d.message = "グローバル変数の初期化式は定数でなければなりません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = "ここには整数・True / False・文字列リテラルだけが書けます";
        d.hint = "計算が必要なら main の中でローカル変数にしてください";
        diag_fail(&d);
    }

    Type *actual = check_expr(s, n->rhs);
    if (!type_equal(actual, declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   n->name, type_name(declared));
        diag_fail(&d);
    }

    // グローバルの IR 名は @g.x。C のシンボルや @main と衝突させないため。
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "@g.%s", n->name);

    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = n->name;
    v->ir_name = sb_str(&sb);
    v->is_global = true;
    v->type = declared;
    v->decl_tok = n->tok;
    v->next = s->scope->vars;
    s->scope->vars = v;

    n->ir_name = v->ir_name;
    n->is_global = true;
    n->type = declared;
}

// ── パス 2：本体の検査 ─────────────────────────────────────

static void check_func(Sema *s, Node *n) {
    s->cur_func = lookup_func(s, n->name);
    s->used = NULL;  // IR 名は関数ごとに振り直す（別の関数なら衝突しない）

    scope_push(s);

    // 引数をローカル変数として登録する
    for (Node *pm = n->params; pm; pm = pm->next) {
        VarEntry *v = declare(s, pm->name, pm->type, pm->tok);
        pm->ir_name = v->ir_name;
    }

    for (Node *st = n->body->body; st; st = st->next) check_stmt(s, st);
    scope_pop(s);

    // 全経路で return するか（型システム 6.1）
    if (n->type->kind != TY_NONE && !always_returns(n->body)) {
        Diag d = {0};
        d.message = diag_fmt("関数 '%s' は値を返さずに終わる経路があります", n->name);
        d.primary.tok = n->tok;
        d.primary.label = diag_fmt("戻り型は '%s' です", type_name(n->type));
        d.hint = "すべての経路で return してください"
                 "（if に else が無いと、条件が偽のとき素通りします）";
        diag_fail(&d);
    }
    s->cur_func = NULL;
}

// main の検査（言語仕様 6.1）
static void check_main(Sema *s, Node *ast) {
    FuncSig *m = lookup_func(s, "main");
    if (!m) {
        Diag d = {0};
        d.message = "main 関数がありません";
        d.primary.tok = ast->tok;
        d.primary.label = "このファイルには入口がありません";
        d.hint = "プログラムの入口として次を定義してください:\n"
                 "             def main() -> int:\n"
                 "                 return 0";
        diag_fail(&d);
    }
    if (m->nparams != 0)
        error_at_hint(m->tok, "main は引数なしで定義してください（def main() -> int:）",
                      "main は引数を取れません");
    if (m->ret->kind != TY_INT)
        error_at_hint(m->tok, "main の戻り値がプロセスの終了コードになります",
                      "main の戻り型は int でなければなりません");
}

void sema(Node *ast) {
    if (ast->kind != ND_BLOCK) UNREACHABLE();

    Sema s = {0};
    scope_push(&s);  // グローバルスコープ

    // ── パス 1：シグネチャとグローバル変数を先に全部登録する ──
    // ★ 本体を見る前に登録するので、前方参照も再帰も自然に通ります。
    //   C がプロトタイプ宣言を要求するのは、この 2 パスを人間にやらせているからです。
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) declare_func(&s, d);
        else if (d->kind == ND_VARDECL) declare_global(&s, d);
        else UNREACHABLE();  // parser が保証している
    }

    // ── パス 2：本体を検査する ──
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_FUNC) check_func(&s, d);

    check_main(&s, ast);
    scope_pop(&s);
}
