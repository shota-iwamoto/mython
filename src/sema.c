#include "sema.h"

#include "module.h"

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

    // ★ 第15章：型が 2 つになりました。
    //   declared … 宣言された型（Token | None）。代入できるかはこれで判定する
    //   type     … 今の型。絞り込まれていれば Token になっている
    Type *declared;
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
typedef struct ModuleSyms ModuleSyms;

typedef struct FuncSig FuncSig;
struct FuncSig {
    char *name;      // 表を引く鍵（"add" / "Token.show"。★ モジュール内で一意）
    char *ir_name;   // IR 上の名前（第13章。"lexer.add" / "lexer.Token.show"）
    Type *ret;
    Type **params;  // 引数の型
    char **pnames;  // 引数名（エラーメッセージ用）
    int nparams;
    Token *tok;     // 定義位置（「この関数はここで定義されています」用）
    ModuleSyms *owner;  // どのモジュールのものか（第13章）
    FuncSig *next;
};

// ── モジュールごとのシンボル表（第13章）──────────────────
//
// ★ 第12章までは「表が 1 本ずつ」でした。モジュールが増えると
//   「モジュールごとに 1 本ずつ」になります。名前空間とはこれのことです。
//   他のモジュールの中身は、必ず修飾（lexer.Token）を通してしか見えません。
struct ModuleSyms {
    Module *mod;
    FuncSig *funcs;    // トップレベル関数とメソッド
    Class *classes;
    Scope *globals;    // グローバル変数のスコープ
    ModuleSyms *next;
};

typedef struct {
    Scope *scope;      // 現在のスコープ
    int loop_depth;    // 今いるループの深さ（break / continue の検査用）
    UsedName *used;    // 割り当て済みの IR 名（関数ごとにリセット）
    FuncSig *funcs;    // 関数表（第8章。メソッドも "Token.show" として入る）
                       // ★ 第13章：これは「今検査中のモジュールの」表です
    FuncSig *cur_func; // 今どの関数を検査中か（return の検査に必要）
    Class *classes;    // クラス表（第12章。同上、モジュールごと）

    // ── 第13章：モジュール ──
    ModuleSyms *cur;   // 今検査中のモジュール
    ModuleSyms *mods;  // 全モジュール（依存順）

    // 今この式に期待されている型（第10章）。
    //
    // ★ 空リスト [] だけは、それ自身から要素型が決まりません。
    //   本格的なやり方は双方向型検査（期待型を引数で渡す）ですが、
    //   v1 で期待型を必要とする式は [] だけなので、状態を 1 つ持たせて済ませます。
    // ⚠️ 期待型が要る式が増えたら、この手は破綻します。そのときは引数で渡す形に直します。
    Type *expected;
} Sema;

// ── モジュールの出入り（第13章）────────────────────────────
//
// ★ 「今どのモジュールを検査中か」を切り替えるだけの関数です。
//   表そのものは Sema に置いたまま（第12章までのコードが 1 行も変わらない）、
//   切り替えるときに ModuleSyms へ書き戻します。
static void enter_module(Sema *s, ModuleSyms *ms) {
    if (s->cur) {  // 今のモジュールの状態を保存する
        s->cur->funcs = s->funcs;
        s->cur->classes = s->classes;
        s->cur->globals = s->scope;
    }
    s->cur = ms;
    s->funcs = ms->funcs;
    s->classes = ms->classes;
    s->scope = ms->globals;
}

// import しているモジュールを名前で引く。
// ⚠️ import していないモジュールは、たとえ読み込まれていても見えません。
static ModuleSyms *lookup_import(Sema *s, const char *name) {
    Module *m = s->cur->mod;
    for (int i = 0; i < m->ndeps; i++)
        if (strcmp(m->deps[i]->name, name) == 0) return m->deps[i]->syms;
    return NULL;
}

// IR 上の修飾名を作る（第12章の mangle をモジュールにも使う）
static char *mangle(const char *prefix, const char *name);

static char *mod_mangle(Sema *s, const char *name) {
    return mangle(s->cur->mod->name, name);
}

