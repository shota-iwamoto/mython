#include "parser.h"

#include <string.h>

#include "diag.h"

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

// n 個先のトークンを覗く（消費しない）。
//
// ★ 第1章でトークンを「配列」にした理由がここで回収されます。
//   「x : int = 1（変数宣言）」と「x = 1（代入）」はどちらも IDENT で始まるため、
//   2 個目のトークンを見ないと区別できません。
//   配列なら O(1)、リンクリストなら辿る必要があります。
static Token *peek_at(Parser *p, int n) {
    int i = p->pos + n;
    if (i >= p->toks.len) i = p->toks.len - 1;  // EOF より先は EOF を返す
    return &p->toks.data[i];
}

// 現在のトークンを消費して返す（1 つ進む）
static Token *advance(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

// 現在のトークンが指定した記号なら消費して返す。違えば NULL。
// 二項演算子のループでこれを使います。
static Token *consume(Parser *p, const char *op) {
    if (tok_is(peek(p), op)) return advance(p);
    return NULL;
}

// 現在のトークンが指定したキーワードなら消費して返す。違えば NULL。
// and / or / not は記号ではなくキーワードなので、consume() が使えません。
static Token *consume_kw(Parser *p, const char *kw) {
    if (tok_is_kw(peek(p), kw)) return advance(p);
    return NULL;
}

// 対応する閉じ記号を要求する。無ければ「開き記号はここ」を添えてエラーにする。
//
//   open  … 対応する開き記号のトークン（'(' や '[' ）
//   close … 要求する閉じ記号（")" や "]"）
//
// ★ 開き記号の位置を覚えて示すのが、この章の中心的な改善です。
//   閉じ忘れは「どこが閉じられていないか」が分からないと直せません。
//
// 第8章（引数リストの ')'）と第10章（添字の ']'）でもそのまま使えます。
static Token *expect_close(Parser *p, const char *close, Token *open) {
    Token *t = peek(p);
    if (tok_is(t, close)) return advance(p);

    char *open_text = xstrndup(open->loc, (size_t)open->len);

    Diag d = {0};
    d.message = diag_fmt("閉じ括弧 '%s' がありません", close);
    d.primary.tok = t;
    d.primary.label = diag_fmt("ここに '%s' が必要です", close);
    d.related.tok = open;
    d.related.label = diag_fmt("対応する '%s' はここです", open_text);
    d.hint = "括弧の対応を確認してください";
    diag_fail(&d);
}

// トークン種別を指定して要求する。
//
// ★ 第4章で「必要になる」と予告しておいた関数です。
//   NEWLINE / INDENT / DEDENT を要求する block() のために実装しました。
//
// ⚠️ 仮想トークンの名前（INDENT）をそのままユーザーに見せないこと。
//    「INDENT が必要です」では利用者に意味が伝わりません。
//    what と hint には人間の言葉を渡します。
// トークン種別の日本語名。
// ⚠️ token_kind_name() は "INDENT" のような内部名を返すので、
//    利用者に見せる診断ではこちらを使います。
static const char *tok_kind_ja(TokenKind kind) {
    switch (kind) {
        case TK_EOF: return "ファイルの終わり";
        case TK_INT: return "整数";
        case TK_PUNCT: return "記号";
        case TK_IDENT: return "名前";
        case TK_KEYWORD: return "予約語";
        case TK_NEWLINE: return "改行";
        case TK_INDENT: return "字下げ";
        case TK_DEDENT: return "字下げの終わり";
        default: UNREACHABLE();
    }
}

static Token *expect(Parser *p, TokenKind kind, const char *what,
                     const char *hint) {
    Token *t = peek(p);
    if (t->kind == kind) return advance(p);

    Diag d = {0};
    d.message = diag_fmt("%sが必要です", what);
    d.primary.tok = t;
    d.primary.label = diag_fmt("ここは%sです", tok_kind_ja(t->kind));
    d.hint = hint;
    diag_fail(&d);
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
//   expr        ::= or_expr                          弱い ↑
//   or_expr     ::= and_expr    { "or"  and_expr }
//   and_expr    ::= not_expr    { "and" not_expr }
//   not_expr    ::= "not" not_expr | comparison
//   comparison  ::= bitor_expr [ compop bitor_expr ]  （連鎖不可）
//   bitor_expr  ::= bitxor_expr { "|"  bitxor_expr }
//   bitxor_expr ::= bitand_expr { "^"  bitand_expr }
//   bitand_expr ::= shift_expr  { "&"  shift_expr }
//   shift_expr  ::= add_expr    { ("<<"|">>") add_expr }
//   add_expr    ::= mul_expr    { ("+"|"-")   mul_expr }
//   mul_expr    ::= unary       { ("*"|"/"|"//"|"%") unary }
//   unary       ::= ("-"|"+"|"~") unary | power
//   power       ::= primary [ "**" unary ]           （第9章で実装）
//   primary     ::= INT | True | False | IDENT | "(" expr ")"   強い ↓

static Node *expr(Parser *p);

// primary ::= INT | "(" expr ")"
static Node *primary(Parser *p) {
    // 括弧：優先順位を無視して中身を先に計算させる。
    // 再帰的に expr() を呼び戻すのがポイント（階層の一番上に戻る）。
    //
    // ★ 開き括弧のトークンを覚えておきます。
    //   閉じ括弧が無かったとき、エラーで「対応する '(' はここ」と示すために
    //   使います。第3章で追加した診断の要点です。
    Token *open = consume(p, "(");
    if (open) {
        Node *n = expr(p);
        expect_close(p, ")", open);
        return n;
    }

    Token *t = peek(p);
    if (t->kind == TK_INT) {
        advance(p);
        return new_int_node(t, t->ival);
    }
    if (t->kind == TK_IDENT) {
        advance(p);
        return new_var_node(t, t->text);
    }

    // True / False は予約語だが、式として使える（値を持つリテラル）。
    // ★ 「予約語はエラー」の判定より前に置くこと。
    //   後ろに置くと True が「'True' は予約語です」になってしまいます。
    if (tok_is_kw(t, "True")) {
        advance(p);
        return new_bool_node(t, true);
    }
    if (tok_is_kw(t, "False")) {
        advance(p);
        return new_bool_node(t, false);
    }

    // 予約語が式の位置に来た場合は、専用の説明を出す。
    // 「式が必要です」だけだと、なぜ変数名として使えないのか分かりません。
    if (t->kind == TK_KEYWORD)
        error_at_hint(t, "予約語は変数名として使えません（言語仕様 2.5）",
                      "'%s' は予約語です", t->text);

    // 「何が来るべきだったか」を具体的に伝える。
    Diag d = {0};
    d.message = "式が必要です";
    d.primary.tok = t;
    d.primary.label = t->kind == TK_EOF ? "ここでファイルが終わっています"
                                        : "ここには式が来るはずです";
    d.hint = "式とは整数リテラル、変数名、または '(' で囲んだ式のことです";
    diag_fail(&d);
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
        error_at_hint(t, "第9章で実装します（負の指数を実行時エラーにするため、"
                         "エラー報告のランタイムが必要です）",
                      "演算子 '**' はまだ未対応です");

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

// 比較演算子なら対応する OpKind を返す。違えば -1。
static int compare_op(Token *t) {
    if (tok_is(t, "==")) return OP_EQ;
    if (tok_is(t, "!=")) return OP_NE;
    if (tok_is(t, "<=")) return OP_LE;  // "<" より先に見る（最長一致の習慣）
    if (tok_is(t, ">=")) return OP_GE;
    if (tok_is(t, "<")) return OP_LT;
    if (tok_is(t, ">")) return OP_GT;
    return -1;
}

// comparison ::= bitor_expr [ compop bitor_expr ]
//
// ⚠️ 連鎖しません（言語仕様 4.1）。while ではなく if で書くのがポイントです。
static Node *comparison(Parser *p) {
    Node *lhs = bitor_expr(p);

    Token *t = peek(p);
    int op = compare_op(t);
    if (op < 0) return lhs;  // 比較演算子がない
    advance(p);

    Node *rhs = bitor_expr(p);

    // ★ 比較の連鎖を禁止する。
    //   放っておいても (1 < 2) < 3 で「bool と int の比較」の型エラーには
    //   なりますが、なぜ bool が出てくるのか利用者には分かりません。
    //   構文の段階で捕まえて、直し方を示します。
    Token *t2 = peek(p);
    if (compare_op(t2) >= 0) {
        Diag d = {0};
        d.message = "比較演算子を連鎖させることはできません";
        d.primary.tok = t2;
        d.primary.label = "2 つ目の比較演算子です";
        d.related.tok = t;
        d.related.label = "1 つ目の比較演算子はここです";
        d.hint = "Python と違い連鎖比較は使えません。'and' で繋いでください"
                 "（例: a < b and b < c）";
        diag_fail(&d);
    }

    return new_binop_node(t, (OpKind)op, lhs, rhs);
}

// not_expr ::= "not" not_expr | comparison
//
// ★ 右結合：自分自身を再帰で呼ぶ（第2章の unary と同じ形）。
static Node *not_expr(Parser *p) {
    Token *t = peek(p);
    if (consume_kw(p, "not")) return new_unary_node(t, OP_NOT, not_expr(p));
    return comparison(p);
}

// and_expr ::= not_expr { "and" not_expr }
//
// ★ 左結合：while ループで lhs を上書きする（第2章の add_expr と同じ形）。
static Node *and_expr(Parser *p) {
    Node *lhs = not_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume_kw(p, "and"))
            lhs = new_logical_node(t, OP_AND, lhs, not_expr(p));
        else
            return lhs;
    }
}

// or_expr ::= and_expr { "or" and_expr }
static Node *or_expr(Parser *p) {
    Node *lhs = and_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume_kw(p, "or"))
            lhs = new_logical_node(t, OP_OR, lhs, and_expr(p));
        else
            return lhs;
    }
}

