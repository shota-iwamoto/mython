# 第7章 制御構文（if / elif / else / while）

> **この章のゴール**
> ブロック構造を持つプログラムが書けるようになり、**出力が目で見えるようになる**。
>
> ```bash
> $ cat t.my
> i: int = 1
> while i <= 5:
>     if i % 2 == 0:
>         print(i)
>     i += 1
> 0
> $ ./build/mythonc t.my -o t && ./t
> 2
> 4
> ```

**この章は 3 つの節目です。**

1. **第4章で作った `INDENT` / `DEDENT` が、3 章越しに消費される** — 「生成側だけ作る」判断の完結
2. **`print` が入り、出力が見えるようになる** — 終了コードだけの世界から抜け出す
3. **第5章の前提が 1 つ壊れる** — 「変数名がそのまま一意な IR 名になる」が成り立たなくなる

第6章で作った `emit_label` / `emit_br` / `emit_cond_br` が、そのまま土台になります。
**短絡評価が書けたなら、if と while はもう書けます。**

---

## 目次

- [7.1 3 章越しの回収：INDENT を消費する](#71-3-章越しの回収indent-を消費する)
- [7.2 block を読む](#72-block-を読む)
- [7.3 if / elif / else](#73-if--elif--else)
- [7.4 while と break / continue](#74-while-と-break--continue)
- [7.5 print の暫定実装](#75-print-の暫定実装)
- [7.6 意味解析：条件は bool でなければならない](#76-意味解析条件は-bool-でなければならない)
- [7.7 ブロックスコープとシャドーイング禁止](#77-ブロックスコープとシャドーイング禁止)
- [7.8 第5章の前提が壊れる：IR 名の一意性](#78-第5章の前提が壊れるir-名の一意性)
- [7.9 コード生成：if](#79-コード生成if)
- [7.10 コード生成：while と break / continue](#710-コード生成while-と-break--continue)
- [7.11 到達不能コード（規約 R7）](#711-到達不能コード規約-r7)
- [7.12 動作確認](#712-動作確認)
- [7.13 まとめと次章の予告](#713-まとめと次章の予告)

---

## 7.1 3 章越しの回収：INDENT を消費する

### 📖 第4章で先送りしたもの

第4章では、字句解析器に `NEWLINE` / `INDENT` / `DEDENT` を**合成させる**ところまで作りました。
そして、こう書いて先送りしました。

> **注記**: `INDENT`/`DEDENT` を消費する構文（if/while）は第7章。
> この章は**生成側**を作り、`--dump-tokens` で検証する

**その第7章が来ました。**

```
第4章：字句解析器が INDENT / DEDENT を「作る」   ← 消費者がいないまま検証した
第7章：構文解析器が INDENT / DEDENT を「使う」   ← ★ 今回
```

**🤔 なぜ 3 章も空けたのか**

消費者（if / while）を作るには条件式が要り、条件式には `bool` が要り、
`bool` には型検査が要ります。**依存の順に積み上げると、この順序になります。**

第4章の時点で `--dump-tokens` という検証手段を用意しておいたおかげで、
「使う側が無いのでテストできない」という状態を避けられました。
**第1章でデバッグ道具に投資した効果が、ここまで効き続けています。**

### 📖 この章で増える文法

```ebnf
stmt       ::= simple_stmt NEWLINE
             | if_stmt                                  ← 新規
             | while_stmt                               ← 新規

block      ::= NEWLINE INDENT stmt { stmt } DEDENT      ← 新規（★ 本題）

if_stmt    ::= "if" expr ":" block
               { "elif" expr ":" block }
               [ "else" ":" block ]                     ← 新規

while_stmt ::= "while" expr ":" block                   ← 新規

simple_stmt::= var_decl | assign_stmt | expr_stmt
             | "break" | "continue" | "pass"            ← 新規
             | print_stmt                               ← 新規（暫定）
```

**`block` の定義がこの章の中心です。**

```ebnf
block ::= NEWLINE INDENT stmt { stmt } DEDENT
```

これは**波括弧言語の `{ stmt }` とまったく同じ形**です。

```ebnf
(* C 言語なら *)
block ::= "{" stmt { stmt } "}"
```

**★ 第4章で仮想トークンを合成しておいたので、
インデント構文の難しさが「波括弧を読む」のと同じ難しさに落ちました。**
オフサイドルールの複雑さは、すべて字句解析器の中に閉じ込められています。

---

## 7.2 block を読む

### ✍️ 第4章で予告した `expect()` を実装する

第4章の `parser.c` には、こんなコメントが残してありました。

```c
// 【第4章で追加する予定】トークン種別を指定して要求する版：
//
//     static Token *expect(Parser *p, TokenKind kind, const char *what) { ... }
//
// NEWLINE / INDENT / DEDENT を要求するときに必要になります。
// 今は記号版だけで足りるので、未使用警告を避けてコメントにしてあります。
```

**その「必要になるとき」が来ました。** コメントを実装に変えます。

```c
// トークン種別を指定して要求する。
static Token *expect(Parser *p, TokenKind kind, const char *what,
                     const char *hint) {
    Token *t = peek(p);
    if (t->kind == kind) return advance(p);

    Diag d = {0};
    d.message = diag_fmt("%s が必要です", what);
    d.primary.tok = t;
    d.primary.label = diag_fmt("ここは %s です", token_kind_name(t->kind));
    d.hint = hint;
    diag_fail(&d);
}
```

**⚠️ 仮想トークンの名前をそのままユーザーに見せない**よう注意します。
「INDENT が必要です」と言われても、利用者には意味が分かりません。
`what` と `hint` には**人間の言葉**を渡します。

### ✍️ `block()`

```c
// block ::= NEWLINE INDENT stmt { stmt } DEDENT
//
// ★ 第4章で作った仮想トークンを、ここで初めて消費します。
static Node *block(Parser *p) {
    Token *head_tok = peek(p);

    expect(p, TK_NEWLINE, "改行", "':' の後は改行してブロックを字下げしてください");
    expect(p, TK_INDENT, "字下げされたブロック",
           "':' の次の行は字下げしてください（スペース 4 個を推奨）");

    Node head = {0};
    Node *cur = &head;
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        cur->next = stmt(p);
        cur = cur->next;
    }
    expect(p, TK_DEDENT, "ブロックの終わり", NULL);

    Node *blk = new_node(ND_BLOCK, head_tok);
    blk->body = head.next;
    return blk;
}
```

**ループの終了条件に `TK_EOF` を入れておく**のを忘れないでください。
第4章の字句解析器はファイル末尾で開いている `DEDENT` を必ず全部出すので
理屈の上では `TK_EOF` に到達しませんが、
**入れておかないと、万一のとき無限ループになります**。

### 📖 空のブロックは字句解析器が防いでくれる

```python
if x > 0:
x = 1        # 字下げしていない
```

このとき `INDENT` は出ません（中身が無いので）。
だから `expect(p, TK_INDENT, ...)` が発火し、
「字下げされたブロックが必要です」という診断になります。

**構文解析器が「空ブロック」を特別扱いする必要はありません。**
第4章の設計が効いています。

---

## 7.3 if / elif / else

### ✍️ AST ノード

```c
// ast.h
typedef enum {
    ...
    ND_IF,        // if 文     → lhs（条件）, body（then）, els（else）
    ND_WHILE,     // while 文  → lhs（条件）, body
    ND_BREAK,     // break
    ND_CONTINUE,  // continue
    ND_PASS,      // pass（何もしない）
    ND_PRINT,     // print(e)（暫定。第8章で本物の関数呼び出しになる）→ lhs
} NodeKind;
```

`else` 節を入れるフィールドを 1 つ足します。

```c
struct Node {
    ...
    Node *lhs, *rhs;
    Node *body;   // ND_BLOCK の中身 / ND_IF の then / ND_WHILE の本体
    Node *els;    // ND_IF の else（★ 新規）
    Node *next;
};
```

### ✍️ `elif` は脱糖する

```c
// if_stmt ::= "if" expr ":" block { "elif" expr ":" block } [ "else" ":" block ]
//
// ★ elif は「else の中に if が 1 個ある」形に脱糖します。
//
//     if a:  A          if a:  A
//     elif b: B    →    else:
//     else:  C              if b: B
//                           else: C
//
//   こうすると ND_ELIF のようなノードが不要になり、
//   意味解析もコード生成も「if は 2 分岐」だけを扱えば済みます。
//   第5章の複合代入（x += e → x = x + e）と同じ発想です。
static Node *if_stmt(Parser *p) {
    Token *t = advance(p);  // "if" または "elif"

    Node *n = new_node(ND_IF, t);
    n->lhs = expr(p);
    expect_colon(p);
    n->body = block(p);

    if (tok_is_kw(peek(p), "elif")) {
        n->els = if_stmt(p);  // ★ 再帰：else の中の if として扱う
    } else if (consume_kw(p, "else")) {
        expect_colon(p);
        n->els = block(p);
    }
    return n;
}
```

**★ 再帰 1 行で `elif` が何個でも繋がります。**
`elif` の個数を数えたり、ループを書いたりする必要はありません。

### ⚠️ `elif` を消費するのは誰か

上のコードで `if_stmt` は先頭のトークンを `advance()` で
**種類を確認せずに**消費しています。呼ばれる場所が 2 か所あるからです。

- `stmt()` から呼ばれるとき → 先頭は `if`
- 自分自身から呼ばれるとき → 先頭は `elif`

**どちらも「条件 `:` ブロック」という同じ構造**なので、同じ関数で読めます。

### ⚠️ 孤立した `elif` / `else`

```python
x: int = 1
else:
    print(1)
```

`if` が無いのに `else` が来ました。このとき `else` は `stmt()` に到達し、
`simple_stmt` → `expr` → `primary` に落ちて
**第5章で作った「予約語は変数名として使えません」** に捕まります。

これは**間違いではありませんが、親切ではありません**。専用の診断を出します。

```c
    // 対応する if が無い elif / else
    if (tok_is_kw(t, "elif") || tok_is_kw(t, "else")) {
        Diag d = {0};
        d.message = diag_fmt("対応する if がない '%s' です", t->text);
        d.primary.tok = t;
        d.primary.label = "この行に対応する 'if' が見つかりません";
        d.hint = "'elif' / 'else' は 'if' と同じ字下げの位置に書いてください";
        diag_fail(&d);
    }
```

**★ 「エラーになるからいい」ではなく「良いエラーになるか」**（第3章からの姿勢）。

---

## 7.4 while と break / continue

### ✍️ while

```c
// while_stmt ::= "while" expr ":" block
static Node *while_stmt(Parser *p) {
    Token *t = advance(p);  // "while"

    Node *n = new_node(ND_WHILE, t);
    n->lhs = expr(p);
    expect_colon(p);
    n->body = block(p);
    return n;
}
```

`if` とほぼ同じです。**違いはコード生成だけ**（条件へ戻る `br` があるかどうか）。

### ✍️ break / continue / pass

```c
    if (tok_is_kw(t, "break")) { advance(p); return new_node(ND_BREAK, t); }
    if (tok_is_kw(t, "continue")) { advance(p); return new_node(ND_CONTINUE, t); }
    if (tok_is_kw(t, "pass")) { advance(p); return new_node(ND_PASS, t); }
```

**`pass` は「何もしない」文です。** コード生成でも本当に何も出しません。

**🤔 なぜ `pass` が要るのか**
Mython の `block` は「1 つ以上の文」を要求します。
空のブロックを書きたいとき（後で埋めるつもりのとき）に置く場所が必要です。
Python と同じ理由です。

---

## 7.5 print の暫定実装

### 📖 終了コードだけの世界から抜け出す

第6章まで、プログラムの結果を見る手段は**終了コードだけ**でした。
これには 2 つの限界があります。

| 限界 | 例 |
|---|---|
| 1 個の値しか見えない | ループが何を計算したか分からない |
| 0〜255 しか表せない | `print(1000)` 相当ができない |

**ループを作る章で「途中経過が見えない」のは致命的**なので、`print` を入れます。

### ⚠️ これは暫定実装です

言語仕様では `print` は**組み込み関数**で、`int` / `str` / `bool` / `float` の
オーバーロードを持ちます。しかし：

- **関数呼び出しの構文**は第8章
- **`str` 型**は第9章

そこでこの章では **`print(式)` を「文」として特別扱い**します。

```c
// print_stmt ::= "print" "(" expr ")"     ← 暫定
//
// ⚠️ 第8章で本物の関数呼び出しが入ったら、この特別扱いは消えます。
//    それまでの足場です（第1章の「暫定の main」と同じ位置づけ）。
static Node *print_stmt(Parser *p) {
    Token *t = advance(p);  // "print"
    Token *open = advance(p);  // "("
    Node *n = new_node(ND_PRINT, t);
    n->lhs = expr(p);
    expect_close(p, ")", open);
    return n;
}
```

`print` は**予約語ではありません**（言語仕様の予約語表に入っていません）。
`IDENT` の "print" の次が `(` なら print 文、と判定します。

```c
    // print( ... ) は暫定の組み込み文
    if (peek(p)->kind == TK_IDENT && strcmp(peek(p)->text, "print") == 0 &&
        tok_is(peek_at(p, 1), "("))
        return print_stmt(p);
```

**★ また 2 トークン先読み**です（第5章の `x: int` と同じ道具）。

### ✍️ コード生成：C の printf を借りる

```llvm
@.fmt.int = private unnamed_addr constant [6 x i8] c"%lld\0A\00"

declare i32 @printf(ptr noundef, ...)

  %t0 = call i32 (ptr, ...) @printf(ptr @.fmt.int, i64 %v)
```

**★ 第1章で用意した `globals` / `decls` バッファが、ここで初めて使われます。**

```c
static void gen_print(Emitter *e, Node *n) {
    // 書式文字列と declare は、最初に print が現れたときに 1 回だけ出す
    if (!e->printf_declared) {
        sb_printf(&e->globals,
                  "@.fmt.int = private unnamed_addr constant [6 x i8] "
                  "c\"%%lld\\0A\\00\"\n");
        sb_printf(&e->decls, "declare i32 @printf(ptr noundef, ...)\n");
        e->printf_declared = true;
    }

    char *v = gen_expr(e, n->lhs);
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call i32 (ptr, ...) @printf(ptr @.fmt.int, i64 %s)\n",
              t, v);
}
```

**⚠️ `[6 x i8]` の 6 は `%lld\n\0` のバイト数**です（`%`,`l`,`l`,`d`,`\n`,`\0`）。
数え間違えると LLVM がエラーにします（あるいは文字列が切れます）。

**⚠️ 可変長引数の呼び出しには関数型を書く**必要があります。

```llvm
  call i32 (ptr, ...) @printf(...)     ; ✅
  call i32 @printf(...)                ; ✗ 可変長引数では不可
```

---

## 7.6 意味解析：条件は bool でなければならない

```c
static void check_cond(Sema *s, Node *stmt_node, Node *cond) {
    Type *t = check_expr(s, cond);
    if (t->kind != TY_BOOL) bool_required(stmt_node, cond, t);
}
```

**第6章で作った `bool_required()` がそのまま使えます。**
`and` / `or` / `not` のために作った関数が、`if` / `while` にも効きました。

```python
x: int = 1
if x:            # ← エラー
    print(1)
```

```
error: 演算子 'if' には bool が必要です
```

**⚠️ メッセージの「演算子」が不自然です。** `if` は演算子ではありません。
`bool_required` は `op_symbol(op_node->op)` でメッセージを組み立てていましたが、
`ND_IF` には `op` がありません。**文言を渡せるように直します。**

```c
// Before（第6章）：op_node から記号を取り出していた
static Type *bool_required(Node *op_node, Node *operand, Type *actual);

// After（第7章）：「どこで必要なのか」を文字列で受け取る
static Type *bool_required(const char *where, Token *where_tok, Node *operand,
                           Type *actual) {
    Diag d = {0};
    d.message = diag_fmt("%s には bool が必要です", where);
    ...
}
```

呼び出し側はこうなります。

```c
bool_required("演算子 'and'", n->tok, n->lhs, l);   // 第6章から
bool_required("if の条件", n->tok, cond, t);        // 第7章
bool_required("while の条件", n->tok, cond, t);
```

**★ 「1 つ上の章で作った関数が、次の章で少し一般化される」**のはよくある流れです。
最初から汎用に作ろうとせず、**2 つ目の利用者が現れてから一般化する**のが安全です。

---

## 7.7 ブロックスコープとシャドーイング禁止

### ✍️ scope_push / scope_pop がついに対で使われる

第5章で作った `scope_push` / `scope_pop` は、
トップレベルの 1 段にしか使っていませんでした。**ここで初めて入れ子になります。**

```c
static void check_block(Sema *s, Node *n) {
    scope_push(s);
    for (Node *st = n->body; st; st = st->next) check_stmt(s, st);
    scope_pop(s);
}
```

ブロックを抜けると、その中で宣言された変数は見えなくなります。

```python
if True:
    y: int = 1
    print(y)      # OK
print(y)          # ← エラー：未定義の名前 'y' です
```

### 📖 シャドーイングは禁止（言語仕様 5.1）

```python
x: int = 1
if True:
    x: int = 2     # ← エラー（Python なら同じ x への代入、C なら別の x）
```

**🤔 なぜ禁止するのか**

`x` と書いたときにどの `x` を指すのか、読み手が**字下げを目で追わないと分からない**
からです。禁止すれば、`x` は常に 1 つに決まります。

### ★ ここで第5章の判断 #54 が試験を受ける

第5章で `lookup`（外側まで探す）と `lookup_local`（現在のスコープだけ）の
**2 つを用意**し、こう書きました。

> `lookup` で再宣言を検査してしまうと、外側のスコープに同名があるだけで
> エラーになります（**第7章で問題化する**）。

実際に第7章に来てみると、**話は逆でした。**
シャドーイングを禁止するなら、「外側に同名があるだけでエラー」が**正しい動作**です。

そして 2 つの関数を分けておいたおかげで、
**2 種類のエラーを撃ち分けられます**。

```c
    // ① 同じスコープでの再宣言
    VarEntry *prev = lookup_local(s, n->name);
    if (prev) { /* 「既に宣言されています」 */ }

    // ② 外側のスコープの変数を隠す（シャドーイング）
    VarEntry *outer = lookup(s, n->name);
    if (outer) {
        Diag d = {0};
        d.message = diag_fmt("変数 '%s' は外側のスコープの変数を隠しています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "シャドーイングは禁止されています（言語仕様 5.1）";
        d.related.tok = outer->decl_tok;
        d.related.label = "外側の宣言はここです";
        d.hint = "別の名前にするか、型注釈を外して既存の変数に代入してください";
        diag_fail(&d);
    }
```

**★ 「2 つに分けておいた」判断が、想定とは違う形で報われました。**
分けておかなければ、この 2 つのエラーは同じ文言になっていたはずです。

---

## 7.8 第5章の前提が壊れる：IR 名の一意性

### ⚠️ ここが、この章でいちばん厄介な問題です

第5章で、変数の IR 名についてこう書きました。

> **シャドーイングを禁止している**（言語仕様 5.1）ので、
> 変数名がそのまま一意な IR 名になります。同名の変数が同時に存在しません。

**この前提は、ブロックスコープが入ると崩れます。**

```python
if True:
    x: int = 1
    print(x)
if True:
    x: int = 2      # ← シャドーイングではない（1 つ目の x はもう死んでいる）
    print(x)
0
```

2 つの `x` は**兄弟スコープ**にあります。
どちらも相手を隠していないので、シャドーイング禁止に引っかかりません。
**しかし、どちらも `%x` という IR 名を要求します。**

```llvm
entry:
  %x = alloca i64
  %x = alloca i64        ; ✗ 同じ名前を 2 回定義している
```

### ✅ 実際に何が起きるか

この問題を**わざと残した状態**（`n->ir_name` ではなく `n->name` を使う）で
コンパイルすると、こうなります。

```
error: multiple definition of local value named 'x'
   10 |   %x = alloca i64
      |   ^
1 error generated.
error: clang の実行に失敗しました（生成した IR に問題があります）
```

**LLVM が捕まえてくれました。** 黙って壊れるより、ずっとましです。
（第6章の `icmp` のときは**黙って間違った答え**を返したので、それに比べれば親切です。）

### ✍️ 解決：sema が IR 名を割り当てる

「名前が衝突したら連番を足す」だけです。**どこでやるかが問題**になります。

- **parser** … スコープを知らないので無理
- **codegen** … 変数の宣言と参照を結びつける情報を持っていない
- **sema** … ✅ シンボルテーブルを持っているので、宣言と参照を結びつけられる

```c
// sema.c
// これまでに使った IR 名を全部覚えておき、衝突したら連番を足す。
static char *unique_ir_name(Sema *s, const char *name) {
    if (!name_used(s, name)) {
        remember_name(s, name);
        return xstrdup(name);
    }
    for (int i = 1;; i++) {
        char *cand = xasprintf("%s.%d", name, i);
        if (!name_used(s, cand)) {
            remember_name(s, cand);
            return cand;
        }
    }
}
```

宣言のときに割り当て、`VarEntry` に覚えます。

```c
struct VarEntry {
    char *name;      // Mython 上の名前（エラーメッセージ用）
    char *ir_name;   // ★ LLVM 上の名前（%x, %x.1, ...）
    ...
};
```

**参照側は、シンボルテーブルを引いたときに一緒に受け取ります。**

```c
static Type *check_var(Sema *s, Node *n) {
    VarEntry *v = lookup(s, n->name);
    if (!v) { /* 未定義エラー */ }
    n->ir_name = v->ir_name;   // ★ codegen はこれを使う
    return v->type;
}
```

コード生成器は `n->name` ではなく `n->ir_name` を見るようになります。

```c
// Before（第5章）
static char *var_ptr(const char *name);   // x → %x

// After（第7章）
// sema が割り当てた一意な名前を使う
sb_printf(&e->allocas, "  %%%s = alloca %s\n", n->ir_name, llvm_mem_type(n->type));
```

### 📖 これが「名前修飾（mangling）」の入口です

同じ名前の別物に、機械的に別名を与える処理を**名前修飾**と呼びます。
第12章（クラスのメソッド）と第13章（モジュール）で本格的に必要になります。

**ここで小さく経験しておくと、後の章が楽になります。**

**★ 教訓：「今は成り立つ前提」には、いつ壊れるかを書いておく。**
第5章のコメントに「シャドーイングを禁止しているので一意」と**理由**を書いておいたので、
ブロックスコープを入れる段になって「あの前提はまだ有効か？」と検算できました。
理由を書かずに結論だけ書いていたら、気づかずに壊れた IR を出していたはずです。

---

## 7.9 コード生成：if

### ✍️ 設計文書のとおりに書く

```c
static void gen_if(Emitter *e, Node *n) {
    int id = e->label_counter++;      // ★ 番号は最初に 1 回だけ確保する

    char then_l[32], else_l[32], end_l[32];
    snprintf(then_l, sizeof(then_l), "if.then.%d", id);
    snprintf(else_l, sizeof(else_l), "if.else.%d", id);
    snprintf(end_l, sizeof(end_l), "if.end.%d", id);

    char *cond = gen_expr(e, n->lhs);
    emit_cond_br(e, cond, then_l, n->els ? else_l : end_l);

    emit_label(e, then_l);
    gen_stmt(e, n->body);
    if (!e->terminated) emit_br(e, end_l);

    if (n->els) {
        emit_label(e, else_l);
        gen_stmt(e, n->els);
        if (!e->terminated) emit_br(e, end_l);
    }

    emit_label(e, end_l);
}
```

**`else` が無ければ `else_l` を作らず、直接 `end_l` へ分岐します。**

### ⚠️ `if (!e->terminated) emit_br(...)` が要る理由

then 節が `break` や `continue` で終わっていたら、そこは**すでに終端済み**です。

```python
while c:
    if x:
        break        # ← ここで br label %while.end.0 が出ている
```

終端済みのブロックにもう 1 つ `br` を出すと、
**1 つの基本ブロックに終端命令が 2 つ**あることになり、LLVM がエラーにします。

`e->terminated` を見て抑制します。**第6章で作ったフラグがここで効きます。**

### 📖 `emit_label` が残りを吸収する

`emit_label(e, end_l)` は「終端していなければ `br` を補う」関数でした。
だから `if.end.N` に入る直前の状態がどうであれ、正しい IR になります。

**この 1 つの関数が、if / while の落とし穴のほとんどを消しています。**

---

## 7.10 コード生成：while と break / continue

### ✍️ while

```c
static void gen_while(Emitter *e, Node *n) {
    int id = e->label_counter++;

    char cond_l[32], body_l[32], end_l[32];
    snprintf(cond_l, sizeof(cond_l), "while.cond.%d", id);
    snprintf(body_l, sizeof(body_l), "while.body.%d", id);
    snprintf(end_l, sizeof(end_l), "while.end.%d", id);

    // ⚠️ 条件ブロックに「入る」ための br が必要（規約 6.4）
    emit_br(e, cond_l);

    emit_label(e, cond_l);
    char *cond = gen_expr(e, n->lhs);   // ★ 条件は毎回評価される
    emit_cond_br(e, cond, body_l, end_l);

    // break / continue の飛び先を積む
    LoopCtx ctx = {.outer = e->loop, .break_label = end_l, .continue_label = cond_l};
    e->loop = &ctx;

    emit_label(e, body_l);
    gen_stmt(e, n->body);
    if (!e->terminated) emit_br(e, cond_l);   // ループバック

    e->loop = ctx.outer;                      // ★ 対で戻す

    emit_label(e, end_l);
}
```

**⚠️ 条件を独立したブロックにするのが要点です。**
`entry` から本体に直接入ってしまうと、
**1 回目の条件判定が行われません**（do-while になってしまいます）。

**⚠️ `LoopCtx` をスタック変数で持つ**のがポイントです。
`gen_while` の呼び出しがネストすれば、C の呼び出しスタックがそのまま
ループのネストになります。**自前でスタック構造を作る必要はありません。**

### ✍️ break / continue

```c
        case ND_BREAK:
            emit_br(e, e->loop->break_label);
            break;
        case ND_CONTINUE:
            emit_br(e, e->loop->continue_label);
            break;
```

**`e->loop` が NULL でないことは sema が保証しています。**

```c
// sema.c
        case ND_BREAK:
        case ND_CONTINUE:
            if (s->loop_depth == 0) {
                Diag d = {0};
                d.message = diag_fmt("'%s' はループの外では使えません",
                                     n->kind == ND_BREAK ? "break" : "continue");
                ...
            }
            break;
```

**★ 「codegen は検査済みの正しい AST だけを受け取る」**（第5章で確立した役割分担）。
だから codegen 側に `if (!e->loop)` のチェックは書きません。

### ⚠️ `continue` の飛び先は「条件」

```
break    → while.end.N    （ループを抜ける）
continue → while.cond.N   （条件の再評価へ）
```

`continue` が `while.body.N` に飛ぶと**条件を評価せずに次の反復**に入ってしまい、
無限ループになります。第11章で `for` を実装するときは、
`continue` の飛び先が「増分処理」になるので、また変わります。

---

## 7.11 到達不能コード（規約 R7）

### 📖 終端の後ろに文が来ることがある

```python
while True:
    break
    print(1)      # ← 到達不能
```

`break` で `br` を出した後に `print` の命令を生成しようとすると、
**終端命令の後ろに命令が並ぶ**ことになり、LLVM がエラーにします。

> **R7. ブロックを終端した後にコードを生成する必要が生じたら、`dead.N` ラベルを作る。**

```c
// 終端済みのブロックの後ろにコードを置く必要が出たら、
// 到達不能ブロックのラベルを作る（規約 R7）
static void ensure_block(Emitter *e) {
    if (!e->terminated) return;
    char l[24];
    snprintf(l, sizeof(l), "dead.%d", e->label_counter++);
    emit_label(e, l);   // terminated 済みなので br は補われない
}
```

各文の生成の先頭で呼びます。

```c
static char *gen_stmt(Emitter *e, Node *n) {
    ensure_block(e);
    switch (n->kind) { ... }
}
```

**⚠️ 到達不能ブロックも終端しなければなりません**（規約 R6）。
`dead.N` の中身の後には、`while` のループバックや関数末尾の `ret` が続くので、
自然に終端されます。

**🤔 なぜ「到達不能です」という警告を出さないのか**
出すべきですが、警告の枠組み（エラーではない診断）をまだ作っていません。
`diag.c` は `diag_fail()`（出したら終了）しか持っていません。
**警告は第8章以降の課題**として、[dev-log](../dev-log.md) に記録しておきます。

---

## 7.12 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
全 118 件パス
```

31 件追加しました（print 3 件・if 6 件・while 3 件・break/continue 3 件・
スコープ 2 件・その他 3 件（pass / 到達不能 / FizzBuzz）・エラー 11 件）。
ビルド警告 0 件、ASan/UBSan も全ケースでクリーンです。

**★ 第1章から用意してあった `# OUTPUT:` が、この章で初めて使われました。**
終了コードだけの検証から抜け出せます。

### ✅ FizzBuzz（の制御構造）

```python
i: int = 1
while i <= 15:
    if i % 15 == 0:
        print(-15)
    elif i % 3 == 0:
        print(-3)
    elif i % 5 == 0:
        print(-5)
    else:
        print(i)
    i += 1
0
```

```bash
$ ./build/mythonc tests/cases/fizzbuzz.my -o t && ./t
1
2
-3
4
-5
-3
7
8
-3
-5
11
-3
13
14
-15
```

**⚠️ 言葉のかわりに負の数を出しています。** `"Fizz"` と書くには `str` 型が必要で、
それは第9章です。**先の章の機能を前借りしない**という方針を守っています。
制御構造（`while` / `if` / `elif` / `else` / `%` / `+=` / `print`）は
すべて本物なので、第9章で `print(-3)` を `print("Fizz")` に書き換えるだけで完成します。

### ✅ while の IR

```bash
$ ./build/mythonc -S tests/cases/while_count.my
```

```llvm
define i64 @mython_main() {
entry:
  %i = alloca i64
  store i64 1, ptr %i
  br label %while.cond.0          ; ★ 条件ブロックに「入る」ための br
while.cond.0:
  %t0 = load i64, ptr %i
  %t1 = icmp sle i64 %t0, 3       ; ★ 条件は反復のたびに評価される
  br i1 %t1, label %while.body.0, label %while.end.0
while.body.0:
  %t2 = load i64, ptr %i
  %t3 = call i32 (ptr, ...) @printf(ptr @.fmt.int, i64 %t2)
  %t4 = load i64, ptr %i
  %t5 = add i64 %t4, 1
  store i64 %t5, ptr %i
  br label %while.cond.0          ; ★ ループバック
while.end.0:
  ret i64 0
}
```

**設計文書（規約 6.4）の図と 1 対 1 で対応しています。**

### ✅ 兄弟スコープと IR 名

```python
if True:
    x: int = 1
    print(x)
if True:
    x: int = 2
    print(x)
0
```

```bash
$ ./build/mythonc tests/cases/scope_siblings.my -o t && ./t
1
2
$ ./build/mythonc -S tests/cases/scope_siblings.my | grep alloca
  %x = alloca i64
  %x.1 = alloca i64
```

**sema が `%x` と `%x.1` に振り分けています。** 名前修飾の最小形です。

### ✅ break / continue

```python
i: int = 0
while i < 5:
    i += 1
    if i % 2 == 0:
        continue
    print(i)
```

```
1
3
5
```

ネストしたループで内側だけを抜けることも確認しました（`break_nested.my`）。
**`LoopCtx` を C の呼び出しスタックに載せる方式が正しく働いています。**

### ✅ 到達不能コード（規約 R7）

```python
while True:
    break
    print(1)
print(2)
0
```

```bash
$ ./t
2
```

生成された IR には `dead.N` ラベルができています。

```llvm
dead.1:
  %t0 = call i32 (ptr, ...) @printf(ptr @.fmt.int, i64 1)
  br label %while.cond.0
```

**到達不能なコードも、ラベルを付けて終端しておけば LLVM は文句を言いません。**

### ✅ エラー：条件が bool でない

```
error: if の条件には bool が必要です
  --> t.my:2:4
   |
 2 | if x:
   |    ^ これは 'int' 型です
   |
note: if の条件はここです
  --> t.my:2:1
   |
 2 | if x:
   | ^^
   |
   = ヒント: Mython は int を真偽値として扱いません（言語仕様 4.4）。比較を書いてください（例: x != 0）
```

**第6章で `and` / `or` のために作った `bool_required()` が、そのまま使えました**
（「どこで必要か」を文字列で受け取る形に一般化）。

### ✅ エラー：シャドーイング

```
error: 変数 'x' は外側のスコープの変数を隠しています
  --> t.my:3:5
   |
 3 |     x: int = 2
   |     ^ シャドーイングは禁止されています（言語仕様 5.1）
   |
note: 外側の宣言はここです
  --> t.my:1:1
   |
 1 | x: int = 1
   | ^
   |
   = ヒント: 別の名前にするか、型注釈を外して既存の変数に代入してください（例: x = 1）
```

**第5章で `lookup` と `lookup_local` を分けておいたので、
「同じスコープの再宣言」と「外側を隠す宣言」を撃ち分けられます。**

### ✅ エラー：ループの外の break

```
error: 'break' はループの外では使えません
  --> t.my:1:1
   |
 1 | break
   | ^^^^^ この 'break' を囲む while がありません
   |
   = ヒント: 'break' は while の中でだけ使えます
```

### ✅ エラー：字下げ忘れ

```
error: 字下げされたブロックが必要です
  --> t.my:2:1
   |
 2 | print(1)
   | ^^^^^ ここは名前です
   |
   = ヒント: ':' の次の行は字下げしてください（スペース 4 個を推奨）
```

**⚠️ 「ここは IDENT です」と出さないこと。** 最初はそう実装してしまいましたが、
`INDENT` や `IDENT` は**コンパイラ内部の言葉**です。
`tok_kind_ja()` を用意して「名前」「整数」「改行」と日本語で出すよう直しました。

### ✅ エラー：孤立した else

```
error: 対応する if がない 'else' です
  --> t.my:2:1
   |
 2 | else:
   | ^^^^ この行に対応する 'if' が見つかりません
   |
   = ヒント: 'elif' / 'else' は 'if' と同じ字下げの位置に書いてください
```

### ✅ エラー：print はまだ int だけ

```
error: print はまだ 'bool' 型を出力できません
   ...
   = ヒント: 第7章の print は int 専用の暫定実装です（bool / str の出力は第9章で対応します）
```

**暫定実装であることと、いつ解消されるかを明示します**（第3章からの約束）。

---

## 7.13 まとめと次章の予告

### できたこと

```
✅ block ::= NEWLINE INDENT stmt { stmt } DEDENT — 第4章の仮想トークンを消費
✅ expect() — 第4章で「必要になる」と予告した関数を実装
✅ if / elif / else（elif は「else の中の if」に脱糖）
✅ while / break / continue / pass
✅ print(int) の暫定実装（globals / decls バッファが初めて使われた）
✅ 条件は bool（第6章の bool_required を一般化して再利用）
✅ ブロックスコープ（scope_push / scope_pop が初めて入れ子で対になる）
✅ シャドーイング禁止（lookup と lookup_local の撃ち分け）
✅ IR 名の一意化 — 第5章の前提が壊れたので sema が名前を割り当てる
✅ 到達不能ブロック dead.N（規約 R7）
✅ 「最後は式」の検査を is_expr_node() で列挙する形に反転
✅ テスト 118 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/lexer.c` | **変更なし**（予約語は第5章で確保済み、`print` は IDENT） |
| `src/ast.h/c` | `ND_IF` / `ND_WHILE` / `ND_BREAK` / `ND_CONTINUE` / `ND_PASS` / `ND_PRINT`、`els`、`ir_name`、ダンプ |
| `src/parser.c` | `expect` / `tok_kind_ja` / `expect_colon` / `block` / `if_stmt` / `while_stmt` / `print_stmt`、`stmt` の分岐 |
| `src/sema.c` | `check_block` / `check_cond` / `check_print`、`loop_depth`、シャドーイング検査、`unique_ir_name`、`is_expr_node` |
| `src/codegen.c` | `gen_if` / `gen_while` / `gen_print` / `ensure_block`、`LoopCtx`、`ir_name` の使用 |
| `tests/cases/` | 31 件追加 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | `# OUTPUT:` をテストランナーに用意 | `print` が入って初めて使われた |
| ch1 | `globals` / `decls` バッファ | `@.fmt.int` と `declare i32 @printf` の置き場所 |
| ch4 | `NEWLINE` / `INDENT` / `DEDENT` の**生成** | `block()` が**3 章越しに**消費した |
| ch4 | `expect()` の予告コメント | そのまま実装に変わった |
| ch5 | `peek_at()`（2 トークン先読み） | `print` `(` の判定に再利用 |
| ch5 | `lookup` と `lookup_local` を分けた | 再宣言とシャドーイングを撃ち分けた |
| ch5 | `collect_allocas` を再帰にした | 入れ子ブロックを 1 行（`els`）追加で対応 |
| ch5 | 「codegen は検査済み AST だけ受け取る」 | `break` の飛び先の存在を sema が保証 |
| ch6 | `emit_label` / `emit_br` / `emit_cond_br` | if / while がほぼこの 3 つだけで書けた |
| ch6 | `terminated` フラグ | `break` で終端済みのブロックに `br` を重ねない |
| ch6 | `bool_required()` | 条件式の検査にそのまま流用（少し一般化） |

### ⚠️ 壊れた前提（正直に記録する）

| 第5章で書いたこと | 第7章での現実 |
|---|---|
| 「シャドーイング禁止なので変数名がそのまま一意な IR 名になる」 | **兄弟スコープでは成り立たない**。sema が `%x` / `%x.1` を割り当てるよう変更 |
| 「最後の文が宣言か代入なら値を持たない」 | 文の種類が増えたので**列挙する向きを反転**（`is_expr_node`） |

**★ 「今は成り立つ前提」には、なぜ成り立つのかを書いておく。**
第5章のコメントに**理由**（シャドーイング禁止だから）を書いておいたおかげで、
ブロックスコープを入れる段になって「あの前提はまだ有効か？」と検算できました。
結論だけ書いていたら、気づかずに壊れた IR を出していたはずです。

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| `print` が int 専用（`"Fizz"` と書けない） | 第9章 |
| `print` が文（本物の関数呼び出しではない） | 第8章 |
| 到達不能コードの**警告**が出せない（`diag.c` は「出したら終了」しかない） | 第8章以降 |
| トップレベルが式文の並び（末尾に `0` が要る） | 第8章 |
| `for` 文 | 第11章 |
| `while ... else:` | 対応しない（言語仕様） |
| `1 // (2 - 2)` が実行時 SIGFPE | 第9章 |

### ✍️ commit する

```bash
git add -A
git commit -m "第7章: 制御構文（if / elif / else / while）"
```

---

## 次章：第8章 関数定義と呼び出し

**達成目標** — 再帰でフィボナッチが計算できる。

```python
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def main() -> int:
    print(fib(10))     # 55
    return 0
```

**やること**

| ファイル | 作業 |
|---|---|
| `parser.c` | `func_def`、`param_list`、`return_stmt`、**関数呼び出し `f(...)`**（postfix） |
| `ast.c` | `ND_FUNC` / `ND_CALL` / `ND_RETURN` |
| `sema.c` | **関数シグネチャの事前登録**（前方参照・再帰のため）、引数の型検査、戻り型の検査 |
| `sema.c` | **「全経路で return するか」の検査**（制御フロー解析の入口） |
| `codegen.c` | 関数ごとの生成、引数の `alloca` へのコピー（規約 R8）、`call` |
| 全体 | **暫定の足場を撤去**：`print` 文 → 組み込み関数、末尾の式 → `main` の `return` |

**★ この章で「暫定」が 3 つ消えます。**
第1章から積み上げてきた足場（暗黙の main / 末尾の式 / print 文）が、
すべて本物に置き換わります。

**⚠️ 予想される落とし穴**

- **シグネチャは本体より先に全部登録する**（`fib` が自分を呼べるように）
- 引数は `%n.arg` で受けて entry で `alloca` にコピーする（規約 R8）
- 「全経路で return」の検査は、この章の `terminated` と同じ発想を **AST 上で**やる
- `tmp_counter` / `label_counter` を**関数ごとにリセット**する（すでにそう書いてある）
- 再帰の深さ — スタックオーバーフローは検出しない（既知の課題として記録する）

### 🤔 第8章に入る前の練習問題

1. **`gen_while` の `emit_br(e, cond_l)`（最初の 1 行）を消して** `make test` を走らせ、
   何が起きるか確認する（**必ず元に戻す**）
2. **`continue` の飛び先を `while.body.N` に変えて**、`continue_basic.my` が
   どう壊れるか予想してから確かめる
3. **`if (!e->terminated) emit_br(e, end_l)` の条件を外して**、
   `break` を含む if でどんな LLVM エラーが出るか見る
4. `unique_ir_name` の連番を消して（常に `name` を返すようにして）、
   `scope_siblings.my` のエラーメッセージを読む
5. **なぜ `while` の条件を `while.cond.N` という独立したブロックにするのか**、
   `entry` から直接 `while.body.N` に入る版を書いて確かめる
