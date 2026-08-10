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
    StrBuf body;     // 関数定義

    int tmp_counter;  // 一時値 %tN の連番（関数ごとにリセットする）
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

// Mython の型に対応する LLVM の型名。
// 第6章で bool（値は i1 / メモリは i8）が入ると分岐が増えます。
static const char *llvm_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
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

// ── 式の生成 ────────────────────────────────────────────────
//
// gen_expr の約束：
//   「式を評価する命令列を body に出力し、
//     結果の値が入っている場所の名前（レジスタ名 or 即値）を返す」
//
// この 1 つの約束が、コード生成器の設計全体を決めます。
// 即値（"42"）とレジスタ（"%t0"）を同じ char * で扱えるので、
// 呼び出し側で場合分けが不要になります。
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
            const char *inst = llvm_binop(n);
            char *l = gen_expr(e, n->lhs);
            char *r = gen_expr(e, n->rhs);
            char *t = new_tmp(e);
            sb_printf(&e->body, "  %s = %s %s %s, %s\n", t, inst, llvm_type(n->type), l,
                      r);
            return t;
        }

        case ND_VAR: {
            // 変数の読み出し（規約 R2）
            char *t = new_tmp(e);
            sb_printf(&e->body, "  %s = load %s, ptr %s\n", t, llvm_type(n->type),
                      var_ptr(n->name));
            return t;
        }

        case ND_UNARY: {
            char *v = gen_expr(e, n->lhs);

            // +x は何もしない（値をそのまま返す）
            if (n->op == OP_POS) return v;

            char *t = new_tmp(e);
            if (n->op == OP_NEG) {
                // ⚠️ LLVM に整数の neg 命令はありません。0 からの減算で表現します。
                sb_printf(&e->body, "  %s = sub i64 0, %s\n", t, v);
            } else if (n->op == OP_BITNOT) {
                // ~x は全ビット反転 = x XOR -1（-1 は全ビット 1）
                sb_printf(&e->body, "  %s = xor i64 %s, -1\n", t, v);
            } else {
                UNREACHABLE();
            }
            return t;
        }

        default:
            UNREACHABLE();
    }
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
            sb_printf(&e->body, "  store %s %s, ptr %s\n", llvm_type(n->type), val,
                      var_ptr(n->name));
            return NULL;
        }

        case ND_ASSIGN: {
            char *val = gen_expr(e, n->rhs);
            sb_printf(&e->body, "  store %s %s, ptr %s\n", llvm_type(n->type), val,
                      var_ptr(n->lhs->name));
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
        sb_printf(&e->body, "  %s = alloca %s\n", var_ptr(n->name),
                  llvm_type(n->type));

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
    e->tmp_counter = 0;  // 関数ごとに一時値の連番をリセット（規約）

    sb_printf(&e->body, "define i64 @mython_main() {\n");
    sb_printf(&e->body, "entry:\n");

    // ① まず全変数の alloca を entry ブロックに出す（規約 R1）
    collect_allocas(e, ast);

    // ② 本体：文を順に生成し、最後の式文の値を返す
    char *last = NULL;
    for (Node *s = ast->body; s; s = s->next) {
        char *v = gen_stmt(e, s);
        if (v) last = v;
    }
    if (!last) UNREACHABLE();  // sema が「最後は式」を保証している

    sb_printf(&e->body, "  ret i64 %s\n", last);
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
