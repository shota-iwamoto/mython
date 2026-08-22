#include "parser.h"

#include <string.h>

#include "diag.h"

// ── パーサの状態 ────────────────────────────────────────────
// トークン配列と「今どこを見ているか」の位置だけを持ちます。
typedef struct {
    TokenVec toks;
    int pos;
    int hidden;  // 脱糖で作る隠し変数の連番（第11章）
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
        case TK_STR: return "文字列";
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
static Node *unary(Parser *p);
static Token *type_name_token(Parser *p, const char *what);
static Node *type_ref(Parser *p, const char *what);

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
    if (t->kind == TK_STR) {
        advance(p);
        return new_str_node(t, t->text, t->slen);
    }

    // リストリテラル [1, 2, 3]（第10章）。
    //
    // ★ '[' の意味は位置で決まります：式の先頭ならリテラル、
    //   式の直後（postfix）なら添字。階層化された文法の副産物で、
    //   特別な処理は要りません。
    if (tok_is(t, "[")) {
        advance(p);
        Node *n = new_node(ND_LIST, t);
        Node head = {0};
        Node *cur = &head;
        if (!tok_is(peek(p), "]")) {
            for (;;) {
                cur->next = expr(p);
                cur = cur->next;
                if (!consume(p, ",")) break;
                if (tok_is(peek(p), "]")) break;  // 末尾のカンマを許す
            }
        }
        expect_close(p, "]", t);
        n->body = head.next;
        return n;
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

// postfix ::= primary { "(" [ arg_list ] ")" }
//
// ★ 呼び出しは「後置演算子」。優先順位は最も強い部類なので、
//   階層のいちばん下（primary のすぐ上）に 1 段挟みます。
//   第10章で xs[0] や p.f を足すときも、このループに追加するだけです。
static Node *postfix(Parser *p) {
    Node *n = primary(p);

    for (;;) {
        Token *open = peek(p);

        // 添字 xs[i]（第10章）
        if (consume(p, "[")) {
            Node *idx = new_node(ND_INDEX, open);
            idx->lhs = n;
            idx->rhs = expr(p);
            expect_close(p, "]", open);
            n = idx;
            continue;
        }

        // メソッド呼び出し xs.append(v)（第10章）と
        // フィールドアクセス t.kind（第12章）。
        //
        // ★ どちらも "." IDENT まで同じ形です。続きが '(' かどうかで分かれます。
        //   ループの中に分岐を 1 個足すだけなので、t.next.kind のような
        //   連鎖も自動的に通ります。
        if (consume(p, ".")) {
            Token *name_tok = peek(p);
            if (name_tok->kind != TK_IDENT)
                error_at_hint(name_tok,
                              "'.' の後にはフィールド名かメソッド名を書きます"
                              "（例: t.kind / xs.append(1)）",
                              "フィールド名かメソッド名が必要です");
            advance(p);

            Token *mopen = peek(p);
            if (!tok_is(mopen, "(")) {
                // フィールドアクセス（第12章。第10章の予告を回収）
                Node *f = new_node(ND_FIELD, name_tok);
                f->lhs = n;
                f->name = name_tok->text;
                n = f;
                continue;
            }
            advance(p);  // "("

            Node *m = new_node(ND_METHOD, name_tok);
            m->lhs = n;
            m->name = name_tok->text;

            Node mhead = {0};
            Node *mcur = &mhead;
            if (!tok_is(peek(p), ")")) {
                for (;;) {
                    mcur->next = expr(p);
                    mcur = mcur->next;
                    if (!consume(p, ",")) break;
                }
            }
            expect_close(p, ")", mopen);
            m->args = mhead.next;
            n = m;
            continue;
        }

        if (!consume(p, "(")) return n;

        // 呼べるのは名前だけ（第一級関数は v1 未対応）
        if (n->kind != ND_VAR) {
            Diag d = {0};
            d.message = "この式は呼び出せません";
            d.primary.tok = n->tok;
            d.primary.label = "呼び出せるのは関数の名前だけです";
            d.hint = "関数を値として扱うことは v1 では対応していません";
            diag_fail(&d);
        }

        Node *call = new_node(ND_CALL, n->tok);
        call->name = n->name;

        Node head = {0};
        Node *cur = &head;
        if (!tok_is(peek(p), ")")) {
            for (;;) {
                cur->next = expr(p);
                cur = cur->next;
                if (!consume(p, ",")) break;
            }
        }
        expect_close(p, ")", open);
        call->args = head.next;
        n = call;
    }
}

// power ::= postfix [ "**" unary ]
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
    Node *base = postfix(p);  // ★ 第8章：primary から postfix に差し替え

    // ★ 第2章に「第9章ではこうなります」と書いておいたとおりの形。
    //   右結合なので、右辺は unary() を再帰で呼びます（2 ** 3 ** 2 = 2 ** 9）。
    Token *t = peek(p);
    if (consume(p, "**")) return new_binop_node(t, OP_POW, base, unary(p));

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
    Node *tr = type_ref(p, "型注釈には型名を書きます（例: x: int = 0）");

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
    n->type_ref = tr;
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

// 脱糖で使う道具（実体は for 文のところにあります。第11章）
static char *hidden_name(Parser *p, const char *tag);
static Node *hidden_decl(Token *tok, char *name, Node *init);

// 代入先と同じ形のノードを、対象を隠し変数に差し替えて作り直す。
//
// ★ 「読み」と「書き」で 2 つ作ります。同じノードを使い回しても動きますが、
//   1 つのノードが木の 2 か所に現れるのは（sema が 2 回検査することになり）
//   後から読む人を必ず混乱させます。
static Node *retarget(Node *target, char *obj, char *idx) {
    Node *n = new_node(target->kind, target->tok);
    n->lhs = new_var_node(target->tok, obj);
    if (target->kind == ND_INDEX) n->rhs = new_var_node(target->tok, idx);
    else n->name = target->name;  // ND_FIELD
    return n;
}

// 複合代入の脱糖（言語仕様 5.2）。
//
//   x += e      →  x = x + e
//
//   t.f += e    →  aug.obj.0 = t              ← 対象は 1 回だけ評価する
//                  aug.obj.0.f = aug.obj.0.f + e
//
//   xs[i] += e  →  aug.obj.0 = xs
//                  aug.idx.1 = i               ← 添字も 1 回だけ
//                  aug.obj.0[aug.idx.1] = aug.obj.0[aug.idx.1] + e
//
// ⚠️ 第5章に「第10章で必要になる」と予告した書き換えです。実際には第12章まで
//    先送りされ、その間 xs[f()] += 1 は**コンパイラを落としていました**
//    （左辺を変数だと決め打ちして name を読んでいたため）。
static Node *aug_assign(Parser *p, Token *t, OpKind op, Node *target, Node *rhs) {
    // 変数はそのまま。2 回評価しても副作用がないので、隠し変数は要りません。
    if (target->kind == ND_VAR) {
        Node *n = new_node(ND_ASSIGN, t);
        n->lhs = target;
        n->rhs = new_binop_node(t, op, new_var_node(target->tok, target->name), rhs);
        return n;
    }

    Node head = {0};
    Node *cur = &head;

    char *obj = hidden_name(p, "aug.obj");
    cur->next = hidden_decl(target->tok, obj, target->lhs);
    cur = cur->next;

    char *idx = NULL;
    if (target->kind == ND_INDEX) {
        idx = hidden_name(p, "aug.idx");
        cur->next = hidden_decl(target->tok, idx, target->rhs);
        cur = cur->next;
    }

    Node *asg = new_node(ND_ASSIGN, t);
    asg->lhs = retarget(target, obj, idx);  // 書き
    asg->rhs = new_binop_node(t, op, retarget(target, obj, idx), rhs);  // 読み
    cur->next = asg;

    // 隠し変数をこの文の中に閉じ込めるため、ブロックで包む（第11章の for と同じ）
    Node *blk = new_node(ND_BLOCK, t);
    blk->body = head.next;
    return blk;
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
static Node *simple_stmt(Parser *p) {
    Token *t0 = peek(p);

    // break / continue / pass
    if (tok_is_kw(t0, "break")) { advance(p); return new_node(ND_BREAK, t0); }
    if (tok_is_kw(t0, "continue")) { advance(p); return new_node(ND_CONTINUE, t0); }
    if (tok_is_kw(t0, "pass")) { advance(p); return new_node(ND_PASS, t0); }

    // return_stmt ::= "return" [ expr ]
    if (tok_is_kw(t0, "return")) {
        advance(p);
        Node *n = new_node(ND_RETURN, t0);
        // 値の有無は「次が改行か」で判断する
        if (peek(p)->kind != TK_NEWLINE) n->lhs = expr(p);
        return n;
    }

    // IDENT の次が ':' なら変数宣言。2 トークン先読みで判別する。
    if (peek(p)->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) return var_decl(p);

    Node *lhs = expr(p);
    Token *t = peek(p);

    int aug = aug_op(t);
    if (!tok_is(t, "=") && aug < 0) {
        // 式文になれるのは呼び出しだけ（docs/spec/type-system.md 6 節）。
        //
        // ★ 第7章までは「プログラムの値＝最後の式」だったので、裸の式を
        //   文として書けました。第8章で足場を外したので本来の厳しさに戻します。
        if (lhs->kind != ND_CALL && lhs->kind != ND_METHOD) {
            Diag d = {0};
            d.message = "この式は文として書けません";
            d.primary.tok = lhs->tok;
            d.primary.label = "計算した値がどこにも使われていません";
            d.hint = "結果を変数に代入するか、関数の呼び出しを書いてください";
            diag_fail(&d);
        }
        return lhs;  // 式文（呼び出し）
    }

    // ここから代入。左辺が代入先になれるか確認する。
    // ★ 第5章の設計どおり、条件を 1 つ足すだけで xs[0] = v に対応できました。
    //   第12章の t.kind = v も、また 1 つ足すだけです。
    if (lhs->kind != ND_VAR && lhs->kind != ND_INDEX && lhs->kind != ND_FIELD) {
        Diag d = {0};
        d.message = "この式には代入できません";
        d.primary.tok = lhs->tok;
        d.primary.label = "代入先にできるのは変数・添字 xs[i]・フィールド t.f だけです";
        d.hint = "計算結果を代入したい場合は、左辺に変数を書いてください";
        diag_fail(&d);
    }
    advance(p);  // "=" または複合代入記号

    Node *rhs = expr(p);

    // ★ 複合代入は脱糖する（言語仕様 5.2）
    //     x += e  →  x = x + e
    //
    // ⚠️ 左辺が「読み」と「書き」の 2 回現れます。変数なら 2 回評価しても
    //    同じですが、xs[f()] += 1 や t.g().f += 1 では f() が 2 回呼ばれます。
    //    第5章に「第10章で必要になる」と書いた書き換えを、ここで実装します。
    if (aug >= 0) return aug_assign(p, t, (OpKind)aug, lhs, rhs);

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

// 脱糖で作る隠し変数の名前。
//
// ⚠️ '.' を含むので、利用者が書ける識別子とは絶対に衝突しません。
//    ネストしても衝突しないよう連番を振ります（シャドーイング禁止のため、
//    同名だと内側の for が「外側を隠しています」というエラーになる）。
static char *hidden_name(Parser *p, const char *tag) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.%d", tag, p->hidden++);
    return sb_str(&sb);
}

// 隠し変数の宣言（型注釈なし = 初期化式の型を使う。第11章）
static Node *hidden_decl(Token *tok, char *name, Node *init) {
    Node *n = new_node(ND_VARDECL, tok);
    n->name = name;
    n->rhs = init;
    return n;  // type_ref は NULL のまま
}

// for_stmt ::= "for" IDENT "in" expr ":" block
//
// ★ この章の主題：for は新しい機能ではなく while の書き換えです。
//   構文解析器で書き換えてしまえば、sema も codegen も for を知らずに済みます。
//   （第5章の複合代入、第7章の elif と同じ「脱糖」の手）
//
//     for x in xs:            for.it.0 = xs          ← 対象は 1 回だけ評価
//         BODY          →     for.ix.0: int = 0
//                             while for.ix.0 < len(for.it.0):
//                                 x = for.it.0[for.ix.0]
//                                 BODY
//                               incr: for.ix.0 += 1  ← continue の飛び先
//
// ⚠️ 脱糖で作るノードにはソース上の位置がないので、
//    for のトークンを流用します（全ノードが tok を持つ約束。第1章）。
static Node *for_stmt(Parser *p) {
    Token *t = advance(p);  // "for"

    Token *var_tok = peek(p);
    if (var_tok->kind != TK_IDENT)
        error_at_hint(var_tok, "for のループ変数は名前で書きます（例: for x in xs:）",
                      "ループ変数の名前が必要です");
    advance(p);

    if (!consume_kw(p, "in"))
        error_at_hint(peek(p), "for は「for 変数 in 対象:」の形で書きます",
                      "'in' が必要です");

    // range(...) は特別扱い（言語仕様 5.5：イテレータプロトコルは作らない）
    bool is_range = peek(p)->kind == TK_IDENT &&
                    strcmp(peek(p)->text, "range") == 0 &&
                    tok_is(peek_at(p, 1), "(");

    Node *iter = NULL, *start = NULL, *stop = NULL;
    long long step = 1;

    if (is_range) {
        advance(p);                 // "range"
        Token *open = advance(p);   // "("

        Node *a1 = expr(p);
        Node *a2 = NULL, *a3 = NULL;
        if (consume(p, ",")) a2 = expr(p);
        if (consume(p, ",")) a3 = expr(p);
        expect_close(p, ")", open);

        if (!a2) {
            start = new_int_node(t, 0);
            stop = a1;
        } else {
            start = a1;
            stop = a2;
        }

        if (a3) {
            // ⚠️ 増分は整数リテラルだけ（v1 の制限）。
            //    符号が実行時に決まると条件式が複雑になります
            //    （(step>0 and i<end) or (step<0 and i>end) を組み立てることになる）。
            //    符号がコンパイル時に分かれば '<' か '>' を選ぶだけで済みます。
            long long sign = 1;
            Node *lit = a3;
            if (lit->kind == ND_UNARY && lit->op == OP_NEG) {
                sign = -1;
                lit = lit->lhs;
            }
            if (lit->kind != ND_INT)
                error_at_hint(a3->tok,
                              "増分は整数リテラルで書いてください（例: range(0, 10, 2)）。"
                              "変数を使いたい場合は while で書けます",
                              "range の増分が定数ではありません");
            step = sign * lit->ival;
            if (step == 0)
                error_at_hint(a3->tok, "増分が 0 だと無限ループになります",
                              "range の増分に 0 は使えません");
        }
    } else {
        iter = expr(p);
    }

    expect_colon(p, "for の対象");
    Node *body = block(p);

    // ── ここから脱糖 ──
    Node head = {0};
    Node *cur = &head;

    char *ix = hidden_name(p, "for.ix");
    Node *cond = NULL;
    Node *bind = NULL;

    if (is_range) {
        // for.ix.N: int = start
        cur->next = hidden_decl(t, ix, start);
        cur = cur->next;

        // 増分の符号で条件の向きが変わる
        cond = new_binop_node(t, step > 0 ? OP_LT : OP_GT, new_var_node(t, ix), stop);

        // x = for.ix.N
        bind = hidden_decl(t, var_tok->text, new_var_node(t, ix));
    } else {
        // for.it.N = <対象>（★ 1 回だけ評価する）
        char *it = hidden_name(p, "for.it");
        cur->next = hidden_decl(t, it, iter);
        cur = cur->next;

        // for.ix.N: int = 0
        cur->next = hidden_decl(t, ix, new_int_node(t, 0));
        cur = cur->next;

        // for.ix.N < len(for.it.N)
        Node *lencall = new_node(ND_CALL, t);
        lencall->name = "len";
        lencall->args = new_var_node(t, it);
        cond = new_binop_node(t, OP_LT, new_var_node(t, ix), lencall);

        // x = for.it.N[for.ix.N]
        Node *idx = new_node(ND_INDEX, t);
        idx->lhs = new_var_node(t, it);
        idx->rhs = new_var_node(t, ix);
        bind = hidden_decl(t, var_tok->text, idx);
    }

    // 本体の先頭にループ変数の束縛を差し込む
    bind->next = body->body;
    body->body = bind;

    // for.ix.N += 1（または step）— ★ continue の飛び先になる
    Node *inc = new_node(ND_ASSIGN, t);
    inc->lhs = new_var_node(t, ix);
    inc->rhs = new_binop_node(t, OP_ADD, new_var_node(t, ix), new_int_node(t, step));

    Node *wh = new_node(ND_WHILE, t);
    wh->lhs = cond;
    wh->body = body;
    wh->incr = inc;

    cur->next = wh;

    // 隠し変数を for 文のスコープに閉じ込めるため、ブロックで包む
    Node *blk = new_node(ND_BLOCK, t);
    blk->body = head.next;
    return blk;
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
    if (tok_is_kw(t, "for")) return for_stmt(p);

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

// 型注釈に書ける名前を 1 つ読む。
//
// ⚠️ 'None' は予約語（TK_KEYWORD）なので、IDENT だけを受け付けると弾かれます。
//    型名として書ける予約語は今のところ None だけです。
static Token *type_name_token(Parser *p, const char *what) {
    Token *t = peek(p);
    if (t->kind == TK_IDENT || tok_is_kw(t, "None")) {
        advance(p);
        return t;
    }
    error_at_hint(t, what, "型名が必要です");
}

// type_ref ::= [ IDENT "." ] IDENT [ "[" type_ref "]" ]
//
// ★ 第10章：型注釈が木になりました。list[list[int]] のように入れ子になるので、
//   文字列 1 個では表せません。再帰下降なら再帰 1 行で読めます。
// ★ 第13章：モジュール修飾（lexer.Token）が書けるようになりました。
static Node *type_ref(Parser *p, const char *what) {
    Token *t = type_name_token(p, what);

    Node *n = new_node(ND_TYPEREF, t);
    n->name = t->text;

    // モジュール修飾 lexer.Token（★ 1 段だけ。a.b.Token は書けない）
    if (tok_is(peek(p), ".")) {
        advance(p);
        Token *m = type_name_token(p, "モジュール修飾の後には型名を書きます"
                                      "（例: lexer.Token）");
        n->mod_name = n->name;
        n->name = m->text;
        n->tok = m;
        if (tok_is(peek(p), "."))
            error_at_hint(peek(p),
                          "モジュールの修飾は 1 段だけです（パッケージはありません）",
                          "'%s.%s' の後にさらに '.' は書けません", n->mod_name,
                          n->name);
    }

    Token *open = peek(p);
    if (consume(p, "[")) {
        n->lhs = type_ref(p, "要素型を書いてください（例: list[int]）");
        expect_close(p, "]", open);
    }
    return n;
}

// param ::= IDENT ":" type
//
// ★ 第12章：メソッドの第 1 引数 self だけは型注釈を書きません
//   （そのクラスに決まっているので、書かせても意味がない）。
//   型は sema が入れます。第11章の「型注釈のない ND_VARDECL」と同じ抜け道で、
//   利用者が書くふつうの引数は今までどおり型注釈が必須です。
static Node *param(Parser *p, bool allow_self) {
    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "引数は「名前: 型」の形で書きます（例: n: int）",
                      "引数名が必要です");
    advance(p);

    if (allow_self && strcmp(name_tok->text, "self") == 0 &&
        !tok_is(peek(p), ":")) {
        Node *n = new_node(ND_PARAM, name_tok);
        n->name = name_tok->text;
        return n;  // type_ref は NULL のまま（sema がクラスの型を入れる）
    }

    if (!consume(p, ":"))
        error_at_hint(peek(p), "引数には型注釈が必須です（例: n: int）",
                      "引数名の後に ':' が必要です");

    Node *tr = type_ref(p, "引数には型注釈が必須です（例: n: int）");

    Node *n = new_node(ND_PARAM, name_tok);
    n->name = name_tok->text;
    n->type_ref = tr;
    return n;
}

// func_def ::= "def" IDENT "(" [ param_list ] ")" "->" type ":" block
//
// in_class … クラス本体の中か（第12章。true なら第 1 引数に self を書ける）
static Node *func_def(Parser *p, bool in_class) {
    Token *kw = advance(p);  // "def"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "def の後には関数名を書きます（例: def f() -> int:）",
                      "関数名が必要です");
    advance(p);

    Node *n = new_node(ND_FUNC, kw);
    n->name = name_tok->text;

    Token *open = peek(p);
    if (!consume(p, "("))
        error_at_hint(open, "関数名の後には引数リストが必要です（例: f() や f(n: int)）",
                      "'(' が必要です");

    Node head = {0};
    Node *cur = &head;
    if (!tok_is(peek(p), ")")) {
        for (;;) {
            // self を書けるのは「クラス本体の中の、第 1 引数」だけ
            cur->next = param(p, in_class && cur == &head);
            cur = cur->next;
            if (!consume(p, ",")) break;
        }
    }
    expect_close(p, ")", open);
    n->params = head.next;

    // メソッドの第 1 引数は self でなければならない（言語仕様 5.10）。
    // ⚠️ ここで弾いておけば、sema は「メソッドの第 1 引数は self」と仮定できます。
    if (in_class && (!n->params || strcmp(n->params->name, "self") != 0)) {
        Diag d = {0};
        d.message = diag_fmt("メソッド '%s' の第 1 引数は self でなければなりません",
                             n->name);
        d.primary.tok = n->params ? n->params->tok : name_tok;
        d.primary.label = "ここに self が必要です";
        d.hint = "Mython は self を明示的に書きます"
                 "（例: def show(self) -> None:）";
        diag_fail(&d);
    }

    // 戻り型は必須（言語仕様 3.3）。
    // 🤔 省略を許すと再帰関数で「戻り型を知るには本体が要り、
    //    本体を見るには戻り型が要る」という循環に陥ります。
    if (!consume(p, "->"))
        error_at_hint(peek(p),
                      "戻り型は省略できません。値を返さないなら -> None と書きます",
                      "'->' と戻り型が必要です");

    n->type_ref = type_ref(p, "戻り型には型名を書きます（例: -> int / -> None）");

    expect_colon(p, "def の宣言");
    n->body = block(p);
    return n;
}

// field_decl ::= IDENT ":" type NEWLINE
//
// ★ 変数宣言（var_decl）とよく似ていますが、初期化式を取りません。
//   フィールドの初期値は init メソッドで決めます（12.6 節）。
static Node *field_decl(Parser *p) {
    Token *name_tok = advance(p);  // IDENT（呼び出し元が確認済み）
    advance(p);                    // ":"

    Node *n = new_node(ND_FIELDDECL, name_tok);
    n->name = name_tok->text;
    n->type_ref = type_ref(p, "フィールドには型注釈が必須です（例: kind: int）");

    if (tok_is(peek(p), "=")) {
        Diag d = {0};
        d.message = "フィールドに初期値は書けません";
        d.primary.tok = peek(p);
        d.primary.label = "ここに '=' は書けません";
        d.hint = "初期値は init メソッドで代入してください"
                 "（例: def init(self) -> None: / self.kind = 0）";
        diag_fail(&d);
    }
    expect_newline(p);
    return n;
}

// class_def ::= "class" IDENT ":" NEWLINE INDENT { field_decl } { func_def } DEDENT
//
// ★ block() を使わないのは、クラス本体が「文の列」ではなく「宣言の列」だからです
//   （トップレベルと同じ性質）。だから字下げの処理も自前で書きます。
static Node *class_def(Parser *p) {
    Token *kw = advance(p);  // "class"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "class の後にはクラス名を書きます（例: class Token:）",
                      "クラス名が必要です");
    advance(p);

    Node *n = new_node(ND_CLASS, kw);
    n->name = name_tok->text;

    expect_colon(p, "class の宣言");
    expect(p, TK_NEWLINE, "改行", "':' の後は改行してクラス本体を字下げしてください");
    expect(p, TK_INDENT, "字下げされたクラス本体",
           "クラスの中身は字下げして書きます（スペース 4 個を推奨）");

    // ★ フィールドとメソッドを 1 本のリストにまとめます。
    //   program() がトップレベルで def とグローバル変数を混ぜているのと同じ形です。
    Node head = {0};
    Node *cur = &head;
    Node *first_method = NULL;

    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        Token *t = peek(p);

        if (tok_is_kw(t, "def")) {
            cur->next = func_def(p, true);  // ★ true = self を書ける
            cur = cur->next;
            if (!first_method) first_method = cur;
            continue;
        }

        if (t->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) {
            // ⚠️ フィールドはメソッドより先（文法がそう決めている）。
            //    レイアウトを確定してからメソッドを型検査したいためです。
            if (first_method) {
                Diag d = {0};
                d.message = "フィールドはメソッドより前に書いてください";
                d.primary.tok = t;
                d.primary.label = "このフィールド宣言がメソッドより後ろにあります";
                d.related.tok = first_method->tok;
                d.related.label = "最初のメソッドはここです";
                d.hint = "クラス本体は「フィールドを全部 → メソッドを全部」の順です"
                         "（文法定義 3 節）";
                diag_fail(&d);
            }
            cur->next = field_decl(p);
            cur = cur->next;
            continue;
        }

        Diag d = {0};
        d.message = "クラスの中に書けるのはフィールドとメソッドだけです";
        d.primary.tok = t;
        d.primary.label = "ここには書けません";
        d.hint = "フィールドは「名前: 型」、メソッドは 'def' で始めます:\n"
                 "             class Token:\n"
                 "                 kind: int\n"
                 "\n"
                 "                 def show(self) -> None:\n"
                 "                     print(self.kind)";
        diag_fail(&d);
    }
    expect(p, TK_DEDENT, "クラス本体の終わり", NULL);

    n->body = head.next;
    return n;
}

