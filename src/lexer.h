// lexer.h — 字句解析（① 文字列 → トークン列）
//
// 第1章の範囲：整数リテラルと EOF のみ。
// 記号・識別子・キーワード・INDENT/DEDENT は第2章以降で足していきます。
#ifndef MYTHON_LEXER_H
#define MYTHON_LEXER_H

#include "util.h"

typedef enum {
    TK_EOF,   // 入力の終わり
    TK_INT,   // 整数リテラル
    // ── 第2章以降で追加していく ──
    // TK_PUNCT, TK_IDENT, TK_KEYWORD, TK_STRING, TK_FLOAT,
    // TK_NEWLINE, TK_INDENT, TK_DEDENT,
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

// --dump-tokens の出力
void dump_tokens(TokenVec toks);

// 位置情報付きエラー。util.h の error_at_pos() に Token を渡すラッパ。
_Noreturn void error_at(Token *tok, const char *fmt, ...);

#endif  // MYTHON_LEXER_H
