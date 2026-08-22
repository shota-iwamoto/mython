// types.h — 型の表現
//
// 第12章の範囲：int / bool / None / str / list[T] / ユーザー定義クラス。
// float は将来、T | None（nullable）は第15章で足します。
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
    TY_CLASS, // ユーザー定義クラス → ptr（参照型。第12章）

    // ── 第15章：nullable ──
    //
    // ★ 当初は Type に bool nullable を足す設計でした（type-system.md 2 節）。
    //   やめた理由は「書き忘れたときにどうなるか」です。フラグ方式だと
    //   `if (t->kind == TY_CLASS)` という既存の判定を Token | None が
    //   すり抜け、None のときに壊れます。種類を分ければ、判定を書き忘れた
    //   場所は「型 'Token | None' にフィールドはありません」で止まります。
    TY_OPT,   // T | None → ptr（elem が中身の型）
    TY_NULL,  // None リテラルの型。変数の型にはならない（代入互換だけで使う）

    // ── 以降の章で追加していく ──
    // TY_FLOAT,  // float
} TypeKind;

// クラス定義の実体は ast.h にあります（フィールドの並びとメソッドを持つため）。
// ★ 型そのものは「どのクラスか」を指せれば十分なので、ここでは前方宣言だけ。
struct Class;

typedef struct Type Type;
struct Type {
    TypeKind kind;

    // list[T] の要素型（第10章）。
    // ★ ここが埋まる型はシングルトンにできません（T ごとに違うため）。
    Type *elem;

    // ── class 用（第12章）──
    char *name;         // クラス名（エラーメッセージ用）
    struct Class *cls;  // 定義への参照。★ 型の同一性はこのポインタで判定する

    // ── 第15章 ──
    // この型の「T | None」版（1 個だけ作ってここに覚えておく）
    Type *opt;
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
extern Type *ty_null;  // None リテラルの型（第15章）

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

// T | None を作る（第15章）。
// ★ 同じ T に対しては 1 個だけ作ります（elem 側にキャッシュする）。
Type *type_opt(Type *elem);

// nullable にできる型か（参照型だけ。int | None は書けない）
bool type_can_be_opt(Type *t);

// T | None なら中身の型、そうでなければ自分自身を返す
Type *type_strip_opt(Type *t);

// S の値を T の場所に置けるか（代入互換性。type-system.md 4 節）。
//
// ★ 第15章：ここが type_equal と分かれました。
//   T → T | None は許し、T | None → T は許しません（一方向）。
bool type_assignable(Type *from, Type *to);

// クラスの型を作る（第12章）。
// ★ list[T] と違い、クラス定義ごとに 1 個だけ作ります
//   （`Token` と書かれた型注釈は、すべて同じ Type * を指す）。
Type *type_class(char *name, struct Class *cls);

// 値のバイト数とアラインメント（第12章。クラスのレイアウト計算に使う）。
// docs/design/memory-model.md 5 節の表がそのまま実装になっています。
int type_size(Type *t);
int type_align(Type *t);

// 型注釈に書ける名前の一覧（エラーメッセージのヒスト用）。
// 例: "int, bool"
const char *type_name_list(void);

#endif  // MYTHON_TYPES_H