// expr ::= or_expr
//
// ★ 第2章で作った bitor_expr 以下の階層は無変更です。
//   この関数の 1 行を書き換えて、上に 4 段積んだだけ。
static Node *expr(Parser *p) { return or_expr(p); }

// 論理行の終わり（NEWLINE）を要求する
static void expect_newline(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TK_NEWLINE) {
        Diag d = {0};
        d.message = "文の後に余分なトークンがあります";
        d.primary.tok = t;
        d.primary.label = "ここから先が解釈できません";
        d.hint = "1 行に書けるのは 1 つの文です（改行で区切ってください）";
        diag_fail(&d);
    }
    advance(p);
}

// var_decl ::= IDENT ":" type "=" expr
//
// 型注釈は必須です（言語仕様 3.3）。
static Node *var_decl(Parser *p) {
    Token *name_tok = advance(p);  // IDENT（呼び出し元が確認済み）
    advance(p);                    // ":"

    // 型注釈。ここでは「名前を記録する」だけで、
    // それが有効な型かどうかの判断は sema に任せます。
    Token *ty_tok = peek(p);
    if (ty_tok->kind != TK_IDENT)
        error_at_hint(ty_tok, "型注釈には型名を書きます（例: x: int = 0）",
                      "型名が必要です");
    advance(p);

    // 初期化式は必須（言語仕様 5.1：未初期化変数を作らせない）
    if (!tok_is(peek(p), "=")) {
        Diag d = {0};
        d.message = "変数宣言には初期化式が必要です";
        d.primary.tok = peek(p);
        d.primary.label = "ここに '= 初期値' が必要です";
        d.hint = "Mython では未初期化の変数を作れません（例: x: int = 0）";
        diag_fail(&d);
    }
    advance(p);  // "="

    Node *n = new_node(ND_VARDECL, name_tok);
    n->name = name_tok->text;
    n->type_name = ty_tok->text;
    n->rhs = expr(p);
    return n;
}