static FuncSig *lookup_func_in(ModuleSyms *ms, const char *name) {
    for (FuncSig *f = ms->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static Class *lookup_class_in(ModuleSyms *ms, const char *name) {
    for (Class *c = ms->classes; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static VarEntry *lookup_global_in(ModuleSyms *ms, const char *name) {
    for (VarEntry *v = ms->globals->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

static FuncSig *lookup_func(Sema *s, const char *name) {
    for (FuncSig *f = s->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

// IR 名（モジュール修飾済み）で引く。
// ★ 第13章：表の鍵は「モジュール内で一意な名前」（add / Token.show）ですが、
//   検査中の関数を特定するときは、定義ノードに入っている IR 名から引くのが
//   確実です（関数もメソッドも同じ 1 行で済む）。
static FuncSig *lookup_func_by_ir(Sema *s, const char *ir_name) {
    for (FuncSig *f = s->funcs; f; f = f->next)
        if (strcmp(f->ir_name, ir_name) == 0) return f;
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
    // ⚠️ "entry" は予約する（第18章）。
    //
    //   LLVM ではラベルとローカル値が同じ名前空間にいます。関数の先頭は
    //   慣習的に `entry:` なので、利用者が `entry` という変数を書くと
    //   `%entry = alloca` と衝突して IR が壊れます（stage1 の移植で踏んだ）。
    //
    //   ★ コンパイラが作る名前は '.' を含めて衝突を避ける約束ですが
    //     （%t.0 / if.then.0 / for.ix.0）、`entry` だけは慣習を優先して
    //     '.' を入れていません。その代わりここで予約します。
    if (strcmp(name, "entry") == 0) return true;

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

// ★ 第13章：モジュール名と同じ名前は宣言できない（13.5 節）。
//
//   import lexer
//   lexer: int = 1      ← エラー
//
// 「lexer.x」の lexer が変数かモジュールか、読む人が迷うからです。
// コンパイラは「変数優先」と決めれば動きますが、それは規則を覚える負担を
// 利用者に押しつけることになります。第7章のシャドーイング禁止と同じ判断です。
static void reject_module_name(Sema *s, const char *name, Token *tok,
                               const char *what) {
    ModuleSyms *ms = lookup_import(s, name);
    if (!ms) return;

    Diag d = {0};
    d.message = diag_fmt("'%s' は import したモジュールの名前です", name);
    d.primary.tok = tok;
    d.primary.label = diag_fmt("この名前の%sは宣言できません", what);
    d.hint = diag_fmt("モジュール名と同じ名前を使うと '%s.x' が曖昧になります",
                      name);
    diag_fail(&d);
}

// ローカル変数として登録する（IR 名は %x 形式）
static VarEntry *declare(Sema *s, char *name, Type *type, Token *tok) {
    reject_module_name(s, name, tok, "変数");

    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%%%s", unique_ir_name(s, name));

    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = name;
    v->ir_name = sb_str(&sb);
    v->declared = type;
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
static Type *resolve_base_type(Sema *s, Node *tr);

// ★ 第15章：T | None を包む層。
//   nullable にできるのは参照型（str / list / class）だけです。
static Type *resolve_type(Sema *s, Node *tr) {
    Type *base = resolve_base_type(s, tr);
    if (!tr->nullable) return base;

    if (!type_can_be_opt(base)) {
        Diag d = {0};
        d.message = diag_fmt("'%s | None' は書けません", type_name(base));
        d.primary.tok = tr->tok;
        d.primary.label = "この型は None になれません";
        d.hint = "None はヌルポインタとして表すので、int や bool には付けられません"
                 "（nullable にできるのは str / list[T] / class です）";
        diag_fail(&d);
    }
    return type_opt(base);
}

static Type *resolve_base_type(Sema *s, Node *tr) {
    // ★ 第13章：lexer.Token のようにモジュール修飾された型注釈。
    //   引く表が「自分のモジュール」から「そのモジュール」に変わるだけです。
    if (tr->mod_name) {
        ModuleSyms *ms = lookup_import(s, tr->mod_name);
        if (!ms) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' を import していません", tr->mod_name);
            d.primary.tok = tr->tok;
            d.primary.label = "この修飾を解決できません";
            d.hint = diag_fmt("ファイルの先頭に 'import %s' を書いてください",
                              tr->mod_name);
            diag_fail(&d);
        }
        Class *c = lookup_class_in(ms, tr->name);
        if (!c) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' にクラス '%s' はありません",
                                 tr->mod_name, tr->name);
            d.primary.tok = tr->tok;
            d.primary.label = "このクラスは定義されていません";
            d.hint = "他のモジュールから使えるのはクラスだけです"
                     "（int や list はモジュール修飾なしで書きます）";
            diag_fail(&d);
        }
        if (tr->lhs)
            error_at_hint(tr->tok, "要素型を取るのは list だけです",
                          "型 '%s.%s' は要素型を取りません", tr->mod_name, tr->name);
        return c->type;
    }

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

// ★ 第13章：同名の別クラスだったときは、モジュール修飾つきで説明する。
//
//   x: a.Box = b.Box()     →  「型 'Box' の式」だけでは何が起きたか分からない
//
// 型が同じかどうかは名前ではなく定義で決まります（第12章の判断）。
// その判断の結果を、利用者に読める言葉で見せるための一言です。
static const char *no_implicit_hint(Type *got, Type *want) {
    if (got->kind == TY_CLASS && want->kind == TY_CLASS &&
        strcmp(got->cls->name, want->cls->name) == 0)
        return diag_fmt("'%s' と '%s' は名前が同じだけの別のクラスです"
                        "（同じ型かどうかは名前ではなく定義で決まります）",
                        got->cls->ir_name, want->cls->ir_name);
    return "Mython には暗黙の型変換がありません（言語仕様 3.5）";
}

// ── None リテラルと is / is not（第15章）──────────────────

// x is None / x is not None の検査。
//
// ★ 右辺は None リテラルだけを許します。一般の同一性比較にしないのは、
//   クラスの == が既に参照比較だからです（区別を説明できない記号は増やさない）。
static Type *check_is(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);

    if (n->rhs->kind != ND_NONE) {
        Type *r = check_expr(s, n->rhs);
        Diag d = {0};
        d.message = diag_fmt("%s は None との比較にだけ使えます", op_symbol(n->op));
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("ここには None を書いてください（型 '%s' の式です）",
                                   type_name(r));
        d.hint = "値が等しいかを調べるには == を使ってください";
        diag_fail(&d);
    }
    n->rhs->type = ty_null;

    if (l->kind != TY_OPT) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' の値が None になることはありません",
                             type_name(l));
        d.primary.tok = n->lhs->tok;
        d.primary.label = "この式は必ず値を持ちます";
        d.hint = type_can_be_opt(l)
                     ? diag_fmt("None を入れたいなら、型注釈を '%s | None' に"
                                "してください", type_name(l))
                     : "None になれるのは str / list[T] / class だけです";
        diag_fail(&d);
    }
    return ty_bool;
}

// 二項演算子が、その型に適用できるか
static bool op_supports(OpKind op, Type *t) {
    // ★ 第12章：クラスと list は「参照」なので、比べられるのは
    //   同一性（== / !=）だけです。大小関係には意味がありません
    //   （言語仕様 4.3 / docs/spec/type-system.md 5.6）。
    if (t->kind == TY_CLASS || t->kind == TY_LIST)
        return op == OP_EQ || op == OP_NE;

    // ★ 第15章：T | None には何も適用できません。
    //   == で比べたいなら、先に絞り込んでもらいます（None かどうかは is で調べる）。
    if (t->kind == TY_OPT || t->kind == TY_NULL) return false;

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
    // ★ 第15章：is / is not は型の合わせ方がまったく違うので、先に分岐します
    if (n->op == OP_IS || n->op == OP_ISNOT) return check_is(s, n);

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
        d.hint = no_implicit_hint(l, r);
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
        // ★ 第13章：モジュール名そのものは値ではありません。
        if (lookup_import(s, n->name)) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' は値として使えません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = "ここにはモジュール名を書けません";
            d.hint = diag_fmt("モジュールの中身は '%s.名前' の形で使います", n->name);
            diag_fail(&d);
        }

        // 同名のモジュールが存在するのに import していない場合。
        // ★ 「未定義の名前です」で突き放さず、書き忘れを指摘します。
        if (module_file_exists(s->cur->mod->dir, n->name)) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' を import していません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = "このモジュールはここからは見えません";
            d.hint = diag_fmt("ファイルの先頭に 'import %s' を書いてください",
                              n->name);
            diag_fail(&d);
        }

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
        case ND_NONE: t = ty_null; break;  // 第15章：ヌルポインタという「値」
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

    if (!type_assignable(actual, declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label =
            diag_fmt("変数 '%s' は '%s' 型として宣言されています", n->name,
                     type_name(declared));
        d.hint = no_implicit_hint(actual, declared);
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

        if (!type_assignable(actual, et)) {
            Diag d = {0};
            d.message = "型が一致しません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
            d.related.tok = target->tok;
            d.related.label = diag_fmt("この要素は '%s' 型です", type_name(et));
            d.hint = no_implicit_hint(actual, et);
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

        if (!type_assignable(actual, ft)) {
            Diag d = {0};
            d.message = "型が一致しません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
            d.related.tok = target->field->tok;
            d.related.label = diag_fmt("フィールド '%s' は '%s' 型として宣言されています",
                                       target->field->name, type_name(ft));
            d.hint = no_implicit_hint(actual, ft);
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

    // ★ 第15章：代入できるかは「宣言された型」で判定します。
    //   絞り込みで一時的に狭くなっていても、代入できる範囲は変わりません。
    s->expected = v->declared;  // ★ xs = [] のため（第10章）
    Type *actual = check_expr(s, n->rhs);
    s->expected = NULL;
    if (!type_assignable(actual, v->declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = v->decl_tok;
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   v->name, type_name(v->declared));
        d.hint = no_implicit_hint(actual, v->declared);
        diag_fail(&d);
    }

    // ★ 代入したら絞り込みは解除する（15.5 節）。
    //   cur = cur.next のあと、cur はまた None かもしれないからです。
    //   本格的なフロー解析の代わりに「代入したら忘れる」という
    //   保守的な近似で済ませています。
    v->type = v->declared;
    target->type = v->declared;
    n->type = v->declared;
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
// ── 絞り込み（narrowing。第15章）──────────────────────────
//
// ★ 第5章からの「変数の型は 1 つ」という前提を、ここだけ崩します。
//
//     t: Token | None = find()
//     if t is not None:
//         print(t.kind)     ← この中でだけ t は Token
//
// 実装は「入る前に変えて、抜けたら戻す」だけです。第7章のスコープと同じ形で、
// C の呼び出しスタックがそのまま絞り込みのスタックになります。
//
// ⚠️ 絞れるのはローカル変数だけです（15.5 節）。
//   ・グローバル変数 … 呼んだ関数の中で書き換えられるかもしれない
//   ・フィールド     … node.next を絞ると「その間 node.next が変わらないこと」を
//                      保証しなければならない。メソッド呼び出し 1 つで壊れる

#define NARROW_MAX 16

typedef struct {
    VarEntry *vars[NARROW_MAX];
    Type *saved[NARROW_MAX];
    int n;
} NarrowSet;

static void narrow_one(Sema *s, Node *var_node, NarrowSet *ns) {
    if (var_node->kind != ND_VAR) return;

    VarEntry *v = lookup(s, var_node->name);
    if (!v || v->is_global) return;   // グローバルは絞らない
    if (v->type->kind != TY_OPT) return;
    if (ns->n >= NARROW_MAX) return;  // 深すぎる条件は諦める（保守的でよい）

    ns->vars[ns->n] = v;
    ns->saved[ns->n] = v->type;
    ns->n++;
    v->type = v->type->elem;  // ★ ここだけ Token になる
}

// 条件式から「絞り込める変数」を集めて適用する。
//   positive = true  … 条件が成り立つ側（then / while の本体）
//   positive = false … 成り立たない側（else）
static void narrow_apply(Sema *s, Node *cond, bool positive, NarrowSet *ns) {
    if (!cond) return;

    if (cond->kind == ND_BINOP && cond->op == OP_ISNOT && positive)
        narrow_one(s, cond->lhs, ns);
    else if (cond->kind == ND_BINOP && cond->op == OP_IS && !positive)
        narrow_one(s, cond->lhs, ns);
    else if (cond->kind == ND_LOGICAL && cond->op == OP_AND && positive) {
        // a is not None and b is not None → 両方絞れる
        // ⚠️ or は絞れません（どちらか一方しか保証されない）
        narrow_apply(s, cond->lhs, true, ns);
        narrow_apply(s, cond->rhs, true, ns);
    }
}

static void narrow_restore(NarrowSet *ns) {
    // ⚠️ 必ず戻します。戻し忘れると、if の外でも絞られたままになります。
    for (int i = 0; i < ns->n; i++) ns->vars[i]->type = ns->saved[i];
    ns->n = 0;
}

static bool always_returns(Node *n);

// 文の並びを順に検査する（スコープは呼び出し側が用意する）。
//
// ★ 第15章：ガード節による絞り込みをここに入れます。
//
//     if b is None:
//         return 0          ← ここで必ず抜ける
//     return b.v            ← だから、この先の b は None ではない
//
// 「その if の中で必ず return するなら、その後ろでは条件の反対側が
//   成り立っている」だけの判断です。到達可能性の検査（第8章の
//   always_returns）を、そのまま絞り込みに再利用しています。
//
// ⚠️ 関数本体もブロックも同じ関数を通します。片方だけに入れると、
//    「関数の直下では効くのに if の中では効かない」という説明できない差が出ます。
static void check_stmt_list(Sema *s, Node *first) {
    NarrowSet guard = {0};

    for (Node *st = first; st; st = st->next) {
        check_stmt(s, st);

        if (st->kind == ND_IF && !st->els && always_returns(st->body))
            narrow_apply(s, st->lhs, false, &guard);
    }

    narrow_restore(&guard);
}

static void check_block(Sema *s, Node *n) {
    scope_push(s);
    check_stmt_list(s, n->body);
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

    // 要素があるなら、最初の要素の型を要素型にする（推論はしない）。
    //
    // ★ 第15章：期待型があるなら、そちらを要素型に使います。
    //   [Box(1), None] は最初の要素だけ見ると list[Box] になってしまい、
    //   2 つ目の None が入りません。宣言に list[Box | None] と書いてあるなら
    //   それに従うのが素直です（第10章の「空リストの期待型」の延長）。
    s->expected = want && want->kind == TY_LIST ? want->elem : NULL;
    Type *first = check_expr(s, n->body);
    Type *et = first;
    if (want && want->kind == TY_LIST && type_assignable(first, want->elem))
        et = want->elem;

    int i = 2;
    for (Node *el = n->body->next; el; el = el->next, i++) {
        s->expected = et;
        Type *t = check_expr(s, el);
        if (!type_assignable(t, et)) {
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
// 「'.' の左がモジュールか」を判定する（第13章。13.5 節の名前解決の順序）。
//
// ★ 変数が先、モジュールは最後。ただしモジュール名と同じ名前の変数は
//   宣言できない（declare_* で弾く）ので、実際には競合しません。
//   それでも順序を実装の順序としてそのまま書いておきます。
static ModuleSyms *dot_module(Sema *s, Node *n) {
    if (n->lhs->kind != ND_VAR) return NULL;
    if (lookup(s, n->lhs->name)) return NULL;
    return lookup_import(s, n->lhs->name);
}

// lexer.MAX_KIND — 他のモジュールのグローバル変数
static Type *check_module_global(Sema *s, Node *n, ModuleSyms *ms) {
    VarEntry *v = lookup_global_in(ms, n->name);
    if (!v) {
        Diag d = {0};
        d.message = diag_fmt("モジュール '%s' に '%s' はありません", ms->mod->name,
                             n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この名前は定義されていません";
        if (lookup_func_in(ms, n->name))
            d.hint = diag_fmt("'%s' は関数です。'%s.%s(...)' と呼んでください",
                              n->name, ms->mod->name, n->name);
        else if (lookup_class_in(ms, n->name))
            d.hint = diag_fmt("'%s' はクラスです。生成するには '%s.%s(...)' と"
                              "書いてください",
                              n->name, ms->mod->name, n->name);
        else
            d.hint = "モジュールから使えるのは、そのファイルのトップレベルの"
                     "関数・クラス・グローバル変数です";
        diag_fail(&d);
    }

    // ★ codegen への記録。ND_FIELD のままだが「グローバル変数の読み書き」になる。
    n->mod_name = ms->mod->name;
    n->ir_name = v->ir_name;
    n->is_global = true;
    n->is_extern = ms != s->cur;
    return v->type;
}

// ★ 第15章：T | None に '.' で触ろうとしたときの案内。
//   ここが narrowing の入口になる、いちばんよく出るエラーです。
static _Noreturn void reject_opt_access(Node *obj, Node *at, const char *what,
                                        Type *ot) {
    Diag d = {0};
    d.message = diag_fmt("型 '%s' の値には%sがありません", type_name(ot), what);
    d.primary.tok = at->tok;
    d.primary.label = "None かもしれない値です";
    d.related.tok = obj->tok;
    d.related.label = diag_fmt("この式は '%s' 型です", type_name(ot));
    if (obj->kind == ND_VAR)
        d.hint = diag_fmt("先に None を除いてください:\n"
                          "             if %s is not None:\n"
                          "                 ...",
                          obj->name);
    else
        d.hint = "一度ローカル変数に入れてから絞り込んでください:\n"
                 "             x: T | None = ...\n"
                 "             if x is not None:\n"
                 "                 ...";
    diag_fail(&d);
}

static Type *check_field(Sema *s, Node *n) {
    ModuleSyms *ms = dot_module(s, n);
    if (ms) return check_module_global(s, n, ms);

    Type *ot = check_expr(s, n->lhs);

    if (ot->kind == TY_OPT) reject_opt_access(n->lhs, n, "フィールド", ot);

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
    // ★ 第13章：メソッドは「クラスが定義されたモジュール」の表にいます。
    //   自分のモジュールの表を引くと、import したクラスのメソッドが見つかりません。
    char *mname = mangle(c->name, n->name);
    FuncSig *f = lookup_func_in(c->owner, mname);
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
        if (!type_assignable(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = diag_fmt("メソッド '%s' の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 mname, i + 1, type_name(at),
                                 type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i + 1],
                                       type_name(f->params[i + 1]));
            d.hint = no_implicit_hint(at, f->params[i + 1]);
            diag_fail(&d);
        }
    }

    // ★ codegen が呼ぶ関数名（@lexer.Token.show）。
    //   import したクラスのメソッドなら declare も要る（第13章）。
    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;
    return f->ret;
}

// メソッド呼び出しの検査（第10章の list.append と、第12章のクラスのメソッド）
// lexer.make(1, "x") / lexer.Token(1, "x") — 他のモジュールの関数・クラス
static Type *check_new(Sema *s, Node *n, Class *c);
static Type *check_call_sig(Sema *s, Node *n, FuncSig *f, const char *what);

static Type *check_module_call(Sema *s, Node *n, ModuleSyms *ms) {
    n->mod_name = ms->mod->name;

    // ★ 名前がクラスなら、これはインスタンス生成（第12章の分岐がそのまま）
    Class *c = lookup_class_in(ms, n->name);
    if (c) {
        n->is_extern = ms != s->cur;  // codegen が init を declare する判断
        return check_new(s, n, c);
    }

    FuncSig *f = lookup_func_in(ms, n->name);
    if (!f) {
        Diag d = {0};
        d.message = diag_fmt("モジュール '%s' に関数 '%s' はありません",
                             ms->mod->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この関数は定義されていません";
        d.hint = lookup_global_in(ms, n->name)
                     ? diag_fmt("'%s' はグローバル変数です。'()' を外してください",
                                n->name)
                     : "そのモジュールのトップレベルに def があるか確認してください";
        diag_fail(&d);
    }

    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;
    return check_call_sig(s, n, f, "関数");
}

static Type *check_method(Sema *s, Node *n) {
    ModuleSyms *ms = dot_module(s, n);
    if (ms) return check_module_call(s, n, ms);

    Type *ot = check_expr(s, n->lhs);

    if (ot->kind == TY_OPT) reject_opt_access(n->lhs, n, "メソッド", ot);

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

        // ⚠️ ここでも代入互換の検査。list[list[int]] に list[str] を
        //    append するのを弾くには、要素型の再帰比較が要ります。
        if (!type_assignable(at, ot->elem)) {
            Diag d = {0};
            d.message = diag_fmt("'%s' のリストに '%s' を追加できません",
                                 type_name(ot->elem), type_name(at));
            d.primary.tok = n->args->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.hint = no_implicit_hint(at, ot->elem);
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
    FuncSig *f = lookup_func_in(c->owner, mangle(c->name, "init"));
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
        if (!type_assignable(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = diag_fmt("'%s' の生成の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 c->name, i + 1, type_name(at),
                                 type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i + 1],
                                       type_name(f->params[i + 1]));
            d.hint = no_implicit_hint(at, f->params[i + 1]);
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

    // ★ 第13章：呼ぶ相手の IR 名（モジュール修飾済み）を codegen に渡す
    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;

    // ③④ 引数の個数と型
    return check_call_sig(s, n, f, "関数");
}

// 呼び出しの引数を FuncSig と突き合わせる（第8章の ③④）。
//
// ★ 第13章：モジュール修飾の呼び出し（lexer.make(1)）でも同じ検査が要るので、
//   関数に切り出しました。呼ぶ側が変わっても、検査は 1 か所のままです。
static Type *check_call_sig(Sema *s, Node *n, FuncSig *f, const char *what) {
    const char *shown = n->mod_name ? diag_fmt("%s.%s", n->mod_name, n->name)
                                    : f->name;

    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams) {
        Diag d = {0};
        d.message = diag_fmt("%s '%s' は %d 個の引数を取りますが、%d 個渡されました",
                             what, shown, f->nparams, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "呼び出しの引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "この関数はここで定義されています";
        diag_fail(&d);
    }

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        // ⚠️ 引数には期待型を渡しません（第10章の判断のまま）。
        //    take([]) の [] は「型注釈を書いてください」というエラーになります。
        Type *at = check_expr(s, a);
        if (!type_assignable(at, f->params[i])) {
            Diag d = {0};
            d.message = diag_fmt("%s '%s' の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 what, shown, i + 1, type_name(at),
                                 type_name(f->params[i]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i],
                                       type_name(f->params[i]));
            d.hint = no_implicit_hint(at, f->params[i]);
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
    if (!type_assignable(got, want)) {
        Diag d = {0};
        d.message = "return の型が戻り型と一致しません";
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = diag_fmt("関数 '%s' の戻り型は '%s' です", s->cur_func->name,
                                   type_name(want));
        d.hint = no_implicit_hint(got, want);
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

        case ND_IF: {
            check_cond(s, "if の条件", n, n->lhs);

            // ★ 第15章：then 節では条件が成り立っている
            NarrowSet ns = {0};
            narrow_apply(s, n->lhs, true, &ns);
            check_block(s, n->body);
            narrow_restore(&ns);

            // els は ND_BLOCK（else）か ND_IF（elif の脱糖結果）。
            // else 節では条件が成り立っていない（if t is None: の反対側）。
            if (n->els) {
                NarrowSet es = {0};
                narrow_apply(s, n->lhs, false, &es);
                check_stmt(s, n->els);
                narrow_restore(&es);
            }
            break;
        }

        case ND_WHILE: {
            check_cond(s, "while の条件", n, n->lhs);
            s->loop_depth++;

            // ★ 本体に入れたということは条件が成り立っている。
            //   while cur is not None: … 連結リストの走査に必須です。
            NarrowSet ns = {0};
            narrow_apply(s, n->lhs, true, &ns);
            check_block(s, n->body);
            if (n->incr) check_stmt(s, n->incr);  // for の増分（第11章）
            narrow_restore(&ns);

            s->loop_depth--;
            break;
        }

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
    reject_module_name(s, n->name, n->tok, "クラス");
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
    c->ir_name = mod_mangle(s, n->name);  // ★ 第13章：%lexer.Token.type になる
    c->owner = s->cur;                    // メソッドはこのモジュールの表にいる
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

    f->ir_name = mangle(c->ir_name, fn->name);  // "lexer.Token.show"（第13章）
    f->owner = s->cur;
    f->next = s->funcs;
    s->funcs = f;

    fn->ir_name = f->ir_name;  // ★ codegen が define する関数名（@lexer.Token.show）
    fn->type = ret;
}

// 1b：フィールドとメソッドを解決する。
// ── 未初期化フィールドの検査（第15章。第12章からの宿題）────
//
// クラス型のフィールドは既定値を作れないので NULL から始まります（ch12 12.6）。
// 第12章ではランタイムで検査していましたが、型の側から塞ぎます。
//
// ⚠️ この検査は「構文的」です。init のどこかに self.f = ... があるかを見るだけで、
//    それが実行されるかまでは見ません（条件つきの代入はすり抜ける）。
//    だから第12章のランタイム検査（my_check_not_none）は残します。
//    静的検査で多くを早く捕まえ、残りを動的検査で安全に受け止めます。
static bool assigns_field(Node *n, const char *fname) {
    if (!n) return false;

    if (n->kind == ND_ASSIGN && n->lhs->kind == ND_FIELD &&
        n->lhs->lhs->kind == ND_VAR && strcmp(n->lhs->lhs->name, "self") == 0 &&
        strcmp(n->lhs->name, fname) == 0)
        return true;

    if (assigns_field(n->lhs, fname)) return true;
    if (assigns_field(n->rhs, fname)) return true;
    if (assigns_field(n->els, fname)) return true;
    if (assigns_field(n->incr, fname)) return true;
    for (Node *st = n->body; st; st = st->next)
        if (assigns_field(st, fname)) return true;
    return false;
}

static Node *find_init(Class *c) {
    for (Node *m = c->node->body; m; m = m->next)
        if (m->kind == ND_FUNC && strcmp(m->name, "init") == 0) return m;
    return NULL;
}

static void check_fields_initialized(Sema *s, Class *c) {
    Node *init = find_init(c);

    for (Field *f = c->fields; f; f = f->next) {
        // ★ 既定値を作れる型は対象外です（int → 0 / str → "" / list → 空 /
        //   T | None → None。どれも「有効な値」から始まります）。
        if (f->type->kind != TY_CLASS) continue;
        if (init && assigns_field(init->body, f->name)) continue;

        Diag d = {0};
        d.message = diag_fmt("フィールド '%s' は init で代入されていません", f->name);
        d.primary.tok = f->tok;
        d.primary.label = "このフィールドは None から始まってしまいます";
        if (init) {
            d.related.tok = init->tok;
            d.related.label = "init はここです";
        } else {
            d.related.tok = c->tok;
            d.related.label = "このクラスには init がありません";
        }
        d.hint = diag_fmt("次のどちらかにしてください:\n"
                          "             ・型を '%s | None' にする\n"
                          "             ・init の中で self.%s = ... と代入する",
                          type_name(f->type), f->name);
        diag_fail(&d);
    }
}

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

    // ③ 第15章：クラス型のフィールドが None から始まらないことを確かめる
    check_fields_initialized(s, c);
}

// extern の引数と戻り値に使える型か（第14章）。
//
// ⚠️ bool（i1）だけは通しません。C の _Bool との ABI が環境依存で、
//    「たまたま動く」形になりやすいためです。境界は狭く保ちます。
static void check_extern_type(Type *t, Token *tok, const char *what) {
    if (t->kind != TY_BOOL) return;
    Diag d = {0};
    d.message = diag_fmt("extern の%sに bool は使えません", what);
    d.primary.tok = tok;
    d.primary.label = "この型は C との境界を越えられません";
    d.hint = "int で受け取り、Mython 側で 'n == 1' と書いてください";
    diag_fail(&d);
}

static void declare_func(Sema *s, Node *n) {
    reject_module_name(s, n->name, n->tok, "関数");
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

    // ★ 第14章：extern は C 側で名前が決まっているので修飾しません
    //   （言語仕様 5.11）。修飾の目的は「Mython 側の名前どうしの衝突を
    //   避けること」なので、C のシンボルにはその目的が成立しません。
    bool is_extern_decl = n->body == NULL;
    if (is_extern_decl) {
        check_extern_type(ret, n->tok, "戻り値");
        for (Node *pm = n->params; pm; pm = pm->next)
            check_extern_type(pm->type, pm->tok, "引数");
    }

    f->ir_name = is_extern_decl ? n->name : mod_mangle(s, n->name);
    f->owner = s->cur;
    f->next = s->funcs;
    s->funcs = f;
    n->ir_name = f->ir_name;
    n->type = ret;
}

// グローバル変数の登録（言語仕様 6.2）
static void declare_global(Sema *s, Node *n) {
    reject_module_name(s, n->name, n->tok, "グローバル変数");
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
    if (!type_assignable(actual, declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   n->name, type_name(declared));
        diag_fail(&d);
    }

    // グローバルの IR 名は @g.<モジュール>.<名前>。
    // ★ 第13章：モジュール名を挟むことで、別ファイルの同名グローバルと
    //   リンク時に衝突しなくなります（@g. は C のシンボルとの衝突よけ。第8章）。
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "@g.%s.%s", s->cur->mod->name, n->name);

    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = n->name;
    v->ir_name = sb_str(&sb);
    v->is_global = true;
    v->declared = declared;
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
    // ★ 第12章：メソッドは修飾名で表に載っています。第13章ではそこに
    //   モジュール名も付くので、定義ノードの IR 名から引きます。
    s->cur_func = lookup_func_by_ir(s, n->ir_name);
    s->used = NULL;  // IR 名は関数ごとに振り直す（別の関数なら衝突しない）

    scope_push(s);

    // 引数をローカル変数として登録する
    for (Node *pm = n->params; pm; pm = pm->next) {
        VarEntry *v = declare(s, pm->name, pm->type, pm->tok);
        pm->ir_name = v->ir_name;
    }

    check_stmt_list(s, n->body->body);
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

// モジュール 1 つぶんの宣言を登録する（パス 1a / 1b / 1c）
static void declare_module(Sema *s, Node *ast) {
    // 1a：クラス名だけ先に登録する（クラスどうしが互いを参照できるように）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_CLASS) declare_class(s, d);

    // 1b：フィールドとメソッド（型注釈に他のクラスを書ける）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_CLASS) declare_class_members(s, d);

    // 1c：トップレベルの関数とグローバル変数（引数の型にクラスを書ける）
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) declare_func(s, d);
        else if (d->kind == ND_VARDECL) declare_global(s, d);
        else if (d->kind == ND_CLASS) continue;  // 1a / 1b で済んでいる
        else if (d->kind == ND_IMPORT) continue;  // 読み込みは module.c が済ませた
        else UNREACHABLE();  // parser が保証している
    }
}

// モジュール 1 つぶんの本体を検査する（パス 2）
static void check_module(Sema *s, Node *ast) {
    for (Node *d = ast->body; d; d = d->next) {
        // ⚠️ extern は本体を持たないので検査するものがありません（第14章）
        if (d->kind == ND_FUNC) { if (d->body) check_func(s, d); }
        // メソッドの本体も、ふつうの関数とまったく同じ手順で検査します。
        // self はもう「型が入った引数」なので、特別扱いは 1 つも要りません。
        else if (d->kind == ND_CLASS)
            for (Node *m = d->body; m; m = m->next)
                if (m->kind == ND_FUNC) check_func(s, m);
    }
}

// ★ 第13章：意味解析の単位が「1 つの AST」から「全モジュール」になりました。
//
//   パス 0   読み込みと構文解析（module.c が依存順に並べて渡してくる）
//   パス 1   モジュールごとに宣言を登録する    ← 依存順なので、
//   パス 2   モジュールごとに本体を検査する       先に登録済みのものだけを参照する
//
// 第8章（関数の前方参照）・第12章（クラスの相互参照）と同じ「先に全部登録」を、
// ファイル単位でもう 1 回やっているだけです。
void sema_program(Module *mods, Module *entry) {
    Sema s = {0};

    // 各モジュールのシンボル表を用意する（この時点では空）
    ModuleSyms *tail = NULL;
    for (Module *m = mods; m; m = m->next) {
        if (m->ast->kind != ND_BLOCK) UNREACHABLE();
        ModuleSyms *ms = xmalloc(sizeof(ModuleSyms));
        ms->mod = m;
        ms->globals = xmalloc(sizeof(Scope));
        m->syms = ms;
        if (tail) tail->next = ms;
        else s.mods = ms;
        tail = ms;
    }

    // パス 1：依存が先に並んでいるので、この順で登録すれば
    //         「他モジュールの型注釈」は必ず解決できる
    for (ModuleSyms *ms = s.mods; ms; ms = ms->next) {
        enter_module(&s, ms);
        declare_module(&s, ms->mod->ast);
    }

    // パス 2：本体
    for (ModuleSyms *ms = s.mods; ms; ms = ms->next) {
        enter_module(&s, ms);
        check_module(&s, ms->mod->ast);
    }

    // main は入口モジュールにだけ要る（他のモジュールにあっても構わない）
    enter_module(&s, entry->syms);
    check_main(&s, entry->ast);
}
