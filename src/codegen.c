#include "codegen.h"

#include "diag.h"

#include <stdio.h>

// ビルド時に Makefile が -DMYTHON_TARGET_TRIPLE=... で渡してきます。
// 万一渡されなかった場合でも動くようにフォールバックを置いておく。
#ifndef MYTHON_TARGET_TRIPLE
#define MYTHON_TARGET_TRIPLE ""
#endif

// ── 出力バッファ ────────────────────────────────────────────
// IR は「後から前に戻って書き足したい」ことがあるため、
// 用途別のバッファに分けて最後に連結します。
// 例：関数本体の生成中に文字列リテラルを見つけたら globals に追記する。
typedef struct {
    StrBuf header;   // source_filename, target triple, 型定義
    StrBuf globals;  // グローバル変数・文字列定数
    StrBuf decls;    // declare（外部関数宣言）
    StrBuf body;     // 完成した関数定義

    // ── 生成中の関数用（関数ごとにリセットする）──
    //
    // ★ なぜ alloca を別バッファにするのか（第6章）
    //   変数の alloca は AST を歩けば見つかりますが（collect_allocas）、
    //   短絡評価の結果を入れる %and.result.N は
    //   「ソースに現れない、コンパイラが自分で作る領域」です。
    //   生成してみて初めて必要だと分かるので、専用のバッファに溜めておき、
    //   最後に entry ブロックの先頭へまとめて差し込みます（規約 R1）。
    StrBuf allocas;  // entry ブロックに置く alloca
    StrBuf fn;       // 関数本体の命令列

    int tmp_counter;    // 一時値 %tN の連番
    int label_counter;  // ラベルの連番（同名ラベルの衝突を防ぐ）
    bool terminated;    // 現在の基本ブロックが終端命令を出力済みか（規約 R6）
} Emitter;

// 新しい一時値の名前を返す（"%t0", "%t1", ...）
//
// ⚠️ 規約 R4：必ず英字始まりの名前にします。
//    %0 のような数値名を自分で使うと、LLVM の暗黙採番と衝突して
//    "instruction expected to be numbered '%N'" という分かりにくい
//    エラーになります。
static char *new_tmp(Emitter *e) {
    char *buf = xmalloc(24);
    snprintf(buf, 24, "%%t%d", e->tmp_counter++);
    return buf;
}

// 値（レジスタ）としての LLVM 型。
//
// ★ 「値の型」と「メモリの型」を別の関数にするのが第6章の要点です。
//   1 つの関数で済ませようとすると、呼び出し側ごとに
//   「今はどっちの意味か」を考えることになり、必ず間違えます。
static const char *llvm_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_BOOL: return "i1";  // レジスタ上は 1 ビット
        default: UNREACHABLE();
    }
}

// メモリ（alloca / load / store）としての LLVM 型。
//
// ⚠️ 規約 R5：bool はメモリ上 i8。
//    alloca i1 も合法ですが、実際には 1 バイト確保され残り 7 ビットが未定義に
//    なります。第9章の C ランタイム連携で困るので、i8 に揃えておきます。
static const char *llvm_mem_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_BOOL: return "i8";  // メモリ上は 1 バイト
        default: UNREACHABLE();
    }
}

// 変数の「箱」の名前（alloca したポインタ）。
//
// ⚠️ シャドーイングを禁止している（言語仕様 5.1）ので、
//    変数名がそのまま一意な IR 名になります。
//    第7章でブロックスコープが入り、第8章で関数が入っても、
//    同名の変数が同時に存在しないため衝突しません。
static char *var_ptr(const char *name) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%%%s", name);
    return sb_str(&sb);
}