// 複合代入の記号なら、対応する演算子を返す。違えば -1。
static int aug_op(Token *t) {
    if (tok_is(t, "+=")) return OP_ADD;
    if (tok_is(t, "-=")) return OP_SUB;
    if (tok_is(t, "*=")) return OP_MUL;
    if (tok_is(t, "//=")) return OP_FLOORDIV;
    if (tok_is(t, "%=")) return OP_MOD;
    return -1;
}

// simple_stmt ::= var_decl | assign_stmt | expr_stmt
//
// ★ 代入文と式文の区別のしかた（docs/spec/grammar.md 第4節）
//
//   左辺を先に「式」として読み、その後に '=' が続いていたら
//   「今読んだ式は代入先だった」と解釈し直します。
//
//   こうすると xs[0] = 1 や p.f = 1（第10章・第12章）にも
//   そのまま対応できます。左辺を読み切るまで代入かどうか判らないからです。
// print_stmt ::= "print" "(" expr ")"
//
// ⚠️ 暫定実装です。言語仕様では print は組み込み「関数」ですが、
//    関数呼び出しの構文は第8章、str 型は第9章なので、
//    この章では「文」として特別扱いします（第1章の暫定 main と同じ足場）。
static Node *print_stmt(Parser *p) {
    Token *t = advance(p);     // "print"
    Token *open = advance(p);  // "("

    Node *n = new_node(ND_PRINT, t);
    n->lhs = expr(p);
    expect_close(p, ")", open);
    return n;
}

