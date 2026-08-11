#include "codegen.h"

#include <string.h>

#include "diag.h"
#include "sema.h"

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

    // 現在のループ（break / continue の飛び先）。第7章
    struct LoopCtx *loop;

    // 文字列リテラルの共有と declare の重複排除（第9章）
    struct StrLit *strs;
    struct StrLit *decled;
    int str_counter;
} Emitter;

// 出力済みの文字列リテラル / declare を覚えておくための小さなリスト。
typedef struct StrLit StrLit;
struct StrLit {
    char *bytes;
    int len;
    char *label;
    StrLit *next;
};

// break / continue の飛び先（規約 6.5）。
//
// ★ スタック変数として持つのがポイントです。gen_while の呼び出しがネストすれば、
//   C の呼び出しスタックがそのままループのネストになります。
//   自前でスタック構造を作る必要はありません。
typedef struct LoopCtx LoopCtx;
struct LoopCtx {
    LoopCtx *outer;
    const char *break_label;     // while.end.N
    const char *continue_label;  // while.cond.N
};

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
        case TY_BOOL: return "i1";   // レジスタ上は 1 ビット
        case TY_NONE: return "void";  // 値がない（第8章）
        case TY_STR: return "ptr";    // 参照型（第9章）
        case TY_LIST: return "ptr";   // MyList へのポインタ（第10章）
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
        case TY_STR: return "ptr";  // ポインタをそのまま置く（第9章）
        case TY_LIST: return "ptr"; // 第10章
        // ⚠️ TY_NONE はメモリ上の表現を持ちません。
        //    ここに来たら「None の変数を作ろうとしている」= コンパイラのバグ。
        default: UNREACHABLE();
    }
}

// ★ 第8章：変数の IR 名は sema が「記号まで含めた完全な形」で割り当てます。
//    ローカル  : %x, %x.1
//    グローバル: @g.x
//    引数      : %n（%n.arg から alloca にコピーしたもの。規約 R8）
//    codegen 側で名前を組み立てる必要はもうありません（var_ptr は廃止）。

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
        // OP_FLOORDIV / OP_MOD / OP_POW はここに来ません。
        // ★ 第9章で「0 除算・負の指数を検査する」ためにランタイム関数
        //   （my_floordiv / my_mod / my_ipow）の呼び出しに変わりました（規約 R10）。
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

// 終端済みのブロックの後ろにコードを置く必要が出たら、
// 到達不能ブロックのラベルを作る（規約 R7）。
//
//     while True:
//         break
//         print(1)     ← 到達不能。ラベルが無いと命令を置けない
static void ensure_block(Emitter *e) {
    if (!e->terminated) return;
    char l[24];
    snprintf(l, sizeof(l), "dead.%d", e->label_counter++);
    emit_label(e, l);  // 終端済みなので br は補われない
}

// ── 文字列リテラル（第9章）────────────────────────────────
//
// ★ 同じ内容のリテラルは 1 つにまとめます（線形探索で十分）。

// IR の文字列に 1 バイト出力する。
//
// ⚠️ 安全策として、ASCII 印字可能文字**以外はすべて** \XX にします。
//    「どの文字をエスケープすべきか」を考えなくて済むようにするためです。
//    UTF-8 の日本語も各バイトが \XX になるだけで、そのまま通ります。
static void emit_ir_byte(StrBuf *sb, unsigned char c) {
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
        sb_printf(sb, "%c", c);
    else
        sb_printf(sb, "\\%02X", c);
}

static char *intern_str(Emitter *e, const char *bytes, int len) {
    for (StrLit *sl = e->strs; sl; sl = sl->next)
        if (sl->len == len && memcmp(sl->bytes, bytes, (size_t)len) == 0)
            return sl->label;

    StrBuf lab;
    sb_init(&lab);
    sb_printf(&lab, "@.str.%d", e->str_counter++);

    // ⚠️ 長さは「バイト数 + 1」。NUL の分を忘れない。
    StrBuf g;
    sb_init(&g);
    sb_printf(&g, "%s = private unnamed_addr constant [%d x i8] c\"", sb_str(&lab),
              len + 1);
    for (int i = 0; i < len; i++) emit_ir_byte(&g, (unsigned char)bytes[i]);
    sb_printf(&g, "\\00\"\n");
    sb_printf(&e->globals, "%s", sb_str(&g));

    StrLit *sl = xmalloc(sizeof(StrLit));
    sl->bytes = (char *)bytes;
    sl->len = len;
    sl->label = sb_str(&lab);
    sl->next = e->strs;
    e->strs = sl;
    return sl->label;
}

