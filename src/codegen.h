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
#include "module.h"

// モジュール 1 つぶんの LLVM IR テキストを生成して返す（第13章）。
//   main_ir_name : 入口モジュールなら「Mython の main の IR 名」。
//                  他のモジュールでは NULL（C の main を出すのは入口だけ）。
char *codegen(Module *mod, const char *main_ir_name);

#endif  // MYTHON_CODEGEN_H
