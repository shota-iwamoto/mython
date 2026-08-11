# 第10章 list[T]（動的配列）

> **この章のゴール**
> 可変長のリストが使えるようになる。**そして第5章の予告が現実になる。**
>
> ```bash
> $ cat t.my
> def main() -> int:
>     xs: list[int] = []
>     xs.append(10)
>     xs.append(20)
>     print(xs[0] + xs[1])
>     print(len(xs))
>     return 0
> $ ./build/mythonc t.my -o t && ./t
> 30
> 2
> ```

**この章は「型が構造を持つ」最初の章です。**

第9章までの型は `int` / `bool` / `str` / `None` の 4 つで、**すべてシングルトン**でした。
`list[int]` は違います。**型が型を含みます。**

```
int          … 1 個だけ作ればいい
list[int]    … 書かれた場所ごとに作られる
list[list[int]] … 入れ子になる
```

**★ 第5章で書いた警告が、ついに効きます。**

> **⚠️ それでも `type_equal()` を必ず通す**
> `a == b` で直接比較したくなりますが、**やってはいけません。**
> 第10章で `list[int]` のような複合型が入ると、
> 同じ意味の型が別のオブジェクトとして作られます。

---

## 目次

- [10.1 複合型と type_equal の再帰](#101-複合型と-type_equal-の再帰)
- [10.2 型注釈が木になる](#102-型注釈が木になる)
- [10.3 メモリ上の表現](#103-メモリ上の表現)
- [10.4 ランタイム：MyList](#104-ランタイムmylist)
- [10.5 構文：添字とメソッド](#105-構文添字とメソッド)
- [10.6 空リスト `[]` の要素型](#106-空リスト--の要素型)
- [10.7 意味解析：添字・append・len](#107-意味解析添字appendlen)
- [10.8 コード生成](#108-コード生成)
- [10.9 動作確認](#109-動作確認)
- [10.10 まとめと次章の予告](#1010-まとめと次章の予告)

---

## 10.1 複合型と type_equal の再帰

### ✍️ `Type` に要素型を足す

```c
typedef enum {
    TY_INT, TY_BOOL, TY_NONE, TY_STR,
    TY_LIST,  // list[T] → ptr（第10章）
} TypeKind;

struct Type {
    TypeKind kind;
    Type *elem;   // list[T] の要素型（★ 第10章で初めて使う）
};
```

**⚠️ `list` 型はシングルトンにできません。**

```c
// int は 1 個だけ作れば足りる
void types_init(void) { ty_int = new_type(TY_INT); ... }

// list[T] は T ごとに違うので、書かれた場所で作る
Type *type_list(Type *elem) {
    Type *t = new_type(TY_LIST);
    t->elem = elem;
    return t;
}
```

### ★ 予告どおり `a == b` が壊れる

```python
xs: list[int] = []
ys: list[str] = xs      # ← これは型エラーになるべき
```

第9章までの `type_equal()` はこうでした。

```c
bool type_equal(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    // 第10章：list[T] なら要素型を再帰比較する   ← ここが空だった
    return true;
}
```

`list[int]` と `list[str]` は**どちらも `kind == TY_LIST`** なので、
**この関数は `true` を返してしまいます。**

**実際にそうなるか、10.9 節で確かめます**（要素型の比較を入れない状態でテストを走らせます）。

### ✍️ 再帰比較を入れる

```c
bool type_equal(Type *a, Type *b) {
    if (a == b) return true;            // シングルトンならここで済む
    if (a->kind != b->kind) return false;

    // ★ 第10章：複合型は中身まで見る
    if (a->kind == TY_LIST) return type_equal(a->elem, b->elem);

    return true;
}
```

**★ 5 章前に「ここに再帰比較が要る」とコメントで書いておいたので、
埋めるべき穴の場所が最初から分かっていました。**

### ✍️ 型の名前も再帰する

エラーメッセージに `list[list[int]]` と出す必要があります。

```c
const char *type_name(Type *t) {
    switch (t->kind) {
        case TY_INT: return "int";
        ...
        case TY_LIST: {
            // ⚠️ 動的に組み立てるので、返り値は毎回新しい文字列になる
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "list[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
    }
}
```

**⚠️ ここで `type_name()` の性質が変わります。**
第5章から「定数文字列を返す関数」でしたが、`list` では**確保した文字列**を返します。
解放しない方針（メモリモデル 3 節）なので問題は起きませんが、
**「返り値を後で解放してはいけない」ことは変わらない**と覚えておきます。

---

## 10.2 型注釈が木になる

### 📖 `list[int]` は 1 個の名前ではない

第9章までの型注釈は**識別子 1 個**でした。

```c
// これで足りていた
char *type_name;   // "int" という文字列
```

`list[int]` は入れ子になるので、**構造が必要**です。

```
list[list[int]]
 ├─ list
     └─ list
         └─ int
```

### ✍️ 型注釈も AST にする

```c
    ND_TYPEREF,  // 型注釈 → name, lhs（要素型の型注釈。無ければ NULL）
```

```c
// type_ref ::= IDENT [ "[" type_ref "]" ]
static Node *type_ref(Parser *p) {
    Token *t = type_name_token(p, "型名が必要です");

    Node *n = new_node(ND_TYPEREF, t);
    n->name = t->text;

    Token *open = peek(p);
    if (consume(p, "[")) {
        n->lhs = type_ref(p);           // ★ 再帰
        expect_close(p, "]", open);
    }
    return n;
}
```

**★ 再帰下降パーサなので、入れ子は再帰 1 行で書けます。**

### ✍️ 解決も再帰

```c
// 型注釈（構文）を Type（意味）に変換する。
//
// ★ 「名前から型への解決は sema の仕事」（第5章の判断 #47）が、
//   複合型になっても同じ形で通用します。
static Type *resolve_type(Node *tr) {
    if (strcmp(tr->name, "list") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "要素型を書いてください（例: list[int]）",
                          "list には要素型が必要です");
        return type_list(resolve_type(tr->lhs));   // ★ 再帰
    }

    if (tr->lhs)
        error_at_hint(tr->tok, "要素型を取るのは list だけです",
                      "型 '%s' は要素型を取りません", tr->name);

    Type *t = type_from_name(tr->name);
    if (!t) { /* 「未知の型名」 */ }
    return t;
}
```

**⚠️ `dict[K,V]` を足すときは、ここに「引数が 2 個」の分岐が増えます。**
今は `list` だけなので、`lhs` 1 本で足ります。

---

## 10.3 メモリ上の表現

```
xs: list[int]
   │
   ▼  ptr
┌──────────────────────┐
│ MyList               │  ← ヒープ（my_alloc）
│   data  ─────────┐   │
│   len   = 2      │   │
│   cap   = 4      │   │
└──────────────────┼───┘
                   ▼
              ┌────┬────┬────┬────┐
              │ 10 │ 20 │    │    │  ← 要素の配列（8 バイト × cap）
              └────┴────┴────┴────┘
```

| Mython | LLVM（値） | LLVM（メモリ） |
|---|---|---|
| `list[T]` | `ptr` | `ptr` |

**★ 要素はすべて 8 バイトに統一します。**

| 要素の型 | 保存のしかた |
|---|---|
| `int` | `i64` そのまま |
| `bool` | `i64` に広げて 0 / 1 |
| `str` | ポインタ（8 バイト） |
| `list[T]` | ポインタ（8 バイト） |

**🤔 なぜ 8 バイトに統一するのか**

要素サイズが型ごとに違うと、`MyList` に `elem_size` を持たせ、
`getelementptr` のオフセット計算を型ごとに変える必要があります。

**全部 8 バイトなら、ランタイムは `long long` の配列と `void *` の配列の
2 種類だけ扱えば済みます。** `bool` のために 7 バイト無駄になりますが、
**実装の単純さと引き換えにするなら安い代償**です（第5章から一貫した判断基準）。

**⚠️ `list[T]` は参照型です**（メモリモデル 1 節）。

```python
xs: list[int] = []
ys: list[int] = xs      # ★ 中身はコピーされない
ys.append(1)
print(len(xs))          # → 1（xs も変わる）
```

**`str` と違い、`list` は可変（mutable）なので、共有が観測できます。**
これは仕様どおりの挙動です（言語仕様 3.1 の参照セマンティクス）。

---

## 10.4 ランタイム：MyList

メモリモデル 4 節で決めた API を実装します。

```c
typedef struct {
    void *data;      // 要素の配列（8 バイト × cap）
    long long len;
    long long cap;
} MyList;

MyList *my_list_new(void) {
    MyList *l = my_alloc((long long)sizeof(MyList));
    l->cap = 4;
    l->data = my_alloc(l->cap * 8);
    l->len = 0;
    return l;
}
```

### ✍️ 伸長は倍々にする

```c
static void grow(MyList *l) {
    if (l->len < l->cap) return;
    long long ncap = l->cap * 2;
    void *nd = my_alloc(ncap * 8);
    memcpy(nd, l->data, (size_t)l->len * 8);
    l->data = nd;      // ⚠️ 古い領域は解放しない（メモリモデル 3 節）
    l->cap = ncap;
}
```

**⚠️ `realloc` を使わないのは、解放しない方針と噛み合わないからです。**
`realloc` は古い領域を無効化しますが、
**私たちは「一度渡したポインタは永久に有効」という前提で動いています。**
`memcpy` して捨てるほうが、方針と一貫します。

**★ 倍々に増やす**ので、n 回の `append` の総コストは O(n) です
（毎回 +1 だと O(n²) になります）。

### ✍️ 範囲検査（規約 R10）

```c
static void check_index(MyList *l, long long i) {
    if (i < 0 || i >= l->len) {
        // 言語仕様 8 節が決めた形式：index out of range: i (len=n)
        char buf[128];
        snprintf(buf, sizeof(buf), "index out of range: %lld (len=%lld)", i, l->len);
        my_panic(buf);
    }
}
```

**★ 検査をランタイム側に置くので、生成する IR に分岐が 1 つも出ません**（R10）。
第9章で 0 除算を `my_floordiv` に押し込んだのと同じ形です。

**⚠️ 負の添字は「範囲外」です。** Python の `xs[-1]`（末尾）は
**採用しません**（言語仕様との差異表）。便利ですが、
「範囲外アクセスのバグ」が「意図しない末尾アクセス」に化けて発見が遅れます。

---

## 10.5 構文：添字とメソッド

### ✍️ postfix のループに 2 つ足すだけ

文法（grammar.md）はこうなっています。

```ebnf
postfix ::= primary { call_suffix | index_suffix | attr_suffix }
```

**第8章で `postfix()` をループで書いておいたので、分岐を 2 本足すだけです。**

```c
static Node *postfix(Parser *p) {
    Node *n = primary(p);

    for (;;) {
        Token *t = peek(p);

        if (tok_is(t, "(")) { /* 呼び出し（第8章）*/ continue; }

        // 添字 xs[i]
        if (consume(p, "[")) {
            Node *idx = new_node(ND_INDEX, t);
            idx->lhs = n;
            idx->rhs = expr(p);
            expect_close(p, "]", t);
            n = idx;
            continue;
        }

        // メソッド xs.append(v)
        if (consume(p, ".")) {
            /* IDENT を読み、"(" 引数 ")" を読む → ND_METHOD */
            continue;
        }
        return n;
    }
}
```

**★ 第8章で「第10章で `xs[0]` や `p.f` を足すときも、
このループに追加するだけです」と書いた予告どおりになりました。**

### ✍️ リストリテラル

```ebnf
list_display ::= "[" [ expr { "," expr } [ "," ] ] "]"
```

```c
    // primary の中
    if (tok_is(t, "[")) {
        advance(p);
        Node *n = new_node(ND_LIST, t);
        /* 要素を , 区切りで読む（末尾の , を許す）*/
        expect_close(p, "]", t);
        return n;
    }
```

**⚠️ 末尾のカンマを許す**のは、行を並べて書くときに便利だからです。

```python
xs: list[int] = [
    1,
    2,      # ← ここでカンマを消さなくていい
]
```

### ⚠️ `[` の意味が 2 つある

| 位置 | 意味 |
|---|---|
| 式の先頭（`primary`） | リストリテラル `[1, 2]` |
| 式の直後（`postfix`） | 添字 `xs[0]` |

**構文解析器はこれを自然に区別できます。**
`primary()` は「式の始まり」でしか呼ばれず、
`postfix()` のループは「式を 1 つ読み終えた後」しか回らないからです。

**★ 階層化された文法の副産物です。** 特別な処理は要りません。

---

## 10.6 空リスト `[]` の要素型

### ⚠️ これが型検査の難所です

```python
xs: list[int] = []
```

**`[]` だけを見ても、要素型が決まりません。**

Mython は型推論をしない（第5章の判断）ので、**注釈から受け取ります**。

```c
// Sema に「今、どの型が期待されているか」を持たせる。
typedef struct {
    ...
    Type *expected;   // ★ 第10章：空リストの要素型を決めるため
} Sema;
```

```c
static void check_vardecl(Sema *s, Node *n) {
    Type *declared = resolve_type(n->type_ref);
    ...
    s->expected = declared;         // ★ 初期化式に期待型を渡す
    Type *actual = check_expr(s, n->rhs);
    s->expected = NULL;
```

```c
static Type *check_list_lit(Sema *s, Node *n) {
    if (!n->body) {                       // 空リスト
        if (!s->expected || s->expected->kind != TY_LIST) {
            Diag d = {0};
            d.message = "空のリストの要素型が決まりません";
            d.hint = "型注釈を書いてください（例: xs: list[int] = []）";
            diag_fail(&d);
        }
        return s->expected;
    }
    // 要素があるなら、最初の要素の型を要素型にする
    Type *et = check_expr(s, n->body);
    /* 2 個目以降が同じ型か確かめる */
    return type_list(et);
}
```

**🤔 なぜ「期待型」を持ち回すのか**

本格的なやり方は**双方向型検査（bidirectional type checking）**で、
`check_expr(node, expected)` のように期待型を引数で渡します。

v1 では**空リストだけ**が期待型を必要とするので、
`Sema` に 1 つフィールドを持たせる形で済ませます。

**⚠️ この手は「期待型が要る式」が増えたら破綻します。**
そのときは引数で渡す形に直します。
**今は「動く最小」を選び、限界を記録しておきます**（第5章から一貫した姿勢）。

**期待型を渡すのは 3 か所**です。

| 場所 | 期待型 |
|---|---|
| 変数宣言の初期化式 | 宣言された型 |
| 代入の右辺 | 左辺の型 |
| `return` の式 | 関数の戻り型 |

関数の引数（`f([])`）では渡していません。**そこは今はエラーになります**
（ヒントで「変数に入れてから渡してください」と案内します）。

---

## 10.7 意味解析：添字・append・len

### ✍️ 添字

```c
static Type *check_index(Sema *s, Node *n) {
    Type *ot = check_expr(s, n->lhs);
    Type *it = check_expr(s, n->rhs);

    if (it->kind != TY_INT) { /* 「添字は int でなければなりません」 */ }

    if (ot->kind == TY_LIST) return ot->elem;   // ★ 要素型を返す
    if (ot->kind == TY_STR) return ty_str;      // 1 文字の str（型システム 5.8）

    /* 「型 'int' は添字を取れません」 */
}
```

**`str` の添字が 1 文字の `str` を返す**のは、
**`char` 型を作らない**という言語仕様の決定です。型が 1 つ減ります。

### ✍️ append

```c
static Type *check_method(Sema *s, Node *n) {
    Type *ot = check_expr(s, n->lhs);

    if (ot->kind == TY_LIST && strcmp(n->name, "append") == 0) {
        /* 引数はちょうど 1 個、型は要素型と一致 */
        Type *at = check_expr(s, n->args);
        if (!type_equal(at, ot->elem)) { /* 型不一致 */ }
        return ty_none;
    }

    /* 「型 'T' にメソッド 'x' はありません」＋ 使えるメソッドの一覧 */
}
```

**⚠️ ここでも `type_equal()` を使います。**
`list[list[int]]` に `list[str]` を append するのを弾くには、再帰比較が要ります。

### ✍️ len は表に 1 行足すだけ

```c
const Builtin BUILTINS[] = {
    ...
    {"len", TY_STR, TY_INT, "my_str_len"},
    {"len", TY_LIST, TY_INT, "my_list_len"},   // ← 追加
```

**★ 第9章で組み込みを「表」にしておいたので、1 行です。**

**⚠️ 表は `TypeKind` でマッチするので、`list[int]` でも `list[str]` でも
同じエントリに当たります。** `len` は要素型を見ないので、これで正しく動きます。
（要素型ごとに区別が必要な組み込みが出てきたら、表の形を変える必要があります。）

### ✍️ 添字への代入

```python
xs[0] = 99
```

**第5章の「式を先に読んで、後から役割を決める」がそのまま効きます。**

```c
    // 代入できる形か（ND_VAR に加えて ND_INDEX を許す）
    if (lhs->kind != ND_VAR && lhs->kind != ND_INDEX) {
        /* 「この式には代入できません」 */
    }
```

**★ 第5章にこう書いてありました。**

> こうすると `xs[0] = 1` や `p.f = 1`（第10章・第12章）にも
> そのまま対応できます。

**5 章前の設計が、条件を 1 つ足すだけで回収されました。**

---

## 10.8 コード生成

### ✍️ 要素の型で呼ぶ関数を選ぶ

```c
// 要素を i64 で持つか、ポインタで持つか
static bool elem_is_ptr(Type *elem) {
    return elem->kind == TY_STR || elem->kind == TY_LIST;
}
```

| 操作 | `int` / `bool` | `str` / `list` |
|---|---|---|
| append | `my_list_push_i64` | `my_list_push_ptr` |
| 読み出し | `my_list_get_i64` | `my_list_get_ptr` |
| 書き込み | `my_list_set_i64` | `my_list_set_ptr` |

**⚠️ `bool` は境界で `zext` / `trunc` します**（規約 R5）。
第9章で C との境界に入れたのと同じ処理です。

### ✍️ リテラル

```llvm
  %t0 = call ptr @my_list_new()
  call void @my_list_push_i64(ptr %t0, i64 1)
  call void @my_list_push_i64(ptr %t0, i64 2)
```

**★ `[1, 2]` は「空リストを作って 2 回 append する」に脱糖されます。**
第5章の複合代入、第7章の `elif` と同じ「脱糖」の手です。

**⚠️ 定数リストを `.rodata` に置く最適化はしません。**
やるなら要素が全部定数のときだけですが、
**分岐が増えるだけで得るものが少ない**ので v1ではやりません。

---

## 10.9 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```
全 201 件パス
```

25 件追加しました（正常系 12・エラー 11・実行時エラー 2）。
ビルド警告 0 件、ASan/UBSan も全ケースでクリーンです。

### ✅ 目標

```bash
$ ./build/mythonc t.my -o t && ./t
30
2
```

### ★ 第5章の予告が現実になった（この章の見どころ）

```python
xs: list[int] = [1, 2]
ys: list[str] = xs      # ← 型エラーになるべき
print(ys[0])
```

**再帰比較を入れた正しい実装：**

```
error: 型が一致しません
  --> t.my:3:21
   |
 3 |     ys: list[str] = xs
   |                     ^^ 型 'list[int]' の式
```

**`type_equal()` から要素型の再帰比較を消すと（＝第5章が警告していた状態）：**

```bash
$ ./build/mythonc t.my -o t        # ← コンパイルが通ってしまう
$ ./t
$ echo $?
139
```

**139 は SIGSEGV（128 + 11）です。**
`int` の `1` を `str` のポインタとして読もうとして落ちました。

**★ これが「今は動くが将来壊れる書き方」の典型です。**
第5章の時点では `int` しか型が無く、`a == b` でも動いていました。
**5 章前に「やってはいけない」と書いておいたおかげで、
埋めるべき穴の場所が最初から分かっていました。**

もし `a == b` のまま第10章に来ていたら、
**コンパイルは通るのに実行時に segfault する**という、
最悪の形のバグを追うことになっていたはずです。

### ✅ リストリテラルの IR

```bash
$ ./build/mythonc -S tests/cases/list_literal.my
```

```llvm
  %xs = alloca ptr
  %t0 = call ptr @my_list_new()
  call void @my_list_push_i64(ptr %t0, i64 1)
  call void @my_list_push_i64(ptr %t0, i64 2)
  call void @my_list_push_i64(ptr %t0, i64 3)
  store ptr %t0, ptr %xs
```

**`[1, 2, 3]` が「空リスト + append 3 回」に脱糖されています。**
コード生成器に「定数リスト」の特別扱いはありません。

### ✅ 入れ子のリスト

```python
grid: list[list[int]] = []
row: list[int] = [3, 4]
grid.append(row)
print(grid[0][1])      # → 4
print(len(grid[0]))    # → 2
```

**`grid[0][1]` が動きます。** `postfix()` がループなので、
添字を 2 回続けて書くだけで自然に読めます。

### ✅ 参照セマンティクス

```python
xs: list[int] = []
ys: list[int] = xs
ys.append(1)
print(len(xs))    # → 1（xs も変わる）
```

**`str` との違いが観測できます。** `str` は不変なので共有が見えませんが、
`list` は可変なので見えます（言語仕様 3.1）。

### ✅ bool のリスト

```python
bs: list[bool] = [True, False]
print(bs[0])      # True
print(bs[1])      # False
```

**要素は `i64` に広げて保存し、読み出しで `i1` に戻しています**（規約 R5）。
第6章で作った境界処理と同じ形です。

### ✅ 伸長

```python
xs: list[int] = []
i: int = 0
while i < 100:
    xs.append(i)
    i += 1
print(len(xs))            # 100
```

初期 `cap` は 4 なので、5 回目・9 回目・17 回目…で伸びています。
**合計 4950（0..99 の和）も正しく取り出せました。**

### ✅ str の添字

```python
s: str = "Mython"
print(s[0])    # M
print(s[5])    # n
```

**1 文字の `str` を返します。** `char` 型を作らないという言語仕様の決定により、
型が 1 つ減っています。

### ✅ 範囲外アクセス（規約 R10）

```bash
$ ./t
runtime error: index out of range: 5 (len=2)
$ echo $?
1
```

**言語仕様 8 節が決めた形式（`index out of range: i (len=n)`）どおりです。**

負の添字も範囲外です。

```
runtime error: index out of range: -1 (len=2)
```

**⚠️ Python の `xs[-1]`（末尾）は採用していません。**
便利ですが、「範囲外アクセスのバグ」が
「意図しない末尾アクセス」に化けて発見が遅れます。

### ✅ エラー：要素型の不一致

```
error: 'int' のリストに 'str' を追加できません
   |
   = ヒント: Mython には暗黙の型変換がありません（言語仕様 3.5）
```

```
error: リストの要素の型がそろっていません（第 2 要素）
   |
 4 |     xs: list[int] = [1, "two"]
   |                         ^^^^^ これは 'str' 型です
   |
note: 最初の要素は 'int' 型です
```

### ✅ エラー：空リストの要素型が決まらない

```
error: 空のリストの要素型が決まりません
   |
 6 |     return take([])
   |                 ^ この [] がどんなリストなのか分かりません
   |
   = ヒント: 型注釈を書いてください（例: xs: list[int] = []）。関数の引数に直接渡す場合は、いったん変数に入れてください
```

**期待型を渡していない場所（関数の引数）では、こう案内します。**
**「できないこと」と「回避のしかた」を同時に書く**のが第3章からの約束です。

### ✅ エラー：型注釈の形

```
error: list には要素型が必要です
   |
   = ヒント: 要素型を書いてください（例: list[int]）
```

```
error: 型 'int' は要素型を取りません
   |
   = ヒント: 要素型を取るのは list だけです
```

### ✅ エラー：str の要素への代入

```
error: 文字列の要素には代入できません
   |
   = ヒント: str は不変（immutable）です。新しい文字列を作ってください
```

### ⚠️ 途中で踏んだ落とし穴

**複数行のリストリテラルが書けませんでした。**

```python
xs: list[int] = [
    1,
    2,
]
```

```
error: 式が必要です
   |
 3 |     xs: list[int] = [
   |                      ^ ここには式が来るはずです
```

**原因は第4章の `OPEN_BRACKETS` です。**

```c
// 第4章：括弧の中では改行を無視する
static const char *OPEN_BRACKETS = "(";   // ← '[' が無い
```

第4章の時点では `[` が言語に存在しなかったので `(` だけでした。
**`[` を足したのに、こちらの更新を忘れていました。**

```c
static const char *OPEN_BRACKETS = "([";
static const char *CLOSE_BRACKETS = ")]";
```

**★ 記号を足したときは「字句解析器の他の場所」も確認する。**
第12章で `{`（辞書）を足すなら、また同じ場所を触ることになります。
コメントに「ここを更新し忘れると複数行のリストが書けなくなる」と書き残しました。

---

## 10.10 まとめと次章の予告

### できたこと

```
✅ TY_LIST（複合型）と type_equal() の再帰比較 — 第5章からの宿題
✅ 型注釈が木になった（ND_TYPEREF、list[list[int]] が書ける）
✅ リストリテラル [1, 2, 3]（末尾のカンマも可）
✅ 添字 xs[i] の読み書き、str の添字（1 文字の str）
✅ メソッド呼び出し xs.append(v)
✅ 空リスト [] の要素型を「期待型」から決める
✅ len() の list 対応（表に 1 行）
✅ MyList（倍々に伸びる動的配列）と範囲検査（規約 R10）
✅ 参照セマンティクスの確認（str との違い）
✅ テスト 201 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `runtime/runtime.c` | `MyList`、`my_list_*` 8 関数、`my_str_index` |
| `src/lexer.c` | 記号 `[` `]` `.`、**`OPEN_BRACKETS` に `[` を追加** |
| `src/types.h/c` | `TY_LIST`、`Type.elem`、`type_list()`、`type_equal` の再帰、`type_name` の再帰 |
| `src/ast.h/c` | `ND_TYPEREF` / `ND_LIST` / `ND_INDEX` / `ND_METHOD`、`type_ref`、ダンプ |
| `src/parser.c` | `type_ref()`、リストリテラル、`postfix` に添字とメソッド、代入先に `ND_INDEX` |
| `src/sema.c` | `resolve_type()`、`check_list_lit` / `check_index_expr` / `check_method`、`expected`、添字への代入 |
| `src/codegen.c` | `elem_is_ptr` / `slot_ty` / `elem_to_slot` / `slot_to_elem`、`gen_list_lit` / `gen_index` / `gen_index_store` / `gen_method` |
| `tests/cases/` | 25 件追加、2 件の期待値を更新 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch5 | **「`a == b` は第10章で壊れる」と警告** | **予告どおり壊れた**（segfault で実証） |
| ch5 | `Type.elem` をコメントで用意 | 埋めるだけで済んだ |
| ch5 | 「式を先に読んで役割を決める」 | `xs[0] = v` が条件 1 つ追加で通った |
| ch5 | 型名の解決は sema の仕事 | 複合型でも同じ形（`resolve_type`）で通用 |
| ch6 | `zext` / `trunc` を境界に閉じ込め | list の要素（bool）でも同じ形 |
| ch8 | `postfix()` をループで書いた | 添字とメソッドを分岐 2 本足すだけ |
| ch9 | 組み込みを「表」にした | `len(list)` が **1 行** |
| ch9 | `declare_rt()`（宣言の重複排除） | ランタイム関数が 9 個増えても無変更 |
| 設計 | R10（複雑さはランタイムへ） | 範囲検査の分岐が IR に 1 つも出ない |
| 設計 | メモリを解放しない | `realloc` を避けて `memcpy` で伸ばせた |

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 期待型を `Sema` の状態で持ち回している（引数に渡す形が本来） | 必要になったら |
| 関数の引数に `[]` を直接渡せない | 同上 |
| `list` のメソッドが `append` だけ（`pop` / `insert` など） | 第14章（標準ライブラリ） |
| スライス `xs[1:3]` | v1 未対応 |
| `dict[K,V]` | 第14章 |
| 要素型ごとに区別が必要な組み込みが出たら、表の形を変える必要がある | 必要になったら |

### ✍️ commit する

```bash
git add -A
git commit -m "第10章: list[T]（動的配列）"
```

---

## 次章：第11章 for 文と range

**達成目標**

```python
def main() -> int:
    xs: list[int] = [10, 20, 30]
    for x in xs:
        print(x)
    for i in range(3):
        print(i)
    return 0
```

**やること**

| ファイル | 作業 |
|---|---|
| `parser.c` | `for_stmt ::= "for" IDENT "in" expr ":" block` |
| `sema.c` | ループ変数を**ブロックスコープに自動宣言**（型は要素型から決まる） |
| **脱糖** | `for` を `while` に**書き換える**のが中心。コード生成器は無変更で済むはず |

**★ この章の主題は「構文糖」です。**
`for` は新しい機能ではなく、`while` の書き方を変えたものにすぎません。

```python
for x in xs:          i: int = 0
    BODY          →   while i < len(xs):
                          x: T = xs[i]
                          BODY
                          i += 1
```

**⚠️ 予想される落とし穴**

- `continue` の飛び先が**増分処理**になる（第7章では条件だった）。
  素朴に脱糖すると `continue` が `i += 1` を飛ばして**無限ループ**になる
- ループ変数の名前が既存の変数と衝突したら？（シャドーイング禁止との関係）
- `range(a, b, c)` の 3 引数形式（言語仕様 5.5）
- 脱糖した AST は「ソースに無いノード」なので、**エラー位置のトークンをどう付けるか**

### 🤔 第11章に入る前の練習問題

1. **`type_equal()` の `TY_LIST` の行を消して** `make test` を走らせ、
   どのテストが落ちるか確認する（**必ず元に戻す**）。
   落ちないテストがあれば、それは「型を混ぜても気づけない」テストです
2. `my_list_grow()` の `l->cap * 2` を `l->cap + 1` に変えて、
   `list_grow.my` の実行時間を比べる（O(n) と O(n²) の差）
3. **`OPEN_BRACKETS` から `[` を消して**、複数行のリストが
   どんなエラーになるか見る
4. `list[list[list[int]]]` を書いて `--dump-ast` で型注釈の木を見る
5. `xs.append(xs)`（自分自身を追加）を書いてみる。
   **型検査に通らない**はずですが、**なぜ通らないのか**を型で説明してみてください
   （`list[T]` に追加できるのは `T` であって `list[T]` ではない）。
   ★ 副産物として、**循環するリストが型レベルで作れない**ことになります
