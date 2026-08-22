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
    FuncSig *funcs;    // 関数表（第8章。メソッドも "Token.show" として入る）
    FuncSig *cur_func; // 今どの関数を検査中か（return の検査に必要）
    Class *classes;    // クラス表（第12章）

    // 今この式に期待されている型（第10章）。
    //
    // ★ 空リスト [] だけは、それ自身から要素型が決まりません。
    //   本格的なやり方は双方向型検査（期待型を引数で渡す）ですが、
    //   v1 で期待型を必要とする式は [] だけなので、状態を 1 つ持たせて済ませます。
    // ⚠️ 期待型が要る式が増えたら、この手は破綻します。そのときは引数で渡す形に直します。
    Type *expected;
} Sema;

static FuncSig *lookup_func(Sema *s, const char *name) {
    for (FuncSig *f = s->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

// ── クラス表と名前修飾（第12章）────────────────────────────
//
// ★ メソッドは「名前を修飾しただけの、ただの関数」です。
//   名前を "Token.show" にしてしまえば、第8章で作った関数表にそのまま載ります。
//   '.' を含む名前は利用者が書ける識別子と絶対に衝突しません
//   （第11章の隠し変数 for.ix.0 と同じ手口）。
static Class *lookup_class(Sema *s, const char *name) {
    for (Class *c = s->classes; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static char *mangle(const char *cls, const char *method) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.%s", cls, method);
    return sb_str(&sb);
}

static Field *lookup_field(Class *c, const char *name) {
    for (Field *f = c->fields; f; f = f->next)
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
static Type *check_list_lit(Sema *s, Node *n);
static Type *check_index_expr(Sema *s, Node *n);
static Type *check_method(Sema *s, Node *n);
static Type *check_field(Sema *s, Node *n);

// 型注釈（構文）を Type（意味）に変換する。
//
// ★ 「名前から型への解決は sema の仕事」（第5章の判断 #47）が、
//   複合型になっても同じ形で通用します。
static Type *resolve_type(Sema *s, Node *tr) {
    if (strcmp(tr->name, "list") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "要素型を書いてください（例: list[int]）",
                          "list には要素型が必要です");
        Type *elem = resolve_type(s, tr->lhs);  // ★ 再帰
        if (elem->kind == TY_NONE)
            error_at_hint(tr->tok, "None 型の値は存在しないので要素にできません",
                          "list の要素型に None は使えません");
        return type_list(elem);
    }

    if (tr->lhs)
        error_at_hint(tr->tok, "要素型を取るのは list だけです",
                      "型 '%s' は要素型を取りません", tr->name);

    Type *t = type_from_name(tr->name);
    if (t) return t;

    // ★ 第12章：組み込みの型名で無ければ、クラス名として引きます。
    //   「型の一覧がソースコードによって増える」のは、この章が初めてです。
    Class *c = lookup_class(s, tr->name);
    if (c) return c->type;

    Diag d = {0};
    d.message = diag_fmt("未知の型名 '%s' です", tr->name);
    d.primary.tok = tr->tok;
    d.primary.label = "この型は存在しません";
    d.hint = diag_fmt("現在使える型: %s、および定義したクラス名", type_name_list());
    diag_fail(&d);
}

// 二項演算子が、その型に適用できるか
static bool op_supports(OpKind op, Type *t) {
    // ★ 第12章：クラスと list は「参照」なので、比べられるのは
    //   同一性（== / !=）だけです。大小関係には意味がありません
    //   （言語仕様 4.3 / docs/spec/type-system.md 5.6）。
    if (t->kind == TY_CLASS || t->kind == TY_LIST)
        return op == OP_EQ || op == OP_NE;

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
        case ND_LIST: t = check_list_lit(s, n); break;
        case ND_INDEX: t = check_index_expr(s, n); break;
        case ND_METHOD: t = check_method(s, n); break;
        case ND_FIELD: t = check_field(s, n); break;
        case ND_UNARY: t = check_unary(s, n); break;
        default: UNREACHABLE();
    }
    n->type = t;  // ★ コード生成器はこれを見る
    return t;
}

// ── 文の検査 ────────────────────────────────────────────────

static void check_stmt(Sema *s, Node *n);

static void check_vardecl(Sema *s, Node *n) {
    // ① 型注釈を解決する（第10章で木になった）
    //
    // ★ 第11章：type_ref が NULL なら「コンパイラが作った宣言」（for の脱糖）。
    //   初期化式の型をそのまま使います。
    // ⚠️ 利用者が書く宣言では parser が必ず type_ref を作るので、
    //    「型注釈は必須」（言語仕様 3.3）は破られません。
    //    言語仕様 5.5 も「ループ変数は型注釈不要（要素型から決まる）」としています。
    Type *declared = NULL;
    if (n->type_ref) {
        declared = resolve_type(s, n->type_ref);
        if (declared->kind == TY_NONE)
            error_at_hint(n->tok, "None 型の値は存在しないので変数にできません",
                          "変数の型に None は使えません");
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

    // ③ 初期化式の型が宣言した型に代入できるか。
    //    ★ 空リスト [] の要素型を決めるため、期待型を渡す（第10章）
    s->expected = declared;
    Type *actual = check_expr(s, n->rhs);
    s->expected = NULL;

    // 型注釈が無ければ、初期化式の型がそのまま変数の型になる（第11章）
    if (!declared) {
        if (actual->kind == TY_NONE)
            error_at_hint(n->rhs->tok, "値を返さない式は変数に入れられません",
                          "None 型の値は変数にできません");
        declared = actual;
    }

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

    // 添字への代入 xs[i] = v（第10章）
    if (target->kind == ND_INDEX) {
        Type *et = check_index_expr(s, target);
        target->type = et;

        // ⚠️ str は不変（immutable）なので s[0] = "x" は書けません（言語仕様 3.1）
        if (target->lhs->type->kind == TY_STR)
            error_at_hint(target->tok,
                          "str は不変（immutable）です。新しい文字列を作ってください",
                          "文字列の要素には代入できません");

        s->expected = et;
        Type *actual = check_expr(s, n->rhs);
        s->expected = NULL;

        if (!type_equal(actual, et)) {
            Diag d = {0};
            d.message = "型が一致しません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
            d.related.tok = target->tok;
            d.related.label = diag_fmt("この要素は '%s' 型です", type_name(et));
            d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
            diag_fail(&d);
        }
        n->type = et;
        return;
    }

    // フィールドへの代入 t.kind = v（第12章）。
    // ★ 添字への代入とまったく同じ形です（型を引く関数が違うだけ）。
    if (target->kind == ND_FIELD) {
        Type *ft = check_field(s, target);
        target->type = ft;

        s->expected = ft;
        Type *actual = check_expr(s, n->rhs);
        s->expected = NULL;

        if (!type_equal(actual, ft)) {
            Diag d = {0};
            d.message = "型が一致しません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
            d.related.tok = target->field->tok;
            d.related.label = diag_fmt("フィールド '%s' は '%s' 型として宣言されています",
                                       target->field->name, type_name(ft));
            d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
            diag_fail(&d);
        }
        n->type = ft;
        return;
    }

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

    s->expected = v->type;  // ★ xs = [] のため（第10章）
    Type *actual = check_expr(s, n->rhs);
    s->expected = NULL;
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
    {"len", TY_LIST, TY_INT, "my_list_len"},  // 第10章（要素型は見ない）
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
        // list[T] にはシングルトンが無いので、表示用の名前を直接書く（第10章）
        const char *nm = BUILTINS[i].arg == TY_LIST
                             ? "list[T]"
                             : type_name(type_from_kind(BUILTINS[i].arg));
        sb_printf(&sb, "%s%s", first ? "" : ", ", nm);
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

// リストリテラルの検査（第10章）
static Type *check_list_lit(Sema *s, Node *n) {
    Type *want = s->expected;  // ★ 使う前に控える（下で check_expr が上書きするため）

    if (!n->body) {
        // 空リストは、それ自身から要素型が決まらない
        if (!want || want->kind != TY_LIST) {
            Diag d = {0};
            d.message = "空のリストの要素型が決まりません";
            d.primary.tok = n->tok;
            d.primary.label = "この [] がどんなリストなのか分かりません";
            d.hint = "型注釈を書いてください（例: xs: list[int] = []）。"
                     "関数の引数に直接渡す場合は、いったん変数に入れてください";
            diag_fail(&d);
        }
        return want;
    }

    // 要素があるなら、最初の要素の型を要素型にする（推論はしない）
    s->expected = want && want->kind == TY_LIST ? want->elem : NULL;
    Type *et = check_expr(s, n->body);

    int i = 2;
    for (Node *el = n->body->next; el; el = el->next, i++) {
        s->expected = et;
        Type *t = check_expr(s, el);
        if (!type_equal(t, et)) {
            Diag d = {0};
            d.message = diag_fmt("リストの要素の型がそろっていません（第 %d 要素）", i);
            d.primary.tok = el->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(t));
            d.related.tok = n->body->tok;
            d.related.label = diag_fmt("最初の要素は '%s' 型です", type_name(et));
            d.hint = "リストの要素はすべて同じ型でなければなりません";
            diag_fail(&d);
        }
    }
    s->expected = NULL;
    return type_list(et);
}

// 添字アクセスの検査（型システム 5.8）
static Type *check_index_expr(Sema *s, Node *n) {
    Type *ot = check_expr(s, n->lhs);
    Type *it = check_expr(s, n->rhs);

    if (it->kind != TY_INT) {
        Diag d = {0};
        d.message = "添字は int でなければなりません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("これは '%s' 型です", type_name(it));
        diag_fail(&d);
    }

    if (ot->kind == TY_LIST) return ot->elem;
    // ★ str の添字は 1 文字の str を返す（char 型は作らない。型システム 5.8）
    if (ot->kind == TY_STR) return ty_str;

    Diag d = {0};
    d.message = diag_fmt("型 '%s' は添字を取れません", type_name(ot));
    d.primary.tok = n->lhs->tok;
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(ot));
    d.hint = "添字が使えるのは list[T] と str です";
    diag_fail(&d);
}

// フィールドアクセスの検査（型システム 5.9。第12章）
static Type *check_field(Sema *s, Node *n) {
    Type *ot = check_expr(s, n->lhs);

    if (ot->kind != TY_CLASS) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' にフィールドはありません", type_name(ot));
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("これは '%s' 型です", type_name(ot));
        d.hint = "'.' でフィールドを読めるのは class のインスタンスだけです";
        diag_fail(&d);
    }

    Field *f = lookup_field(ot->cls, n->name);
    if (!f) {
        Diag d = {0};
        d.message = diag_fmt("クラス '%s' にフィールド '%s' はありません",
                             ot->cls->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = "このフィールドは宣言されていません";
        d.related.tok = ot->cls->tok;
        d.related.label = "クラスの定義はここです";
        d.hint = "クラス本体の先頭に「名前: 型」の形で宣言してください";
        diag_fail(&d);
    }

    n->field = f;  // ★ codegen はこれ（の index）を getelementptr に渡す
    return f->type;
}

// クラスのメソッド呼び出しの検査（型システム 5.10。第12章）。
//
// ★ 「関数呼び出しの検査に self を 1 個足すだけ」です。
//   名前を修飾して関数表に載せておいたので、引ける表は第8章のまま。
static Type *check_class_method(Sema *s, Node *n, Class *c) {
    char *mname = mangle(c->name, n->name);
    FuncSig *f = lookup_func(s, mname);
    if (!f) {
        Diag d = {0};
        d.message = diag_fmt("クラス '%s' にメソッド '%s' はありません", c->name,
                             n->name);
        d.primary.tok = n->tok;
        d.primary.label = "このメソッドは定義されていません";
        d.related.tok = c->tok;
        d.related.label = "クラスの定義はここです";
        if (lookup_field(c, n->name))
            d.hint = diag_fmt("'%s' はフィールドです。'()' を外してください", n->name);
        diag_fail(&d);
    }

    // 引数の個数（self は数えない）
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams - 1) {
        Diag d = {0};
        d.message = diag_fmt("メソッド '%s' は %d 個の引数を取りますが、%d 個渡されました",
                             mname, f->nparams - 1, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "呼び出しの引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "このメソッドはここで定義されています";
        d.hint = "self は自動的に渡されるので、書く必要はありません";
        diag_fail(&d);
    }

    // ★ 第 1 引数は self なので、実引数は params[i + 1] と比べます
    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        s->expected = f->params[i + 1];
        Type *at = check_expr(s, a);
        s->expected = NULL;
        if (!type_equal(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = diag_fmt("メソッド '%s' の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 mname, i + 1, type_name(at),
                                 type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i + 1],
                                       type_name(f->params[i + 1]));
            d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
            diag_fail(&d);
        }
    }

    n->ir_name = mname;  // ★ codegen が呼ぶ関数名（@Token.show）
    return f->ret;
}

// メソッド呼び出しの検査（第10章の list.append と、第12章のクラスのメソッド）
static Type *check_method(Sema *s, Node *n) {
    Type *ot = check_expr(s, n->lhs);

    if (ot->kind == TY_CLASS) return check_class_method(s, n, ot->cls);

    if (ot->kind == TY_LIST && strcmp(n->name, "append") == 0) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1) {
            Diag d = {0};
            d.message = diag_fmt("append は 1 個の引数を取りますが、%d 個渡されました",
                                 nargs);
            d.primary.tok = n->tok;
            d.primary.label = "引数の個数が違います";
            d.hint = "xs.append(値) の形で使ってください";
            diag_fail(&d);
        }

        s->expected = ot->elem;
        Type *at = check_expr(s, n->args);
        s->expected = NULL;

        // ⚠️ ここでも type_equal()。list[list[int]] に list[str] を
        //    append するのを弾くには、要素型の再帰比較が要ります。
        if (!type_equal(at, ot->elem)) {
            Diag d = {0};
            d.message = diag_fmt("'%s' のリストに '%s' を追加できません",
                                 type_name(ot->elem), type_name(at));
            d.primary.tok = n->args->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
            diag_fail(&d);
        }
        return ty_none;
    }

    Diag d = {0};
    d.message = diag_fmt("型 '%s' にメソッド '%s' はありません", type_name(ot),
                         n->name);
    d.primary.tok = n->tok;
    d.primary.label = "このメソッドは存在しません";
    d.hint = "組み込みの型で使えるのは list[T] の append だけです"
             "（class のメソッドは自分で定義できます）";
    diag_fail(&d);
}