static Node *simple_stmt(Parser *p) {
    Token *t0 = peek(p);

    // break / continue / pass
    if (tok_is_kw(t0, "break")) { advance(p); return new_node(ND_BREAK, t0); }
    if (tok_is_kw(t0, "continue")) { advance(p); return new_node(ND_CONTINUE, t0); }
    if (tok_is_kw(t0, "pass")) { advance(p); return new_node(ND_PASS, t0); }

    // print( ... ) は暫定の組み込み文。
    // print は予約語ではないので、IDENT "print" の次が '(' かで判定します（2 トークン先読み）。
    if (t0->kind == TK_IDENT && strcmp(t0->text, "print") == 0 &&
        tok_is(peek_at(p, 1), "("))
        return print_stmt(p);

    // IDENT の次が ':' なら変数宣言。2 トークン先読みで判別する。
    if (peek(p)->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) return var_decl(p);

    Node *lhs = expr(p);
    Token *t = peek(p);

    int aug = aug_op(t);
    if (!tok_is(t, "=") && aug < 0) return lhs;  // 代入ではない → 式文

    // ここから代入。左辺が代入先になれるか確認する。
    if (lhs->kind != ND_VAR) {
        Diag d = {0};
        d.message = "この式には代入できません";
        d.primary.tok = lhs->tok;
        d.primary.label = "代入先にできるのは変数だけです";
        d.hint = "添字 xs[0] やフィールド p.f への代入は第10章・第12章で対応します";
        diag_fail(&d);
    }
    advance(p);  // "=" または複合代入記号

    Node *rhs = expr(p);

    // ★ 複合代入は脱糖する（言語仕様 5.2）
    //     x += e  →  x = x + e
    //
    // ⚠️ 左辺を 2 回書くことになります。変数なら 2 回評価しても同じですが、
    //    第10章で xs[f()] += 1 のような形を許すときは
    //    「左辺は 1 回だけ評価」を守る書き換えが必要になります。
    if (aug >= 0) rhs = new_binop_node(t, (OpKind)aug, new_var_node(lhs->tok, lhs->name), rhs);

    Node *n = new_node(ND_ASSIGN, t);
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

// ブロックを開く ':' を要求する
static void expect_colon(Parser *p, const char *what) {
    if (consume(p, ":")) return;

    Diag d = {0};
    d.message = diag_fmt("%sの後に ':' が必要です", what);
    d.primary.tok = peek(p);
    d.primary.label = "ここに ':' が必要です";
    d.hint = "ブロックを開く行は ':' で終わり、次の行を字下げします";
    diag_fail(&d);
}

static Node *stmt(Parser *p);

// block ::= NEWLINE INDENT stmt { stmt } DEDENT
//
// ★ 第4章で字句解析器に合成させた仮想トークンを、ここで初めて消費します。
//   波括弧言語の "{" stmt { stmt } "}" とまったく同じ形になっているのが要点です。
//   オフサイドルールの複雑さは、すべて字句解析器の中に閉じ込められています。
static Node *block(Parser *p) {
    Token *head_tok = peek(p);

    expect(p, TK_NEWLINE, "改行", "':' の後は改行してブロックを字下げしてください");
    expect(p, TK_INDENT, "字下げされたブロック",
           "':' の次の行は字下げしてください（スペース 4 個を推奨）");

    Node head = {0};
    Node *cur = &head;
    // ⚠️ TK_EOF も終了条件に入れる。字句解析器は末尾で DEDENT を必ず出すので
    //    理屈の上では到達しませんが、入れておかないと万一のとき無限ループになります。
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        cur->next = stmt(p);
        cur = cur->next;
    }
    expect(p, TK_DEDENT, "ブロックの終わり", NULL);

    Node *blk = new_node(ND_BLOCK, head_tok);
    blk->body = head.next;
    return blk;
}

