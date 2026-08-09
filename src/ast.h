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
    ND_INT,    // 整数リテラル → ival
    ND_BINOP,  // 二項演算   → op, lhs, rhs
    ND_UNARY,  // 単項演算   → op, lhs
    ND_BLOCK,  // 文のリスト → body（next で連結）
    // ── 第5章以降で追加していく ──
    // ND_VAR, ND_CALL, ND_IF, ND_WHILE, ND_RETURN, ND_FUNC, ...
} NodeKind;

// 演算子の種類。
// トークンの文字列（"+"）ではなくこの enum で持つことで、
// コード生成の switch がコンパイラにチェックされるようになります。
typedef enum {
    // 二項
    OP_ADD,       // +
    OP_SUB,       // -
    OP_MUL,       // *
    OP_TRUEDIV,   // /   ← int には使えない（// を使う）。仕様 4.2
    OP_FLOORDIV,  // //
    OP_MOD,       // %
    OP_BITAND,    // &
    OP_BITOR,     // |
    OP_BITXOR,    // ^
    OP_SHL,       // <<
    OP_SHR,       // >>
    // 単項
    OP_NEG,     // -x
    OP_POS,     // +x
    OP_BITNOT,  // ~x
} OpKind;

// 演算子の記号（エラーメッセージと --dump-ast 用）
const char *op_symbol(OpKind op);

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
    OpKind op;       // ND_BINOP / ND_UNARY

    // ── 子ノード ──
    // 単項演算は lhs だけを使います（rhs は NULL）。
    Node *lhs, *rhs;

    // ND_BLOCK の中身（先頭の文）。以降は next でたどります。
    Node *body;

    // ── 兄弟ノード ──
    //
    // ★ 文のリストは「配列」ではなく「next で繋いだ単方向リスト」にします。
    //
    //   🤔 トークンは配列にしたのに、なぜ文はリストなのか？
    //      トークンは任意の位置に O(1) でアクセスしたい（先読みのため）。
    //      文は「先頭から順に 1 回たどる」だけなので、リストで十分です。
    //      リストなら要素数を先に数える必要がなく、追加が簡単になります。
    Node *next;
};

// コンストラクタ
Node *new_node(NodeKind kind, Token *tok);
Node *new_int_node(Token *tok, long long value);
Node *new_binop_node(Token *tok, OpKind op, Node *lhs, Node *rhs);
Node *new_unary_node(Token *tok, OpKind op, Node *operand);

// AST を S 式で標準出力に表示する（--dump-ast 用）。
// テキストとして比較できる形にしておくのが重要です。
// 第17章でMython 版パーサの出力と diff するときに、この形式が正解になります。
void dump_ast(Node *node);

#endif  // MYTHON_AST_H
