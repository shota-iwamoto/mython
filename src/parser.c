#include "parser.h"

// ── パーサの状態 ────────────────────────────────────────────
// トークン配列と「今どこを見ているか」の位置だけを持ちます。
typedef struct {
    TokenVec toks;
    int pos;
} Parser;

// ── トークン操作の基本部品 ──────────────────────────────────
// この 4 つの関数だけで、パーサ全体を書きます。

// 現在のトークンを覗く（消費しない）
static Token *peek(Parser *p) { return &p->toks.data[p->pos]; }

// 【第5章で追加する予定】n 個先のトークンを覗く関数。
// トークンをリンクリストではなく配列で持っているので O(1) で書けます。
//
//     static Token *peek_at(Parser *p, int n) {
//         int i = p->pos + n;
//         if (i >= p->toks.len) i = p->toks.len - 1;  // EOF より先は EOF
//         return &p->toks.data[i];
//     }
//
// 「x : int = 1（変数宣言）」と「x = 1（代入）」はどちらも IDENT で始まるため、
// 2 個先まで見て区別する必要があります。今は使わないので、
// -Wall の未使用警告を出さないためコメントにしてあります。

// 現在のトークンを消費して返す（1 つ進む）
static Token *advance(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

// ④ 現在のトークンが指定した記号なら消費、違えばエラー。
static Token *expect_punct(Parser *p, const char *op) {
    Token *t = peek(p);
    if (!tok_is(t, op)) error_at(t, "'%s' が必要です", op);
    return advance(p);
}

// 【第4章で追加する予定】トークン種別を指定して要求する版：
//
//     static Token *expect(Parser *p, TokenKind kind, const char *what) {
//         Token *t = peek(p);
//         if (t->kind != kind)
//             error_at(t, "%s が必要です（実際は %s）", what,
//                      token_kind_name(t->kind));
//         return advance(p);
//     }
//
// NEWLINE / INDENT / DEDENT を要求するときに必要になります。
// 今は記号版だけで足りるので、未使用警告を避けてコメントにしてあります。

// 現在のトークンが指定した記号なら消費して返す。違えば NULL。
// 二項演算子のループでこれを使います。
static Token *consume(Parser *p, const char *op) {
    if (tok_is(peek(p), op)) return advance(p);
    return NULL;
}

// ── 文法規則（優先順位の階層）─────────────────────────────────
//
// ★ この章の中心概念。
//
// 優先順位の「弱い」演算子を上（先に呼ばれる関数）、
// 「強い」演算子を下（後から呼ばれる関数）に置くと、
// それだけで優先順位が実現されます。
//
// なぜそうなるのかは docs/spec/grammar.md 第5節に完全なトレースがあります。
//
//   expr        ::= bitor_expr                       弱い ↑
//   bitor_expr  ::= bitxor_expr { "|"  bitxor_expr }
//   bitxor_expr ::= bitand_expr { "^"  bitand_expr }
//   bitand_expr ::= shift_expr  { "&"  shift_expr }
//   shift_expr  ::= add_expr    { ("<<"|">>") add_expr }
//   add_expr    ::= mul_expr    { ("+"|"-")   mul_expr }
//   mul_expr    ::= unary       { ("*"|"/"|"//"|"%") unary }
//   unary       ::= ("-"|"+"|"~") unary | power
//   power       ::= primary [ "**" unary ]           （第9章で実装）
//   primary     ::= INT | "(" expr ")"               強い ↓

static Node *expr(Parser *p);

// primary ::= INT | "(" expr ")"
static Node *primary(Parser *p) {
    // 括弧：優先順位を無視して中身を先に計算させる。
    // 再帰的に expr() を呼び戻すのがポイント（階層の一番上に戻る）。
    if (consume(p, "(")) {
        Node *n = expr(p);
        expect_punct(p, ")");
        return n;
    }

    Token *t = peek(p);
    if (t->kind == TK_INT) {
        advance(p);
        return new_int_node(t, t->ival);
    }

    error_at(t, "式が必要です");
}

// power ::= primary [ "**" unary ]
//
// ⚠️ '**' は第9章で実装します（負の指数を実行時エラーにするため
//    ランタイムが必要）。今は親切なメッセージを出すだけにします。
//
// ★ 検査をここに置く理由：
//    '**' は「基数を読み終えた後」に現れます。unary() の入口に置くと、
//    2 ** 10 の '**' は誰にも見られず、最終的に program() の
//    「式の後に余分なトークンがあります」になってしまいます
//    （実際にそのバグを踏みました）。
//
//    第9章ではこの関数がこうなります（右結合なので unary() を再帰で呼ぶ）:
//        Node *base = primary(p);
//        if (consume(p, "**"))
//            return new_binop_node(t, OP_POW, base, unary(p));
//        return base;
static Node *power(Parser *p) {
    Node *base = primary(p);

    Token *t = peek(p);
    if (tok_is(t, "**"))
        error_at(t, "演算子 '**' はまだ未対応です（第9章で実装予定）");

    return base;
}

// unary ::= ("-" | "+" | "~") unary | power
//
// ★ 右結合の書き方：自分自身を再帰で呼ぶ。
//    これで "- - 5" が (- (- 5)) と右から結合します。
static Node *unary(Parser *p) {
    Token *t = peek(p);

    if (consume(p, "-")) return new_unary_node(t, OP_NEG, unary(p));
    if (consume(p, "+")) return new_unary_node(t, OP_POS, unary(p));
    if (consume(p, "~")) return new_unary_node(t, OP_BITNOT, unary(p));

    return power(p);
}

// mul_expr ::= unary { ("*" | "/" | "//" | "%") unary }
//
// ★ 左結合の書き方：while ループで lhs を上書きし続ける。
//    これで "8 // 4 // 2" が ((8//4)//2) と左から結合します。
static Node *mul_expr(Parser *p) {
    Node *lhs = unary(p);
    for (;;) {
        Token *t = peek(p);
        // ★ "//" を "/" より先に判定すること。
        //    字句解析側で最長一致しているので実際は衝突しませんが、
        //    順序を意識する習慣をつけます。
        if (consume(p, "//"))     lhs = new_binop_node(t, OP_FLOORDIV, lhs, unary(p));
        else if (consume(p, "*")) lhs = new_binop_node(t, OP_MUL, lhs, unary(p));
        else if (consume(p, "/")) lhs = new_binop_node(t, OP_TRUEDIV, lhs, unary(p));
        else if (consume(p, "%")) lhs = new_binop_node(t, OP_MOD, lhs, unary(p));
        else return lhs;
    }
}

// add_expr ::= mul_expr { ("+" | "-") mul_expr }
static Node *add_expr(Parser *p) {
    Node *lhs = mul_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "+"))      lhs = new_binop_node(t, OP_ADD, lhs, mul_expr(p));
        else if (consume(p, "-")) lhs = new_binop_node(t, OP_SUB, lhs, mul_expr(p));
        else return lhs;
    }
}

