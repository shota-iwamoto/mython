// lexer.h — 字句解析（① 文字列 → トークン列）
//
// 第4章の範囲：整数リテラル（10/16/8/2 進）・記号・
// 仮想トークン（NEWLINE / INDENT / DEDENT）・EOF。
// 識別子・キーワード・文字列は第5章以降で足していきます。
#ifndef MYTHON_LEXER_H
#define MYTHON_LEXER_H

#include <stdbool.h>

#include "util.h"

typedef enum {
    TK_EOF,    // 入力の終わり
    TK_INT,    // 整数リテラル
    TK_PUNCT,  // 記号（+ - * // ( ) など）

    // ── 仮想トークン（ソース上に対応する文字がない）──
    //
    // ★ 字句解析器が「合成」します。ここが Python 風言語の核心です。
    //   INDENT / DEDENT を波括弧言語の '{' / '}' と同じように扱えるので、
    //   インデント構文の解析が波括弧言語とまったく同じ難しさに落ちます。
    TK_NEWLINE,  // 論理行の終わり
    TK_INDENT,   // ブロックの開始
    TK_DEDENT,   // ブロックの終了

    // ── 第5章以降で追加していく ──
    // TK_IDENT, TK_KEYWORD, TK_STRING, TK_FLOAT,
} TokenKind;

typedef struct Token Token;
struct Token {
    TokenKind kind;

    // ── 位置情報（エラー報告に使う。全トークンが必ず持つ）──
    const char *file;        // ファイル名
    const char *line_start;  // このトークンがある行の先頭
    int line;                // 1 起算の行番号
    int col;                 // 1 起算の桁番号

    // ── ソース上の実体 ──
    const char *loc;  // ソース文字列中の開始位置（複製しない）
    int len;          // バイト長

    // ── 値（kind によって使い分ける）──
    long long ival;  // TK_INT
};

// トークンの可変長配列。
// リンクリストではなく配列にするのは、パーサが peek(2) のような
// 任意の先読みを O(1) でできるようにするためです。
typedef struct {
    Token *data;
    int len;
    int cap;
} TokenVec;

// src を字句解析してトークン列を返す。末尾には必ず TK_EOF が入る。
// src は解析後もトークンから参照されるので、解放してはいけません。
TokenVec tokenize(const char *file, const char *src);

// TokenKind の名前（--dump-tokens 用）
const char *token_kind_name(TokenKind kind);

// トークンが指定した記号そのものか判定する。
//   tok_is(t, "+")  →  t が記号 '+' なら true
// 記号の文字列を Token に複製せず、ソース上の位置と長さで比較します。
bool tok_is(Token *tok, const char *op);

// --dump-tokens の出力
void dump_tokens(TokenVec toks);

// 位置情報付きのエラー報告は diag.h にあります（error_at / diag_fail）。

#endif  // MYTHON_LEXER_H