// ランタイム関数を宣言する（1 回だけ）。
static void declare_rt(Emitter *e, const char *sig) {
    for (StrLit *d = e->decled; d; d = d->next)
        if (strcmp(d->label, sig) == 0) return;
    sb_printf(&e->decls, "declare %s\n", sig);
    StrLit *d = xmalloc(sizeof(StrLit));
    d->label = (char *)sig;
    d->next = e->decled;
    e->decled = d;
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
static char *gen_call(Emitter *e, Node *n);
static char *gen_list_lit(Emitter *e, Node *n);
static char *gen_index(Emitter *e, Node *n);
static char *gen_method(Emitter *e, Node *n);

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

            // ── 文字列（第9章）──────────────────────────────
            if (ot->kind == TY_STR) {
                if (n->op == OP_ADD) {
                    declare_rt(e, "ptr @my_str_concat(ptr, ptr)");
                    sb_printf(&e->fn, "  %s = call ptr @my_str_concat(ptr %s, ptr %s)\n",
                              t, l, r);
                    return t;
                }
                // ⚠️ 比較は「内容」で行う（言語仕様 4.3）。ポインタ比較ではない。
                //   my_str_cmp が strcmp の符号を返すので、0 と比べる述語を
                //   変えるだけで 6 種類すべてに対応できます。
                declare_rt(e, "i64 @my_str_cmp(ptr, ptr)");
                char *c = new_tmp(e);
                sb_printf(&e->fn, "  %s = call i64 @my_str_cmp(ptr %s, ptr %s)\n", c,
                          l, r);
                sb_printf(&e->fn, "  %s = icmp %s i64 %s, 0\n", t,
                          icmp_pred(n->op, ty_int), c);
                return t;
            }

            // ── 検査つきの算術（規約 R10。第9章）──────────────
            //
            // ★ 0 除算は SIGFPE でプロセスが死にます。何が起きたか分からない
            //   より、メッセージを出して死ぬほうが親切です。分岐を IR に出さず、
            //   ランタイム関数に押し込むのが R10 の実践です。
            if (n->op == OP_FLOORDIV || n->op == OP_MOD || n->op == OP_POW) {
                const char *fn = n->op == OP_FLOORDIV ? "my_floordiv"
                                 : n->op == OP_MOD    ? "my_mod"
                                                      : "my_ipow";
                StrBuf sig;
                sb_init(&sig);
                sb_printf(&sig, "i64 @%s(i64, i64)", fn);
                declare_rt(e, sb_str(&sig));
                sb_printf(&e->fn, "  %s = call i64 @%s(i64 %s, i64 %s)\n", t, fn, l, r);
                return t;
            }

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

        case ND_STR:
            // ★ リテラルは .rodata の定数。ラベルをそのまま ptr として使えます
            //   （opaque pointer なので getelementptr は不要）。
            return intern_str(e, n->sval, n->slen);

        case ND_LOGICAL:
            return gen_logical(e, n);

        case ND_CALL:
            return gen_call(e, n);

        case ND_LIST:
            return gen_list_lit(e, n);

        case ND_INDEX:
            return gen_index(e, n);

        case ND_METHOD:
            return gen_method(e, n);

        case ND_VAR:
            // 変数の読み出し（規約 R2）。bool なら i8 → i1 の変換も入る。
            // ★ n->name ではなく sema が割り当てた n->ir_name を使う（第7章）
            return gen_load(e, n->type, n->ir_name);

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

// ── list[T] の生成（第10章）────────────────────────────────
//
// ★ 要素はすべて 8 バイト。i64 で持つか、ポインタで持つかの 2 通りだけです。
static bool elem_is_ptr(Type *elem) {
    return elem->kind == TY_STR || elem->kind == TY_LIST;
}

// 要素の値を「ランタイムに渡す形」にする（bool は i64 に広げる。規約 R5）
static char *elem_to_slot(Emitter *e, Type *elem, char *v) {
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, v);
    return t;
}

// ランタイムから受け取った値を「Mython の値」に戻す（bool は i1 に縮める）
static char *slot_to_elem(Emitter *e, Type *elem, char *v) {
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i64 %s to i1\n", t, v);
    return t;
}

static const char *slot_ty(Type *elem) { return elem_is_ptr(elem) ? "ptr" : "i64"; }

// [1, 2, 3] は「空リストを作って append を繰り返す」に脱糖する。
// ★ 第5章の複合代入、第7章の elif と同じ「脱糖」の手です。
static char *gen_list_lit(Emitter *e, Node *n) {
    Type *elem = n->type->elem;

    declare_rt(e, "ptr @my_list_new()");
    char *l = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @my_list_new()\n", l);

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "my_list_push_ptr" : "my_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    for (Node *el = n->body; el; el = el->next) {
        char *v = elem_to_slot(e, elem, gen_expr(e, el));
        sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, l, sty, v);
    }
    return l;
}

