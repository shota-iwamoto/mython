// codegen.h — コード生成（④ AST → LLVM IR テキスト）
//
// 生成規約は docs/design/ir-conventions.md にあります。
// 特に重要な規約：
//   R1  ローカル変数はすべて entry ブロックで alloca する
//   R4  一時値には英字始まりの名前を付ける（%t0）
//   R6  すべての基本ブロックは終端命令で終わる
//   R11 target triple を必ず出力する
#ifndef MYTHON_CODEGEN_H
#define MYTHON_CODEGEN_H

#include "ast.h"

// AST から LLVM IR テキストを生成して返す。
//   source_name : source_filename に書くファイル名（デバッグ情報用）
char *codegen(Node *ast, const char *source_name);

#endif  // MYTHON_CODEGEN_H
