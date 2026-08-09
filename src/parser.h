// parser.h — 構文解析（② トークン列 → AST）
//
// 手法は「再帰下降構文解析」です。
// 文法規則 1 つ ＝ static 関数 1 つ に対応させます。
// 文法は docs/spec/grammar.md にあります。
//
// 第1章の文法（暫定）：
//     program ::= expr EOF
//     expr    ::= INT
//
// ⚠️ この文法は正式な言語仕様とは違います。第1章では
//    「パイプラインを端から端まで通す」ことだけを目的にしているためです。
//    第4章で正式な形（def / インデントブロック）に置き換えます。
#ifndef MYTHON_PARSER_H
#define MYTHON_PARSER_H

#include "ast.h"
#include "lexer.h"

// トークン列を構文解析して AST の根を返す
Node *parse(TokenVec toks);

#endif  // MYTHON_PARSER_H
