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

// 現在のトークンが期待する種類なら消費して返す。違えばエラー。
static Token *expect(Parser *p, TokenKind kind, const char *what) {
    Token *t = peek(p);
    if (t->kind != kind)
        error_at(t, "%s が必要です（実際は %s）", what, token_kind_name(t->kind));
    return advance(p);
}

// ── 文法規則 ────────────────────────────────────────────────

// expr ::= INT
//
// 第2章で、ここが優先順位の階層に置き換わります：
//     expr ::= add_expr
//     add_expr ::= mul_expr { ("+"|"-") mul_expr }
//     ...
static Node *expr(Parser *p) {
    Token *t = expect(p, TK_INT, "整数");
    return new_int_node(t, t->ival);
}

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