static char *gen_index(Emitter *e, Node *n) {
    Type *ot = n->lhs->type;
    char *obj = gen_expr(e, n->lhs);
    char *idx = gen_expr(e, n->rhs);

    // str の添字は 1 文字の str を返す（型システム 5.8）
    if (ot->kind == TY_STR) {
        declare_rt(e, "ptr @my_str_index(ptr, i64)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @my_str_index(ptr %s, i64 %s)\n", t, obj,
                  idx);
        return t;
    }

    Type *elem = ot->elem;
    const char *sty = slot_ty(elem);
    const char *get = elem_is_ptr(elem) ? "my_list_get_ptr" : "my_list_get_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(ptr, i64)", sty, get);
    declare_rt(e, sb_str(&sig));

    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call %s @%s(ptr %s, i64 %s)\n", t, sty, get, obj, idx);
    return slot_to_elem(e, elem, t);
}

static void gen_index_store(Emitter *e, Node *target, char *val) {
    Type *elem = target->lhs->type->elem;
    char *obj = gen_expr(e, target->lhs);
    char *idx = gen_expr(e, target->rhs);

    const char *sty = slot_ty(elem);
    const char *set = elem_is_ptr(elem) ? "my_list_set_ptr" : "my_list_set_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, i64, %s)", set, sty);
    declare_rt(e, sb_str(&sig));

    char *v = elem_to_slot(e, elem, val);
    sb_printf(&e->fn, "  call void @%s(ptr %s, i64 %s, %s %s)\n", set, obj, idx, sty,
              v);
}

static char *gen_method(Emitter *e, Node *n) {
    // 今のところ list.append だけ（sema が保証している）
    Type *elem = n->lhs->type->elem;
    char *obj = gen_expr(e, n->lhs);

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "my_list_push_ptr" : "my_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    char *v = elem_to_slot(e, elem, gen_expr(e, n->args));
    sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, obj, sty, v);
    return NULL;
}

// ── 制御構文の生成（規約 6.3 / 6.4 / 6.5）──────────────────

static char *gen_stmt(Emitter *e, Node *n);

static void gen_if(Emitter *e, Node *n) {
    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する

    char then_l[32], else_l[32], end_l[32];
    snprintf(then_l, sizeof(then_l), "if.then.%d", id);
    snprintf(else_l, sizeof(else_l), "if.else.%d", id);
    snprintf(end_l, sizeof(end_l), "if.end.%d", id);

    char *cond = gen_expr(e, n->lhs);
    // else が無ければ else ブロックを作らず、直接 end へ分岐する
    emit_cond_br(e, cond, then_l, n->els ? else_l : end_l);

    emit_label(e, then_l);
    gen_stmt(e, n->body);
    // ⚠️ then 節が break / continue で終わっていたら、そこは既に終端済み。
    //    もう 1 つ br を出すと「1 ブロックに終端命令が 2 つ」になり LLVM が怒ります。
    if (!e->terminated) emit_br(e, end_l);

    if (n->els) {
        emit_label(e, else_l);
        gen_stmt(e, n->els);
        if (!e->terminated) emit_br(e, end_l);
    }

    emit_label(e, end_l);
}

static void gen_while(Emitter *e, Node *n) {
    int id = e->label_counter++;

    char cond_l[32], body_l[32], end_l[32];
    snprintf(cond_l, sizeof(cond_l), "while.cond.%d", id);
    snprintf(body_l, sizeof(body_l), "while.body.%d", id);
    snprintf(end_l, sizeof(end_l), "while.end.%d", id);

    // ⚠️ 条件ブロックに「入る」ための br が必要（規約 6.4）。
    //    条件を独立したブロックにしないと 1 回目の判定が飛ばされ、
    //    do-while になってしまいます。
    emit_br(e, cond_l);

    emit_label(e, cond_l);
    char *cond = gen_expr(e, n->lhs);  // ★ 条件は反復のたびに評価される
    emit_cond_br(e, cond, body_l, end_l);

    // ★ 第11章：増分があるなら continue の飛び先は「増分ブロック」。
    //   無ければ従来どおり「条件ブロック」（第7章のまま）。
    //
    // ⚠️ ここを間違えると、for の中の continue が増分を飛ばして無限ループになります。
    char incr_l[32];
    const char *cont_l = cond_l;
    if (n->incr) {
        snprintf(incr_l, sizeof(incr_l), "for.incr.%d", id);
        cont_l = incr_l;
    }

    // break / continue の飛び先を積む
    LoopCtx ctx = {.outer = e->loop, .break_label = end_l, .continue_label = cont_l};
    e->loop = &ctx;

    emit_label(e, body_l);
    gen_stmt(e, n->body);

    if (n->incr) {
        // ★ emit_label が「終端していなければ br を補う」ので、
        //   本体から増分ブロックへは自動的に繋がります（第6章の関数）。
        emit_label(e, incr_l);
        gen_stmt(e, n->incr);
    }
    if (!e->terminated) emit_br(e, cond_l);  // ループバック

    e->loop = ctx.outer;  // ★ 対で戻す

    emit_label(e, end_l);
}