// program ::= { class_def | func_def | global_var | NEWLINE } EOF
//
// ★ 第8章でトップレベルが「宣言だけ」になりました（言語仕様 6.3）。
//   実行文は書けません。プログラムの入口は def main() -> int: です。
// extern_def ::= "extern" "def" IDENT "(" [ param_list ] ")" "->" type NEWLINE
//
// ★ 新しいノード種別は作りません。ND_FUNC の body が NULL——
//   「宣言はあるが定義がない」という意味そのものです（第14章）。
static Node *extern_def(Parser *p) {
    advance(p);  // "extern"

    if (!tok_is_kw(peek(p), "def"))
        error_at_hint(peek(p), "extern の後には def を書きます"
                               "（例: extern def my_system(cmd: str) -> int）",
                      "extern の後に def が必要です");
    advance(p);  // "def"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "extern def の後には C の関数名を書きます",
                      "関数名が必要です");
    advance(p);

    Token *open = peek(p);
    if (!consume(p, "("))
        error_at_hint(peek(p), "引数リストを書いてください（例: (path: str)）",
                      "'(' が必要です");

    Node head = {0};
    Node *cur = &head;
    if (!tok_is(peek(p), ")")) {
        for (;;) {
            cur->next = param(p, false);
            cur = cur->next;
            if (!consume(p, ",")) break;
        }
    }
    expect_close(p, ")", open);

    if (!consume(p, "->"))
        error_at_hint(peek(p), "戻り型を書いてください（例: -> int）",
                      "'->' が必要です");

    Node *n = new_node(ND_FUNC, name_tok);
    n->name = name_tok->text;
    n->params = head.next;
    n->type_ref = type_ref(p, "戻り型を書いてください（例: -> int）");

    // ⚠️ 本体は読みません。':' を書いていたらここで気づけるようにします。
    if (tok_is(peek(p), ":"))
        error_at_hint(peek(p), "extern 宣言は本体を持ちません（改行で終わります）",
                      "extern def に ':' は書けません");
    expect_newline(p);

    n->body = NULL;  // ★ extern の印
    return n;
}

