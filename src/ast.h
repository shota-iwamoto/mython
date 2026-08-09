// ast.h — 抽象構文木（AST）のノード定義
//
// 第1章の範囲：整数リテラルノードのみ。
//
// 設計方針：全ノード種別を 1 つの構造体 Node で表します。
// 美しくはありませんが、C で共用体を安全に扱うより読みやすく、
// chibicc / tcc など実績ある小型 C コンパイラと同じ方式です。
// 詳細は docs/design/architecture.md 3.2 節。
#ifndef MYTHON_AST_H
#define MYTHON_AST_H

#include "lexer.h"

typedef enum {
    ND_INT,  // 整数リテラル → ival
    // ── 第2章以降で追加していく ──
    // ND_BINOP, ND_UNARY, ND_VAR, ND_CALL, ...
    // ND_IF, ND_WHILE, ND_RETURN, ND_FUNC, ...
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;

    // このノードの代表トークン。
    // ★ エラー報告に必須なので、全ノードが必ず持ちます。
    //   「型が合いません」のようなエラーで位置を示せるかどうかは、
    //   これを最初から持たせているかで決まります。
    Token *tok;

    // 型（第5章で sema が埋める。それまでは常に NULL）
    // struct Type *type;

    // ── 値 ──
    long long ival;  // ND_INT

    // ── 子ノード（第2章以降）──
    // Node *lhs, *rhs;
    // Node *next;
};

// コンストラクタ
Node *new_node(NodeKind kind, Token *tok);
Node *new_int_node(Token *tok, long long value);

// AST を S 式で標準出力に表示する（--dump-ast 用）。
// テキストとして比較できる形にしておくのが重要です。
// 第17章でMython 版パーサの出力と diff するときに、この形式が正解になります。
void dump_ast(Node *node);

#endif  // MYTHON_AST_H