// shift_expr ::= add_expr { ("<<" | ">>") add_expr }
static Node *shift_expr(Parser *p) {
    Node *lhs = add_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "<<"))      lhs = new_binop_node(t, OP_SHL, lhs, add_expr(p));
        else if (consume(p, ">>")) lhs = new_binop_node(t, OP_SHR, lhs, add_expr(p));
        else return lhs;
    }
}

// bitand_expr ::= shift_expr { "&" shift_expr }
static Node *bitand_expr(Parser *p) {
    Node *lhs = shift_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "&")) lhs = new_binop_node(t, OP_BITAND, lhs, shift_expr(p));
        else return lhs;
    }
}

// bitxor_expr ::= bitand_expr { "^" bitand_expr }
static Node *bitxor_expr(Parser *p) {
    Node *lhs = bitand_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "^")) lhs = new_binop_node(t, OP_BITXOR, lhs, bitand_expr(p));
        else return lhs;
    }
}

// bitor_expr ::= bitxor_expr { "|" bitxor_expr }
static Node *bitor_expr(Parser *p) {
    Node *lhs = bitxor_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "|")) lhs = new_binop_node(t, OP_BITOR, lhs, bitxor_expr(p));
        else return lhs;
    }
}

// expr ::= bitor_expr
//
// 第6章で、この上に or_expr / and_expr / not_expr / comparison が積まれます。
static Node *expr(Parser *p) { return bitor_expr(p); }

// program ::= expr EOF
static Node *program(Parser *p) {
    Node *n = expr(p);

    // 式を読み終えたら EOF のはず。そうでなければ余計なものが残っている。
    Token *t = peek(p);
    if (t->kind != TK_EOF)
        error_at(t, "式の後に余分なトークンがあります");

    return n;
}

// ── 入口 ───────────────────────────────────────────────────

Node *parse(TokenVec toks) {
    Parser p = {.toks = toks, .pos = 0};

    // 空ファイル（TK_EOF のみ）を親切に弾く
    if (toks.len > 0 && toks.data[0].kind == TK_EOF)
        error_at(&toks.data[0], "空のプログラムです。整数を 1 つ書いてください");

    return program(&p);
}