// print(e) の暫定実装。C の printf を借ります。
//
// ★ 第1章で用意した globals / decls バッファが、ここで初めて使われます。
// 組み込み関数の呼び出し（第9章）。
//
// ★ sema が選んだ候補（n->builtin）に従って、対応するランタイム関数を呼ぶだけ。
//   「どの実装を呼ぶか」の判断は sema 側で終わっています。
static char *gen_builtin_call(Emitter *e, Node *n) {
    const Builtin *b = n->builtin;
    // ⚠️ 引数の型は表からではなく実引数から取ります。
    //    list[T] にはシングルトンが無いので type_from_kind では引けません（第10章）。
    Type *at = n->args->type;
    Type *rt = type_from_kind(b->ret);

    // ⚠️ bool は Mython のレジスタ上では i1 ですが、C 側は long long で
    //    受け取ります。境界で i64 に広げます（規約 R5 と同じ考え方）。
    const char *argty = at->kind == TY_BOOL ? "i64" : llvm_type(at);

    // declare を 1 回だけ出す
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(%s)", llvm_type(rt), b->impl, argty);
    declare_rt(e, sb_str(&sig));

    char *v = gen_expr(e, n->args);
    if (at->kind == TY_BOOL) {
        char *z = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", z, v);
        v = z;
    }

    if (rt->kind == TY_NONE) {
        sb_printf(&e->fn, "  call void @%s(%s %s)\n", b->impl, argty, v);
        return NULL;
    }
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call %s @%s(%s %s)\n", t, llvm_type(rt), b->impl,
              argty, v);
    return t;
}

// 関数呼び出し。
//
// ⚠️ void の呼び出しに結果を代入してはいけません。
//      %t0 = call void @f()   ✗
//      call void @f()         ✅
static char *gen_call(Emitter *e, Node *n) {
    if (n->builtin) return gen_builtin_call(e, n);

    // 引数を左から順に評価する（言語仕様 4.5）
    StrBuf args;
    sb_init(&args);
    bool first = true;
    for (Node *a = n->args; a; a = a->next) {
        char *v = gen_expr(e, a);
        sb_printf(&args, "%s%s %s", first ? "" : ", ", llvm_type(a->type), v);
        first = false;
    }

    if (n->type->kind == TY_NONE) {
        sb_printf(&e->fn, "  call void @%s(%s)\n", n->name, sb_str(&args));
        return NULL;
    }
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call %s @%s(%s)\n", t, llvm_type(n->type), n->name,
              sb_str(&args));
    return t;
}