// 二項演算子に対応する LLVM 命令の名前を返す。
//
// ⚠️ int は符号付きなので、必ず 's' の付く命令を使います。
//    sdiv / srem / ashr（udiv / urem / lshr ではない）。
//    間違えると負数で誤った結果になります。
static const char *llvm_binop(Node *n) {
    switch (n->op) {
        case OP_ADD: return "add";
        case OP_SUB: return "sub";
        case OP_MUL: return "mul";
        case OP_FLOORDIV: return "sdiv";  // 符号付き除算
        case OP_MOD: return "srem";       // 符号付き剰余
        case OP_BITAND: return "and";
        case OP_BITOR: return "or";
        case OP_BITXOR: return "xor";
        case OP_SHL: return "shl";
        case OP_SHR: return "ashr";  // 算術シフト（符号を保つ）

        // OP_TRUEDIV はここに来ません。
        // ★ 第2章ではこの関数で弾いていましたが、第5章で意味解析パスを
        //   作ったので、本来の担当である sema.c へ移しました。
        //   コード生成器は「検査済みの正しい AST」だけを受け取る、
        //   という役割分担がここで確立します。
        default:
            UNREACHABLE();
    }
}

// 比較演算子に対応する icmp の述語。
//
// ⚠️ 落とし穴：i1 の符号付き比較は逆になる
//    i1 を 2 の補数で解釈すると True(1) は -1 です。
//    icmp slt i1 0, 1 は「0 < -1」を聞くことになり False になります。
//    そのため bool の大小比較は符号なし（ult など）を使います。
//    eq / ne には符号が無いので影響しません。
static const char *icmp_pred(OpKind op, Type *operand_type) {
    bool sign = operand_type->kind == TY_INT;  // int は符号付き
    switch (op) {
        case OP_EQ: return "eq";
        case OP_NE: return "ne";
        case OP_LT: return sign ? "slt" : "ult";
        case OP_LE: return sign ? "sle" : "ule";
        case OP_GT: return sign ? "sgt" : "ugt";
        case OP_GE: return sign ? "sge" : "uge";
        default: UNREACHABLE();
    }
}

// ── メモリとレジスタの境界（規約 R5）──────────────────────
//
// ★ zext / trunc はこの 2 つの関数の中だけに閉じ込めます。
//   他の場所には 1 つも現れません。

// メモリから読む：bool なら i8 → i1 に縮める
static char *gen_load(Emitter *e, Type *ty, const char *ptr) {
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = load %s, ptr %s\n", t, llvm_mem_type(ty), ptr);
    if (ty->kind != TY_BOOL) return t;

    char *t2 = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i8 %s to i1\n", t2, t);
    return t2;
}

// メモリへ書く：bool なら i1 → i8 に広げる
static void gen_store(Emitter *e, Type *ty, const char *val, const char *ptr) {
    if (ty->kind == TY_BOOL) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i8\n", t, val);
        val = t;
    }
    sb_printf(&e->fn, "  store %s %s, ptr %s\n", llvm_mem_type(ty), val, ptr);
}

// ── 基本ブロック（規約 R6 / R7）────────────────────────────
//
// ⚠️ IR にフォールスルーはありません。「次のブロックに続くだけ」でも
//    br label %next が必要です。初心者が最もよくハマる落とし穴です。
//
// ★ 「終端したか」を追跡する変数を 1 つ持つだけで、
//   「br を書き忘れた」も「終端の後に命令を置いた」も起きなくなります。
//   第7章の if / while、第8章の return はこの 3 つの関数の上に載ります。

// ラベルを出力する。直前のブロックが終端していなければ暗黙のジャンプを補う。
static void emit_label(Emitter *e, const char *label) {
    if (!e->terminated) sb_printf(&e->fn, "  br label %%%s\n", label);
    sb_printf(&e->fn, "%s:\n", label);
    e->terminated = false;
}

static void emit_br(Emitter *e, const char *label) {
    sb_printf(&e->fn, "  br label %%%s\n", label);
    e->terminated = true;
}

static void emit_cond_br(Emitter *e, const char *cond, const char *then_l,
                         const char *else_l) {
    sb_printf(&e->fn, "  br i1 %s, label %%%s, label %%%s\n", cond, then_l, else_l);
    e->terminated = true;
}

