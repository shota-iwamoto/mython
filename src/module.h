// module.h — モジュール（= 1 ファイル）の読み込み（第13章）
//
// ★ v1 の決めごと：1 ファイル = 1 モジュール = 1 名前空間 = 1 つの .ll
//   （docs/chapters/ch13-modules.md 13.2 節）
//
// 入口のファイルを 1 つ渡すと、import をたどって必要なファイルを全部読み、
// 「依存が先、依存する側が後」の順（トポロジカル順）に並べて返します。
// 循環 import はここで検出してエラーにします。
#ifndef MYTHON_MODULE_H
#define MYTHON_MODULE_H

#include "ast.h"

typedef struct Module Module;
struct Module {
    char *name;   // "lexer"（IR の名前修飾に使う）
    char *path;   // "…/lexer.my"
    char *dir;    // import を探すディレクトリ（入口ファイルのある場所）
    char *src;    // ソース（★ トークンが参照し続けるので解放しない）
    Node *ast;    // 構文解析した結果

    Module **deps;  // import しているモジュール
    int ndeps;

    char *ll_path;  // 出力する .ll のパス（main.c が決める）
    void *syms;     // 意味解析が使うシンボル表（sema.c の ModuleSyms *）

    // 深さ優先探索の状態。★ この 3 値が循環検出そのものです。
    //   0 = 未訪問 / 1 = 訪問中 / 2 = 完了
    int state;

    Module *next;  // 依存順のリスト（依存が先に来る）
};

// 入口ファイルから import をたどって全モジュールを読む。
// 戻り値は依存順に並んだ先頭。*entry_out に入口モジュールを入れる。
Module *load_modules(const char *entry_path, Module **entry_out);

// dir に <name>.my があるか（「import を書き忘れていませんか」の診断用）
bool module_file_exists(const char *dir, const char *name);

#endif  // MYTHON_MODULE_H
