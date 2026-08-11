// types.h — 型の表現
//
// 第10章の範囲：int / bool / None / str / list[T]。
// str / float は第9章、list / class は第10章・第12章で足します。
//
// 型付け規則の全体像は docs/spec/type-system.md にあります。
#ifndef MYTHON_TYPES_H
#define MYTHON_TYPES_H

#include <stdbool.h>

typedef enum {
    TY_INT,   // int  → i64
    TY_BOOL,  // bool → i1（メモリ上は i8。規約 R5）
    TY_NONE,  // None → void（値を返さない。メモリ上の表現は無い）
    TY_STR,   // str  → ptr（参照型。第9章）
    TY_LIST,  // list[T] → ptr（複合型。第10章）
    // ── 以降の章で追加していく ──
    // TY_FLOAT, TY_STR, TY_NONE,   // 第9章
    // TY_LIST,   // 第10章
    // TY_CLASS,  // 第12章
} TypeKind;

typedef struct Type Type;
struct Type {
    TypeKind kind;

    // list[T] の要素型（第10章）。
    // ★ ここが埋まる型はシングルトンにできません（T ごとに違うため）。
    Type *elem;

    // ── 以降の章で使うフィールド ──
    // Type *elem;    // list[T] の要素型（第10章）
    // char *name;    // class 名（第12章）
    // bool nullable; // T | None（第12章）
};

// ★ プリミティブ型はシングルトン（起動時に 1 個だけ作る）。
//
// 🤔 なぜシングルトンにするのか
//   `int` 型のオブジェクトを毎回 xmalloc するのは無駄です。
//   1 個だけ作ってポインタを共有すれば、確保が減るうえに
//   「プリミティブ型どうしの比較はポインタ比較で済む」という利点も得られます。
//   ただし将来 list[int] のような複合型が入るので、
//   型の比較は必ず type_equal() を通します（== で直接比べない）。
extern Type *ty_int;
extern Type *ty_bool;
extern Type *ty_none;
extern Type *ty_str;

// プリミティブ型のシングルトンを作る。main の最初に 1 回だけ呼ぶ。
void types_init(void);

// 2 つの型が同じか。
// 第10章で list の要素型の再帰比較を足します。
bool type_equal(Type *a, Type *b);

// エラーメッセージ用の型名（"int" など）
const char *type_name(Type *t);

// 型注釈の名前から型を引く。未知の名前なら NULL。
//   "int" → ty_int
Type *type_from_name(const char *name);

// TypeKind からシングルトンを引く（組み込み関数の表で使う。第9章）
Type *type_from_kind(int kind);

// list[T] を作る（第10章）。
// ⚠️ シングルトンではありません。書かれた場所ごとに新しく作られるので、
//    型の比較は必ず type_equal() を通すこと。
Type *type_list(Type *elem);

// 型注釈に書ける名前の一覧（エラーメッセージのヒスト用）。
// 例: "int, bool"
const char *type_name_list(void);

#endif  // MYTHON_TYPES_H