// ── 式の生成 ────────────────────────────────────────────────
//
// gen_expr の約束：
//   「式を評価する命令列を body に出力し、
//     結果の値が入っている場所の名前（レジスタ名 or 即値）を返す」
//
// この 1 つの約束が、コード生成器の設計全体を決めます。
// 即値（"42"）とレジスタ（"%t0"）を同じ char * で扱えるので、
// 呼び出し側で場合分けが不要になります。
static char *gen_logical(Emitter *e, Node *n);

static char *gen_expr(Emitter *e, Node *n) {
    switch (n->kind) {
        case ND_INT: {
            // 整数リテラルは命令を出す必要すらありません。
            // LLVM は即値をオペランドに直接書けるので（add i64 42, 1）、
            // 「42」という文字列をそのまま返します。
            char *buf = xmalloc(24);
            snprintf(buf, 24, "%lld", n->ival);
            return buf;
        }

        case ND_BINOP: {
            // ★ 左辺 → 右辺の順に生成する（仕様 4.5：評価順は左から右）
            char *l = gen_expr(e, n->lhs);
            char *r = gen_expr(e, n->rhs);
            char *t = new_tmp(e);

            // ⚠️ オペランドの型は「結果の型」ではありません。
            //    比較の結果は bool ですが、比べているのは左辺の型（int など）です。
            //    第5章までは両者が一致していたので llvm_type(n->type) で
            //    動いていました。比較演算子で初めてこの前提が崩れます。
            Type *ot = n->lhs->type;

            if (is_compare(n->op))
                sb_printf(&e->fn, "  %s = icmp %s %s %s, %s\n", t,
                          icmp_pred(n->op, ot), llvm_type(ot), l, r);
            else
                sb_printf(&e->fn, "  %s = %s %s %s, %s\n", t, llvm_binop(n),
                          llvm_type(ot), l, r);
            return t;
        }

        case ND_BOOL: {
            // True / False は i1 の即値。LLVM は "true" / "false" と書けます。
            return n->ival ? "true" : "false";
        }

        case ND_LOGICAL:
            return gen_logical(e, n);

        case ND_VAR:
            // 変数の読み出し（規約 R2）。bool なら i8 → i1 の変換も入る。
            return gen_load(e, n->type, var_ptr(n->name));

        case ND_UNARY: {
            char *v = gen_expr(e, n->lhs);

            // +x は何もしない（値をそのまま返す）
            if (n->op == OP_POS) return v;

            char *t = new_tmp(e);
            if (n->op == OP_NEG) {
                // ⚠️ LLVM に整数の neg 命令はありません。0 からの減算で表現します。
                sb_printf(&e->fn, "  %s = sub i64 0, %s\n", t, v);
            } else if (n->op == OP_BITNOT) {
                // ~x は全ビット反転 = x XOR -1（-1 は全ビット 1）
                sb_printf(&e->fn, "  %s = xor i64 %s, -1\n", t, v);
            } else if (n->op == OP_NOT) {
                // not x は x XOR true（~x と同じ発想。幅が 1 ビットになっただけ）
                sb_printf(&e->fn, "  %s = xor i1 %s, true\n", t, v);
            } else {
                UNREACHABLE();
            }
            return t;
        }

        default:
            UNREACHABLE();
    }
}

