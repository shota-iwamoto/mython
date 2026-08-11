// ast.h — 抽象構文木（AST）のノード定義
//
// 第6章の範囲：リテラル・二項/単項演算・比較・論理演算・変数・代入・ブロック。
//
// 設計方針：全ノード種別を 1 つの構造体 Node で表します。
// 美しくはありませんが、C で共用体を安全に扱うより読みやすく、
// chibicc / tcc など実績ある小型 C コンパイラと同じ方式です。
// 詳細は docs/design/architecture.md 3.2 節。
#ifndef MYTHON_AST_H
#define MYTHON_AST_H

#include <stdbool.h>

#include "lexer.h"
#include "types.h"

typedef enum {
    ND_INT,      // 整数リテラル → ival
    ND_BOOL,     // 真偽値リテラル True / False → ival（0 / 1）
    ND_STR,      // 文字列リテラル → sval, slen（第9章）
    ND_BINOP,    // 二項演算（比較を含む）→ op, lhs, rhs
    ND_LOGICAL,  // and / or     → op, lhs, rhs
                 // ★ 見た目は二項演算だが「右辺を評価しないことがある」ため
                 //   コード生成がまったく違う。だからノード種別を分ける。
                 //   ノード種別は「構文の形」ではなく「生成のしかた」で分ける。
    ND_UNARY,    // 単項演算（not を含む）→ op, lhs
    ND_VAR,      // 変数参照     → name
    ND_VARDECL,  // 変数宣言 x: T = e → name, type_name, rhs
    ND_ASSIGN,   // 代入 x = e   → lhs（ND_VAR）, rhs
    ND_BLOCK,    // 文のリスト   → body（next で連結）

    // ── 第7章：制御構文 ──
    ND_IF,        // if 文    → lhs（条件）, body（then）, els（else）
    ND_WHILE,     // while 文 → lhs（条件）, body
    ND_BREAK,     // break
    ND_CONTINUE,  // continue
    ND_PASS,      // pass（何もしない）
    ND_PRINT,     // print(e) → lhs
                  // ⚠️ 暫定。第8章で本物の関数呼び出しに置き換わります。

    // ── 第8章：関数 ──
    ND_FUNC,    // 関数定義 → name, params, type_name（戻り型）, body
    ND_PARAM,   // 仮引数   → name, type_name
    ND_CALL,    // 呼び出し → name, args
    ND_RETURN,  // return   → lhs（値なしなら NULL）

    // ── 第10章：list[T] ──
    ND_TYPEREF,  // 型注釈    → name, lhs（要素型の型注釈。無ければ NULL）
    ND_LIST,     // [1, 2, 3] → body（要素。next で連結）
    ND_INDEX,    // xs[i]     → lhs（対象）, rhs（添字）
    ND_METHOD,   // xs.f(...) → lhs（対象）, name, args

    // ── 第12章以降で追加していく ──
    // ND_FIELD, ND_CLASS, ...
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
    OP_POW,       // **  （第9章。ランタイムの my_ipow を呼ぶ）

    // 比較（結果は bool）
    // ⚠️ この 6 つは連続して並べること。is_compare() が範囲で判定します。
    OP_EQ,  // ==
    OP_NE,  // !=
    OP_LT,  // <
    OP_LE,  // <=
    OP_GT,  // >
    OP_GE,  // >=

    // 論理（ND_LOGICAL。短絡評価する）
    OP_AND,  // and
    OP_OR,   // or

    // 単項
    OP_NEG,     // -x
    OP_POS,     // +x
    OP_BITNOT,  // ~x
    OP_NOT,     // not x
} OpKind;

// 比較演算子か。
// ★ enum の並び順に依存しています（OP_EQ 〜 OP_GE が連続していること）。
//   switch で 6 個並べるより短く、演算子を足したときの書き忘れも起きません。
static inline bool is_compare(OpKind op) { return OP_EQ <= op && op <= OP_GE; }

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

    // ★ この式の型。**意味解析パス (sema.c) が埋めます。**
    //   構文解析の直後は必ず NULL です。
    //   codegen は sema が埋めたこの型を見て命令を選びます。
    Type *type;

    // ── 値 ──
    long long ival;  // ND_INT / ND_BOOL
    char *sval;      // ND_STR（エスケープ解決済み）
    int slen;        // ND_STR のバイト長
    OpKind op;       // ND_BINOP / ND_UNARY

    // 名前。
    //   ND_VAR / ND_VARDECL : 変数名
    char *name;

    // LLVM 上の名前（%x, %x.1, ...）。★ 意味解析パスが割り当てます。
    //
    // 🤔 なぜ name をそのまま使わないのか（第7章）
    //   第5章では「シャドーイング禁止なので変数名は一意」でしたが、
    //   ブロックスコープが入ると兄弟スコープが同じ名前を使えます。
    //     if a:
    //         x: int = 1      ← %x
    //     if b:
    //         x: int = 2      ← %x（衝突！）
    //   どちらも相手を隠していないのでシャドーイングではありません。
    //   そこで sema が衝突しない名前を割り当てます（名前修飾の入口）。
    char *ir_name;

    // このノードがグローバル変数か（第8章）。
    // グローバルは alloca せず、@g.x を直接読み書きします。
    bool is_global;

    // 型注釈の木（ND_VARDECL / ND_PARAM / ND_FUNC）。第10章。
    // ★ list[list[int]] のように入れ子になるので、文字列 1 個では表せません。
    Node *type_ref;

    // 型注釈に書かれた名前（ND_TYPEREF）。
    // 「int」のような文字列で、sema が Type * に解決します。
    //
    // 🤔 なぜ parser が Type * に解決しないのか
    //   名前から型への解決は「意味」の話であって「構文」の話ではありません。
    //   parser は「そこに識別子が書かれている」ことだけを記録し、
    //   それが有効な型かどうかの判断は sema に任せます。
    //   パスの責務を混ぜないための分離です。
    char *type_name;

    // ── 子ノード ──
    // 単項演算は lhs だけを使います（rhs は NULL）。
    Node *lhs, *rhs;

    // ND_BLOCK の中身（先頭の文）。以降は next でたどります。
    // ND_IF の then 節、ND_WHILE の本体もここです。
    Node *body;

    // ND_CALL が組み込み関数のとき、sema が選んだ候補（第9章）。
    // ユーザー定義関数の呼び出しなら NULL。
    const struct Builtin_ *builtin;

    // ND_FUNC の仮引数リスト / ND_CALL の実引数リスト（next で連結）。第8章
    Node *params;
    Node *args;

    // ND_IF の else 節（第7章）。
    // elif は「else の中の if」に脱糖するので、ND_BLOCK か ND_IF が入ります。
    Node *els;

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
Node *new_bool_node(Token *tok, bool value);
Node *new_str_node(Token *tok, char *bytes, int len);
Node *new_binop_node(Token *tok, OpKind op, Node *lhs, Node *rhs);
Node *new_logical_node(Token *tok, OpKind op, Node *lhs, Node *rhs);
Node *new_unary_node(Token *tok, OpKind op, Node *operand);
Node *new_var_node(Token *tok, char *name);

// AST を S 式で標準出力に表示する（--dump-ast 用）。
// テキストとして比較できる形にしておくのが重要です。
// 第17章でMython 版パーサの出力と diff するときに、この形式が正解になります。
void dump_ast(Node *node);

#endif  // MYTHON_AST_H