// if_stmt ::= "if" expr ":" block { "elif" expr ":" block } [ "else" ":" block ]
//
// ★ elif は「else の中に if が 1 個ある」形に脱糖します。
//
//     if a:   A          if a:  A
//     elif b: B    →     else:
//     else:   C              if b: B
//                            else: C
//
//   ND_ELIF のようなノードが不要になり、意味解析もコード生成も
//   「if は 2 分岐」だけを扱えば済みます。
//   第5章の複合代入（x += e → x = x + e）と同じ発想です。
static Node *if_stmt(Parser *p) {
    // 先頭は "if"（stmt から呼ばれたとき）か "elif"（自分自身から呼ばれたとき）。
    // どちらも「条件 ':' ブロック」という同じ構造なので、同じ関数で読めます。
    Token *t = advance(p);

    Node *n = new_node(ND_IF, t);
    n->lhs = expr(p);
    expect_colon(p, "if の条件");
    n->body = block(p);

    if (tok_is_kw(peek(p), "elif")) {
        n->els = if_stmt(p);  // ★ 再帰 1 行で elif が何個でも繋がる
    } else if (consume_kw(p, "else")) {
        expect_colon(p, "'else'");
        n->els = block(p);
    }
    return n;
}

// while_stmt ::= "while" expr ":" block
static Node *while_stmt(Parser *p) {
    Token *t = advance(p);  // "while"

    Node *n = new_node(ND_WHILE, t);
    n->lhs = expr(p);
    expect_colon(p, "while の条件");
    n->body = block(p);
    return n;
}

// stmt ::= simple_stmt NEWLINE | if_stmt | while_stmt
//
// 第8章で return と def が加わります。
static Node *stmt(Parser *p) {
    Token *t = peek(p);

    if (tok_is_kw(t, "if")) return if_stmt(p);
    if (tok_is_kw(t, "while")) return while_stmt(p);

    // 対応する if が無い elif / else。
    // 放っておいても primary() の「予約語は変数名として使えません」に
    // 捕まりますが、それでは何が悪いのか分かりません。
    if (tok_is_kw(t, "elif") || tok_is_kw(t, "else")) {
        Diag d = {0};
        d.message = diag_fmt("対応する if がない '%s' です", t->text);
        d.primary.tok = t;
        d.primary.label = "この行に対応する 'if' が見つかりません";
        d.hint = "'elif' / 'else' は 'if' と同じ字下げの位置に書いてください";
        diag_fail(&d);
    }

    Node *s = simple_stmt(p);
    expect_newline(p);
    return s;
}

// program ::= { stmt } EOF
//
// ⚠️ 暫定仕様：トップレベルは式文の並びで、
//    プログラムの値は「最後の式の値」になります。
//    第8章で `def main() -> int:` が正式な入口になったら置き換えます。
static Node *program(Parser *p) {
    // ★ 「ダミーの先頭ノード」を使うと、リスト構築が分岐なしで書けます。
    //   head.next が最初の要素になり、「空かどうか」の場合分けが消えます。
    Node head = {0};
    Node *cur = &head;

    Token *first = peek(p);

    while (peek(p)->kind != TK_EOF) {
        // トップレベルにインデントされた行が来た。
        // ブロックを作る構文（if / while / def）はまだ無いので、必ず誤り。
        Token *t = peek(p);
        if (t->kind == TK_INDENT) {
            Diag d = {0};
            d.message = "予期しないインデントです";
            d.primary.tok = t;
            d.primary.label = "この行が余分に字下げされています";
            d.hint = "ブロックを作る構文（if / while / def）は第7章以降で実装します";
            diag_fail(&d);
        }
        if (t->kind == TK_DEDENT) {
            // 字句解析器の不整合。ユーザーのミスでは起こり得ない。
            UNREACHABLE();
        }

        cur->next = stmt(p);
        cur = cur->next;
    }

    if (!head.next) {
        Diag d = {0};
        d.message = "空のプログラムです";
        d.primary.tok = first;
        d.primary.label = "文が 1 つも見つかりません";
        d.hint = "式や変数宣言を 1 行以上書いてください（例: 1 + 2）";
        diag_fail(&d);
    }

    Node *blk = new_node(ND_BLOCK, first);
    blk->body = head.next;
    return blk;
}

// ── 入口 ───────────────────────────────────────────────────

Node *parse(TokenVec toks) {
    Parser p = {.toks = toks, .pos = 0};
    return program(&p);
}