// import_stmt ::= "import" IDENT NEWLINE
//
// ★ 新しいノード種別はこの 1 つだけです。
//   式の側（lexer.make(1) / lexer.MAX）は第12章の postfix() がそのまま読みます。
//   「'.' の左がモジュールかどうか」は名前解決の話なので sema が決めます（13.3 節）。
static Node *import_stmt(Parser *p) {
    Token *kw = advance(p);  // "import"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok,
                      "import の後にはモジュール名を書きます（例: import lexer）",
                      "モジュール名が必要です");
    advance(p);

    if (tok_is(peek(p), "."))
        error_at_hint(peek(p),
                      "モジュール名にドットは使えません（パッケージはありません）",
                      "'%s' の後に '.' は書けません", name_tok->text);

    Node *n = new_node(ND_IMPORT, kw);
    n->name = name_tok->text;
    expect_newline(p);
    return n;
}

static Node *program(Parser *p) {
    // ★ 「ダミーの先頭ノード」を使うと、リスト構築が分岐なしで書けます。
    //   head.next が最初の要素になり、「空かどうか」の場合分けが消えます。
    Node head = {0};
    Node *cur = &head;

    Token *first = peek(p);

    while (peek(p)->kind != TK_EOF) {
        Token *t = peek(p);

        if (t->kind == TK_INDENT) {
            Diag d = {0};
            d.message = "予期しないインデントです";
            d.primary.tok = t;
            d.primary.label = "この行が余分に字下げされています";
            d.hint = "トップレベルに書けるのは def / class / import / extern とグローバル変数だけです"
                     "（言語仕様 6.3）";
            diag_fail(&d);
        }
        if (t->kind == TK_DEDENT) {
            // 字句解析器の不整合。ユーザーのミスでは起こり得ない。
            UNREACHABLE();
        }

        if (tok_is_kw(t, "def")) {
            cur->next = func_def(p, false);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "class")) {  // 第12章
            cur->next = class_def(p);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "import")) {  // 第13章
            cur->next = import_stmt(p);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "extern")) {  // 第14章
            cur->next = extern_def(p);
            cur = cur->next;
            continue;
        }

        // グローバル変数 ::= IDENT ":" type "=" expr NEWLINE
        if (t->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) {
            cur->next = var_decl(p);
            cur = cur->next;
            expect_newline(p);
            continue;
        }

        // それ以外はトップレベルに書けない
        Diag d = {0};
        d.message = "トップレベルに実行文は書けません";
        d.primary.tok = t;
        d.primary.label = "ここに書けるのは def / class / import / extern とグローバル変数だけです";
        d.hint = "処理は main の中に書いてください:\n"
                 "             def main() -> int:\n"
                 "                 ...\n"
                 "                 return 0";
        diag_fail(&d);
    }

    Node *blk = new_node(ND_BLOCK, first);
    blk->body = head.next;
    return blk;
}

// ── 入口 ───────────────────────────────────────────────────

Node *parse(TokenVec toks) {
    Parser p = {.toks = toks, .pos = 0, .hidden = 0};
    return program(&p);
}