// ── 短絡評価（規約 6.6）────────────────────────────────────
//
// ★ この章で初めて基本ブロックを分岐させます。
//
//   a and b  … a が偽なら b を評価せずに偽
//   a or  b  … a が真なら b を評価せずに真
//
//   「評価しない」を実現するには命令を飛び越える必要があるので、分岐が要ります。
//
// 🤔 なぜ phi を使わないのか（規約 R3）
//   教科書的には合流点で phi を使いますが、phi は「どのブロックから来たか」を
//   書く必要があり、生成側が前のブロックのラベルを覚えていなければなりません。
//   ネストすると管理が急激に面倒になります。
//   「alloca に置いて最後に読む」方式ならその面倒がゼロで、
//   mem2reg がこの alloca を phi に変換してくれます。
static char *gen_logical(Emitter *e, Node *n) {
    // ⚠️ 番号は最初に 1 回だけ確保する。
    //    使うたびに e->label_counter++ すると同じ and の中で番号がずれます。
    int id = e->label_counter++;
    const char *kind = n->op == OP_AND ? "and" : "or";

    char rhs_l[32], end_l[32], res[40];
    snprintf(rhs_l, sizeof(rhs_l), "%s.rhs.%d", kind, id);
    snprintf(end_l, sizeof(end_l), "%s.end.%d", kind, id);
    snprintf(res, sizeof(res), "%%%s.result.%d", kind, id);

    // 結果を入れる箱。★ alloca は entry ブロックへ（規約 R1）
    sb_printf(&e->allocas, "  %s = alloca i8\n", res);

    // ① 左辺を評価し、その値をいったん結果として置く
    char *l = gen_expr(e, n->lhs);
    gen_store(e, ty_bool, l, res);

    // ② 右辺を評価すべきか分岐する（and と or で真偽が逆）
    if (n->op == OP_AND)
        emit_cond_br(e, l, rhs_l, end_l);
    else
        emit_cond_br(e, l, end_l, rhs_l);

    // ③ 右辺（飛ばされることがあるブロック）
    emit_label(e, rhs_l);
    char *r = gen_expr(e, n->rhs);
    gen_store(e, ty_bool, r, res);
    emit_br(e, end_l);

    // ④ 合流点
    emit_label(e, end_l);
    return gen_load(e, ty_bool, res);
}

// ── 文の生成 ────────────────────────────────────────────────
//
// 文は「値を返さない」ので、gen_expr とは別の関数にします。
// ただし式文だけは値を持つので、その値を返します
// （プログラムの値＝最後の式文の値、という暫定仕様のため）。
static char *gen_stmt(Emitter *e, Node *n) {
    switch (n->kind) {
        case ND_VARDECL: {
            // alloca は entry ブロックに出済み（規約 R1）。ここでは store だけ。
            char *val = gen_expr(e, n->rhs);
            gen_store(e, n->type, val, var_ptr(n->name));
            return NULL;
        }

        case ND_ASSIGN: {
            char *val = gen_expr(e, n->rhs);
            gen_store(e, n->type, val, var_ptr(n->lhs->name));
            return NULL;
        }

        default:
            return gen_expr(e, n);  // 式文
    }
}

// ── alloca の収集（規約 R1）────────────────────────────────
//
// ★ すべてのローカル変数は entry ブロックで alloca します。
//
//   関数本体の途中に alloca を書いても動きますが、ループの中にあると
//   反復のたびにスタックを消費します。entry にまとめるのが LLVM の作法で、
//   mem2reg が最適化しやすい形でもあります。
//
//   本体を生成する「前」に AST を歩いて、宣言されている変数を全部集めます。
//   第7章で if / while のブロックが入っても、再帰で辿れば同じように動きます。
static void collect_allocas(Emitter *e, Node *n) {
    if (!n) return;

    if (n->kind == ND_VARDECL)
        sb_printf(&e->allocas, "  %s = alloca %s\n", var_ptr(n->name),
                  llvm_mem_type(n->type));  // ★ bool は i8（規約 R5）

    // 子と兄弟をたどる
    collect_allocas(e, n->lhs);
    collect_allocas(e, n->rhs);
    for (Node *s = n->body; s; s = s->next) collect_allocas(e, s);
}

// ── 関数の生成 ──────────────────────────────────────────────