// インスタンス生成 Token(1, "x") の検査（第12章）。
//
// ★ 構文上はただの関数呼び出し（ND_CALL）です。名前解決の段階で分岐します。
//   「どう扱うか」の判断をここで終わらせ、codegen には n->cls という
//   記録を残すだけ。第9章の n->builtin とまったく同じ形です。
static Type *check_new(Sema *s, Node *n, Class *c) {
    n->cls = c;  // ★ codegen はこれを見て「生成」だと分かる

    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;

    // init が無いクラスは、引数なしでしか作れない
    if (!c->has_init) {
        if (nargs != 0) {
            Diag d = {0};
            d.message = diag_fmt("クラス '%s' には init が無いので引数を渡せません",
                                 c->name);
            d.primary.tok = n->tok;
            d.primary.label = diag_fmt("%d 個の引数が渡されています", nargs);
            d.related.tok = c->tok;
            d.related.label = "クラスの定義はここです";
            d.hint = "引数を受け取るには init メソッドを定義してください:\n"
                     "             def init(self, ...) -> None:";
            diag_fail(&d);
        }
        return c->type;
    }

    // init があるなら、その引数と突き合わせる（self は飛ばす）
    FuncSig *f = lookup_func(s, mangle(c->name, "init"));
    if (nargs != f->nparams - 1) {
        Diag d = {0};
        d.message = diag_fmt("'%s' の生成には %d 個の引数が必要ですが、%d 個渡されました",
                             c->name, f->nparams - 1, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "init はここで定義されています";
        d.hint = "self は自動的に渡されるので、書く必要はありません";
        diag_fail(&d);
    }

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        s->expected = f->params[i + 1];
        Type *at = check_expr(s, a);
        s->expected = NULL;
        if (!type_equal(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = diag_fmt("'%s' の生成の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 c->name, i + 1, type_name(at),
                                 type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i + 1],
                                       type_name(f->params[i + 1]));
            d.hint = "Mython には暗黙の型変換がありません（言語仕様 3.5）";
            diag_fail(&d);
        }
    }
    return c->type;
}