// ── 文の生成 ────────────────────────────────────────────────
//
// 文は「値を返さない」ので、gen_expr とは別の関数にします。
// ただし式文だけは値を持つので、その値を返します
// （プログラムの値＝最後の式文の値、という暫定仕様のため）。
static char *gen_stmt(Emitter *e, Node *n) {
    // 終端済みブロックの後ろに来たら、到達不能ブロックを開く（規約 R7）
    ensure_block(e);

    switch (n->kind) {
        case ND_BLOCK: {
            char *last = NULL;
            for (Node *st = n->body; st; st = st->next) {
                char *v = gen_stmt(e, st);
                if (v) last = v;
            }
            return last;
        }

        case ND_IF: gen_if(e, n); return NULL;
        case ND_WHILE: gen_while(e, n); return NULL;
        case ND_RETURN: {
            if (!n->lhs) {
                sb_printf(&e->fn, "  ret void\n");  // 規約 R9
            } else {
                char *v = gen_expr(e, n->lhs);
                sb_printf(&e->fn, "  ret %s %s\n", llvm_type(n->lhs->type), v);
            }
            e->terminated = true;
            return NULL;
        }
        case ND_PASS: return NULL;  // 本当に何も出さない

        // 飛び先は sema が保証している（ループの外なら検査で弾かれる）
        case ND_BREAK: emit_br(e, e->loop->break_label); return NULL;
        case ND_CONTINUE: emit_br(e, e->loop->continue_label); return NULL;

        case ND_VARDECL: {
            // alloca は entry ブロックに出済み（規約 R1）。ここでは store だけ。
            char *val = gen_expr(e, n->rhs);
            gen_store(e, n->type, val, n->ir_name);
            return NULL;
        }

        case ND_ASSIGN: {
            char *val = gen_expr(e, n->rhs);
            // 添字への代入 xs[i] = v（第10章）
            if (n->lhs->kind == ND_INDEX) {
                gen_index_store(e, n->lhs, val);
                return NULL;
            }
            gen_store(e, n->type, val, n->lhs->ir_name);
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

    // ⚠️ グローバル変数は alloca しない（@g.x をそのまま読み書きする）
    if (n->kind == ND_VARDECL && !n->is_global)
        sb_printf(&e->allocas, "  %s = alloca %s\n", n->ir_name,
                  llvm_mem_type(n->type));  // ★ bool は i8（規約 R5）

    // 子と兄弟をたどる。
    // ★ 第5章で「再帰なので第7章でブロックが入っても勝手に見つかる」と
    //   書いたとおりになりました。else 節の分だけ 1 行足せば済みます。
    collect_allocas(e, n->lhs);
    collect_allocas(e, n->rhs);
    collect_allocas(e, n->els);
    for (Node *s = n->body; s; s = s->next) collect_allocas(e, s);
}

// ── 関数の生成 ──────────────────────────────────────────────

// 1 つの関数を出力する。
//
// ★ 第8章：第1章から「暗黙の main」だったものが、ここで普通の関数になりました。
static void gen_func(Emitter *e, Node *n) {
    // 関数ごとに状態をリセットする（第1章から決めてあった規約）
    e->tmp_counter = 0;
    e->label_counter = 0;
    e->terminated = false;
    e->loop = NULL;
    sb_init(&e->allocas);
    sb_init(&e->fn);

    // main はラッパ方式（規約 7 節の方式 A）なので @mython_main として出す
    const char *ir_name =
        strcmp(n->name, "main") == 0 ? "mython_main" : n->name;

    // ① 引数を alloca にコピーする（規約 R8）。
    //
    // 🤔 なぜコピーするのか
    //   %n.arg は SSA レジスタなので代入できません。Mython では引数に代入
    //   できる（a = a + 1）ので、ローカル変数と同じ「箱」にしてしまいます。
    //   mem2reg がこの余分なコピーを消してくれます。
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->allocas, "  %s = alloca %s\n", pm->ir_name,
                  llvm_mem_type(pm->type));
        StrBuf arg;
        sb_init(&arg);
        sb_printf(&arg, "%%%s.arg", pm->name);
        gen_store(e, pm->type, sb_str(&arg), pm->ir_name);
    }

    // ② ローカル変数の alloca（第5章のまま）
    collect_allocas(e, n->body);

    // ③ 本体
    gen_stmt(e, n->body);

    // ④ 終端されていなければ終端する（規約 R6）
    if (!e->terminated) {
        if (n->type->kind == TY_NONE) {
            sb_printf(&e->fn, "  ret void\n");  // 規約 R9
        } else {
            // 全経路 return は sema が保証済み。ここに来るのは
            // 「if/else の両方が return して合流点が到達不能」の場合。
            sb_printf(&e->fn, "  unreachable\n");
        }
    }

    // ⑤ 組み立て
    sb_printf(&e->body, "\ndefine %s @%s(", llvm_type(n->type), ir_name);
    bool first = true;
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->body, "%s%s %%%s.arg", first ? "" : ", ",
                  llvm_type(pm->type), pm->name);
        first = false;
    }
    sb_printf(&e->body, ") {\nentry:\n");
    sb_printf(&e->body, "%s", sb_str(&e->allocas));
    sb_printf(&e->body, "%s", sb_str(&e->fn));
    sb_printf(&e->body, "}\n");
}

// グローバル変数を出力する（言語仕様 6.2）
static void gen_global(Emitter *e, Node *n) {
    // 初期化式がリテラルであることは sema が保証している
    if (n->type->kind == TY_STR) {
        char *lab = intern_str(e, n->rhs->sval, n->rhs->slen);
        sb_printf(&e->globals, "%s = global ptr %s\n", n->ir_name, lab);
        return;
    }
    sb_printf(&e->globals, "%s = global %s %lld\n", n->ir_name,
              llvm_mem_type(n->type), n->rhs->ival);
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

    // ⑤ グローバル変数と関数定義
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_VARDECL) gen_global(&e, d);
    }
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) gen_func(&e, d);
    }
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