// ユーザーのプログラム本体を @mython_main として出力する。
//
// 第1章ではプログラム全体がただ 1 つの式なので、
// 「その式を評価して返すだけの関数」になります。
static void gen_mython_main(Emitter *e, Node *ast) {
    // 関数ごとにリセットする（規約）
    e->tmp_counter = 0;
    e->label_counter = 0;
    e->terminated = false;
    sb_init(&e->allocas);
    sb_init(&e->fn);

    // ① まず全変数の alloca を集める（第5章のまま）→ e->allocas
    collect_allocas(e, ast);

    // ② 本体：文を順に生成し、最後の式文の値を返す。
    //    この生成中に、短絡評価が必要とする alloca が e->allocas に増えます。
    char *last = NULL;
    for (Node *s = ast->body; s; s = s->next) {
        char *v = gen_stmt(e, s);
        if (v) last = v;
    }
    if (!last) UNREACHABLE();  // sema が「最後は式」を保証している

    // 最後の式が bool なら i64 に広げてから返す。
    // （@mython_main は i64 を返す。第8章で def main() -> int: になれば消えます）
    if (ast->type->kind == TY_BOOL) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, last);
        last = t;
    }
    sb_printf(&e->fn, "  ret i64 %s\n", last);

    // ③ 組み立て：entry の直後に alloca をまとめて差し込む（規約 R1）
    sb_printf(&e->body, "define i64 @mython_main() {\n");
    sb_printf(&e->body, "entry:\n");
    sb_printf(&e->body, "%s", sb_str(&e->allocas));
    sb_printf(&e->body, "%s", sb_str(&e->fn));
    sb_printf(&e->body, "}\n");
}

// C の main を出力する。
//
// Mython の main は int（= i64）を返しますが、C の main は i32 を返します。
// そこで「ユーザーの main を @mython_main として出し、@main は
// それを呼んで trunc するラッパにする」方式をとります。
// （ir-conventions.md 第7節の方式 A）
//
// 🤔 なぜラッパ方式か：main を他の関数と同じ規則で生成できるので、
//    コード生成器に「main だけ特別」という分岐が入りません。
static void gen_c_main(Emitter *e) {
    e->tmp_counter = 0;

    sb_printf(&e->body, "\n");
    sb_printf(&e->body, "define i32 @main() {\n");
    sb_printf(&e->body, "entry:\n");

    char *t0 = new_tmp(e);
    sb_printf(&e->body, "  %s = call i64 @mython_main()\n", t0);

    char *t1 = new_tmp(e);
    sb_printf(&e->body, "  %s = trunc i64 %s to i32\n", t1, t0);

    sb_printf(&e->body, "  ret i32 %s\n", t1);
    sb_printf(&e->body, "}\n");
}

// ── 入口 ───────────────────────────────────────────────────

char *codegen(Node *ast, const char *source_name) {
    Emitter e = {0};
    sb_init(&e.header);
    sb_init(&e.globals);
    sb_init(&e.decls);
    sb_init(&e.body);

    // ① ヘッダ
    sb_printf(&e.header, "; Generated by mythonc\n");
    sb_printf(&e.header, "source_filename = \"%s\"\n", source_name);

    // ⚠️ 規約 R11：target triple は必ず出力する。
    //    書かないと clang が -Woverride-module 警告を出します。
    if (MYTHON_TARGET_TRIPLE[0])
        sb_printf(&e.header, "target triple = \"%s\"\n", MYTHON_TARGET_TRIPLE);

    // ⑤ 関数定義
    sb_printf(&e.body, "\n");
    gen_mython_main(&e, ast);
    gen_c_main(&e);

    // 4 つのバッファを規定の順に連結する
    StrBuf out;
    sb_init(&out);
    sb_printf(&out, "%s", sb_str(&e.header));
    sb_printf(&out, "%s", sb_str(&e.globals));
    sb_printf(&out, "%s", sb_str(&e.decls));
    sb_printf(&out, "%s", sb_str(&e.body));
    return sb_str(&out);
}