// 関数呼び出しの検査（docs/spec/type-system.md 5.7 の順序に従う）
static Type *check_call(Sema *s, Node *n) {
    if (is_builtin_name(n->name)) return check_builtin_call(s, n);

    // ★ 第12章：名前がクラスなら、これは呼び出しではなくインスタンス生成
    Class *cls = lookup_class(s, n->name);
    if (cls) return check_new(s, n, cls);

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

    s->expected = want;  // ★ return [] のため（第10章）
    Type *got = check_expr(s, n->lhs);
    s->expected = NULL;
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
            if (n->incr) check_stmt(s, n->incr);  // for の増分（第11章）
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

// ★ 第12章：登録が 3 段に分かれます。
//
//     1a  クラス名と Type だけ登録する      ← クラスどうしの相互参照のため
//     1b  フィールドとメソッドを解決する      ← 型注釈に他のクラスを書ける
//     1c  トップレベルの関数・グローバル変数   ← 引数の型にクラスを書ける
//
//   関数の前方参照（第8章）と同じ問題を、同じ手（先に名前だけ登録）で解いています。

// 1a：クラス名と Type を作る。中身はまだ見ない。
static void declare_class(Sema *s, Node *n) {
    if (type_from_name(n->name))
        error_at_hint(n->tok, diag_fmt("'%s' は組み込みの型名です", n->name),
                      "クラス名 '%s' は使えません", n->name);
    if (is_builtin_name(n->name))
        error_at_hint(n->tok, diag_fmt("'%s' は組み込み関数の名前です", n->name),
                      "クラス名 '%s' は使えません", n->name);

    Class *prev = lookup_class(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("クラス '%s' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再定義されています";
        d.related.tok = prev->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }

    Class *c = xmalloc(sizeof(Class));
    c->name = n->name;
    c->tok = n->tok;
    c->node = n;
    c->type = type_class(n->name, c);  // ★ クラスにつき Type は 1 個だけ
    c->next = s->classes;
    s->classes = c;

    n->cls = c;
    n->type = c->type;
}

// フィールドを並べて、オフセットとサイズを決める。
//
// ★ docs/design/memory-model.md 5 節の表がそのまま実装になっています。
//   ⚠️ 読み書きに offset は使いません（getelementptr に渡すのは index）。
//      offset は「自分の計算が合っているか」を確かめるための値です。
static int align_up(int offset, int align) {
    return (offset + align - 1) / align * align;
}

static void layout_class(Class *c) {
    int offset = 0, max_align = 1, index = 0;
    for (Field *f = c->fields; f; f = f->next) {
        int a = type_align(f->type);
        offset = align_up(offset, a);  // ★ パディングはここで入る
        f->offset = offset;
        f->index = index++;
        offset += type_size(f->type);
        if (a > max_align) max_align = a;
    }
    c->nfields = index;
    c->align = max_align;
    c->size = align_up(offset, max_align);  // 全体もアラインメントに切り上げる
}

// メソッドを FuncSig として登録する。名前は "Token.show"（名前修飾）。
static void declare_method(Sema *s, Class *c, Node *fn) {
    char *mname = mangle(c->name, fn->name);

    FuncSig *prev = lookup_func(s, mname);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("メソッド '%s' は既に定義されています", mname);
        d.primary.tok = fn->tok;
        d.primary.label = "ここで再定義されています";
        d.related.tok = prev->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }

    Field *clash = lookup_field(c, fn->name);
    if (clash) {
        Diag d = {0};
        d.message = diag_fmt("'%s' はフィールドと同じ名前です", fn->name);
        d.primary.tok = fn->tok;
        d.primary.label = "メソッド名がフィールド名と衝突しています";
        d.related.tok = clash->tok;
        d.related.label = "同名のフィールドはここです";
        d.hint = "t.f が「フィールド」か「メソッド」か決められなくなるため禁止です";
        diag_fail(&d);
    }

    Type *ret = resolve_type(s, fn->type_ref);

    int nparams = 0;
    for (Node *pm = fn->params; pm; pm = pm->next) nparams++;

    FuncSig *f = xmalloc(sizeof(FuncSig));
    f->name = mname;
    f->ret = ret;
    f->nparams = nparams;
    f->params = xmalloc(sizeof(Type *) * (size_t)nparams);
    f->pnames = xmalloc(sizeof(char *) * (size_t)nparams);
    f->tok = fn->tok;

    int i = 0;
    for (Node *pm = fn->params; pm; pm = pm->next, i++) {
        // ★ 第 1 引数 self には型注釈がありません（parser が保証している）。
        //   そのクラスの型をここで入れます。これが「self の暗黙の型」です。
        Type *pt = pm->type_ref ? resolve_type(s, pm->type_ref) : c->type;
        if (pt->kind == TY_NONE)
            error_at_hint(pm->tok, "None 型の値は存在しないので引数にできません",
                          "引数の型に None は使えません");
        f->params[i] = pt;
        f->pnames[i] = pm->name;
        pm->type = pt;
    }

    // コンストラクタ init は値を返せない（生成した自分自身が返るため）
    if (strcmp(fn->name, "init") == 0) {
        if (ret->kind != TY_NONE)
            error_at_hint(fn->tok,
                          "init は戻り値を持てません（-> None と書いてください）",
                          "init の戻り型は None でなければなりません");
        c->has_init = true;
    }

    f->next = s->funcs;
    s->funcs = f;

    fn->ir_name = mname;  // ★ codegen が define する関数名（@Token.show）
    fn->type = ret;
}

// 1b：フィールドとメソッドを解決する。
static void declare_class_members(Sema *s, Node *n) {
    Class *c = n->cls;

    // ① フィールド（宣言順にリストの末尾へ足す。並び順がレイアウトになる）
    Field tail = {0};
    Field *cur = &tail;
    for (Node *m = n->body; m; m = m->next) {
        if (m->kind != ND_FIELDDECL) continue;

        Field *prev = lookup_field(c, m->name);
        if (prev) {
            Diag d = {0};
            d.message = diag_fmt("フィールド '%s' は既に宣言されています", m->name);
            d.primary.tok = m->tok;
            d.primary.label = "ここで再宣言されています";
            d.related.tok = prev->tok;
            d.related.label = "最初の宣言はここです";
            diag_fail(&d);
        }

        Type *ft = resolve_type(s, m->type_ref);
        if (ft->kind == TY_NONE)
            error_at_hint(m->tok, "None 型の値は存在しないのでフィールドにできません",
                          "フィールドの型に None は使えません");

        Field *f = xmalloc(sizeof(Field));
        f->name = m->name;
        f->type = ft;
        f->tok = m->tok;
        cur->next = f;
        cur = f;
        c->fields = tail.next;  // ★ lookup_field を回すために毎回つなぎ直す
        m->type = ft;
    }
    c->fields = tail.next;
    layout_class(c);

    // ② メソッド
    for (Node *m = n->body; m; m = m->next)
        if (m->kind == ND_FUNC) declare_method(s, c, m);
}

static void declare_func(Sema *s, Node *n) {
    if (lookup_class(s, n->name)) {
        Diag d = {0};
        d.message = diag_fmt("'%s' はクラス名として使われています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この名前の関数は定義できません";
        d.related.tok = lookup_class(s, n->name)->tok;
        d.related.label = "クラスの定義はここです";
        d.hint = "クラス名は「インスタンス生成」の呼び出しに使われます"
                 "（例: Token(1, \"x\")）";
        diag_fail(&d);
    }
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

    Type *ret = resolve_type(s, n->type_ref);

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
        Type *pt = resolve_type(s, pm->type_ref);
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
    Type *declared = resolve_type(s, n->type_ref);
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
    // ★ 第12章：メソッドは修飾名（Token.show）で表に載っています。
    //   ir_name があればそれが表の鍵。無ければ今までどおり関数名。
    s->cur_func = lookup_func(s, n->ir_name ? n->ir_name : n->name);
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

    // ── パス 1：宣言を先に全部登録する ──
    // ★ 本体を見る前に登録するので、前方参照も再帰も自然に通ります。
    //   C がプロトタイプ宣言を要求するのは、この 2 パスを人間にやらせているからです。

    // 1a：クラス名だけ先に登録する（クラスどうしが互いを参照できるように）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_CLASS) declare_class(&s, d);

    // 1b：フィールドとメソッド（型注釈に他のクラスを書ける）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_CLASS) declare_class_members(&s, d);

    // 1c：トップレベルの関数とグローバル変数（引数の型にクラスを書ける）
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) declare_func(&s, d);
        else if (d->kind == ND_VARDECL) declare_global(&s, d);
        else if (d->kind == ND_CLASS) continue;  // 1a / 1b で済んでいる
        else UNREACHABLE();  // parser が保証している
    }

    // ── パス 2：本体を検査する ──
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) check_func(&s, d);
        // メソッドの本体も、ふつうの関数とまったく同じ手順で検査します。
        // self はもう「型が入った引数」なので、特別扱いは 1 つも要りません。
        else if (d->kind == ND_CLASS)
            for (Node *m = d->body; m; m = m->next)
                if (m->kind == ND_FUNC) check_func(&s, m);
    }

    check_main(&s, ast);
    scope_pop(&s);
}
