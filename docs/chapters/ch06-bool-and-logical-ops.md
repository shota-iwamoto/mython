# 第6章 bool・比較演算・論理演算

> **この章のゴール**
> `bool` 型が加わり、比較と論理演算ができるようになる。
>
> ```bash
> $ cat t.my
> x: int = 5
> flag: bool = x < 10 and not (x == 3)
> flag
> $ ./build/mythonc t.my -o t && ./t; echo $?
> 1
> ```

**この章は 3 つの節目です。**

1. **型が 2 つになる** — 第5章で書いた「型が一致しません」が**ついにテストできる**
2. **初めて基本ブロックを分岐させる** — 短絡評価のために `br` を出す（規約 R6・R7 の実践）
3. **値の表現とメモリの表現が食い違う型が現れる** — `bool` はレジスタ `i1` / メモリ `i8`（規約 R5）

第5章までは「1 つの式 → 1 つの値」という素直な変換でした。
この章から**コード生成が制御フローを組み立て始めます**。第7章の if / while はこの延長です。

---

## 目次

- [6.1 型が 2 つになると何が起きるか](#61-型が-2-つになると何が起きるか)
- [6.2 types.c に bool を足す](#62-typesc-に-bool-を足す)
- [6.3 字句解析器：比較演算子と最長一致（3 度目）](#63-字句解析器比較演算子と最長一致3-度目)
- [6.4 True / False はリテラルなのに予約語](#64-true--false-はリテラルなのに予約語)
- [6.5 構文解析器：階層の「上」に 4 段積む](#65-構文解析器階層の上に-4-段積む)
- [6.6 比較の連鎖を禁止する](#66-比較の連鎖を禁止する)
- [6.7 意味解析：比較は bool を返す](#67-意味解析比較は-bool-を返す)
- [6.8 コード生成：i1 と i8 の使い分け](#68-コード生成i1-と-i8-の使い分け)
- [6.9 コード生成：基本ブロックと終端フラグ](#69-コード生成基本ブロックと終端フラグ)
- [6.10 コード生成：短絡評価](#610-コード生成短絡評価)
- [6.11 動作確認](#611-動作確認)
- [6.12 まとめと次章の予告](#612-まとめと次章の予告)

---

## 6.1 型が 2 つになると何が起きるか

### 📖 第5章で書いた「テストできないコード」を回収する

第5章の最後に、正直にこう書きました。

> **⚠️ ただし「テストできないコードを書いた」ことは事実です。第6章で必ずテストを追加します。**

`check_vardecl` と `check_assign` には「型が一致しません」の検査が入っていますが、
型が `int` しか無いので**発火させる方法がありませんでした**。

この章で `bool` が入ると、こう書けます。

```python
x: int = True      # ← 型が一致しません
```

**型検査器が初めて本当の仕事をします。**

### 📖 この章で増える文法

```ebnf
expr       ::= or_expr

or_expr    ::= and_expr   { "or"  and_expr }      ← 新規
and_expr   ::= not_expr   { "and" not_expr }      ← 新規
not_expr   ::= "not" not_expr | comparison        ← 新規
comparison ::= bitor_expr [ compop bitor_expr ]   ← 新規（連鎖は不可）
compop     ::= "==" | "!=" | "<" | "<=" | ">" | ">="

bitor_expr ::= ...                                 ← 第2章で作った階層はそのまま
```

**第2章で作った階層の「上」に 4 段積むだけです。**
下の階層には一切手を触れません。これが階層化された文法の強みです。

```
       弱い ↑   or_expr        ← 新規
                and_expr       ← 新規
                not_expr       ← 新規
                comparison     ← 新規
                bitor_expr     ┐
                bitxor_expr    │
                bitand_expr    │
                shift_expr     ├ 第2章で作った部分（無変更）
                add_expr       │
                mul_expr       │
                unary          │
                power          │
       強い ↓   primary        ┘
```

**🤔 なぜ比較は算術より弱いのか**

`a + 1 < b * 2` を `(a + 1) < (b * 2)` と読みたいからです。
比較を算術より**弱く**（＝階層の上に）置くと、自動的にそうなります。
同じ理由で `and` は比較より弱く、`or` は `and` より弱くします。

---

## 6.2 types.c に bool を足す

### ✍️ 3 か所を足すだけ

第5章で「第6章の作業は型を 1 つ足すだけになる」と予告しました。**本当にそうなるか確かめます。**

```c
// types.h
typedef enum {
    TY_INT,   // int  → i64
    TY_BOOL,  // bool → i1（メモリ上は i8）
    // ── 以降の章で追加していく ──
} TypeKind;

extern Type *ty_int;
extern Type *ty_bool;
```

```c
// types.c
Type *ty_int;
Type *ty_bool;

void types_init(void) {
    ty_int = new_type(TY_INT);
    ty_bool = new_type(TY_BOOL);
}

const char *type_name(Type *t) {
    switch (t->kind) {
        case TY_INT: return "int";
        case TY_BOOL: return "bool";
        default: UNREACHABLE();
    }
}

Type *type_from_name(const char *name) {
    if (strcmp(name, "int") == 0) return ty_int;
    if (strcmp(name, "bool") == 0) return ty_bool;
    return NULL;
}

const char *type_name_list(void) { return "int, bool"; }
```

**`type_equal` は 1 文字も変えていません。** シングルトンなので `a == b` で判定が済みます。

### ✅ ここで効く：エラーメッセージが自動で育つ

`type_name_list()` を直したので、第5章で作った「未知の型名」のヒントが自動的に育ちます。

```
   = ヒント: 現在使える型: int, bool
```

**エラーメッセージを 1 か所にまとめておくと、機能追加が勝手に反映されます。**

---

## 6.3 字句解析器：比較演算子と最長一致（3 度目）

### ✍️ 記号表に 6 個足す

```c
static const char *PUNCTS[] = {
    // 3 文字
    "//=",
    // 2 文字
    "//", "**", "<<", ">>", "+=", "-=", "*=", "%=",
    "==", "!=", "<=", ">=",          // ← 追加
    // 1 文字
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "(", ")", ":", "=",
    "<", ">",                        // ← 追加
    NULL,
};
```

**⚠️ 最長一致の原則が 3 度目の登場です**（第2章・第5章に続いて）。

| 危ない組み合わせ | 先に書くべきもの | 間違えるとどうなるか |
|---|---|---|
| `<=` と `<` | `<=` | `x <= 1` が `x < (= 1)` に割れる |
| `>=` と `>` | `>=` | 同上 |
| `==` と `=` | `==` | `a == b` が `a = (= b)` になり「代入」と誤解される |
| `<<` と `<` | `<<` | `1 << 2` が `1 < (< 2)` に割れる |
| `!=` | （`!` は単独では記号でない） | `!` 単独はエラーのままでよい |

この表の 4 行目に注目してください。**`<<` はすでに 2 文字の段にあったので、
`<` を 1 文字の段に足しただけで自動的に守られます。**
第2章で「段」を作っておいた設計が、そのまま効いています。

**⚠️ `!` を単独の記号にしていないこと**は意図的です。
Mython に `!x`（否定）はありません（`not x` を使う）。
`!` 単独を記号にすると `!x` が「未知の演算子」ではなく
「`!` の後に `x`」と読まれてしまい、エラーが分かりにくくなります。

---

## 6.4 True / False はリテラルなのに予約語

### 📖 `True` は識別子ではない

`True` / `False` は**値**ですが、Mython では**予約語**です（言語仕様 2.5）。
第5章で作ったキーワード表に、すでに入っています。

```c
static const char *KEYWORDS[] = {
    "and", "as", "break", "class", "continue", "def", "elif", "else",
    "extern", "False", "for", "if", "import", "in", "is", "None",
    //        ~~~~~~~                                    ~~~~
    "not", "or", "pass", "return", "True", "while",
    // ~~~                         ~~~~~~
    ...
};
```

**第5章で「今使わないものまで予約する」判断をしておいたので、字句解析器は無変更です。**
`True` を変数名に使えないようにする作業は、5 章前に終わっていたことになります。

### ✍️ primary で True / False を受ける

第5章の `primary()` は、予約語が来たら無条件でエラーにしていました。
`True` / `False` だけは**先に**受け止めます。

```c
    Token *t = peek(p);
    ...
    // True / False は予約語だが、式として使える（値を持つ）。
    // ★ 「予約語はエラー」の判定より前に置くこと。
    if (tok_is_kw(t, "True")) {
        advance(p);
        return new_bool_node(t, true);
    }
    if (tok_is_kw(t, "False")) {
        advance(p);
        return new_bool_node(t, false);
    }

    // ここから下は「式として使えない予約語」
    if (t->kind == TK_KEYWORD)
        error_at_hint(t, "予約語は変数名として使えません（言語仕様 2.5）",
                      "'%s' は予約語です", t->text);
```

**⚠️ 順序が重要です。** 後ろに置くと `True` が
「'True' は予約語です」というエラーになります。

### ✍️ AST ノード

```c
// ast.h
typedef enum {
    ND_INT,      // 整数リテラル → ival
    ND_BOOL,     // 真偽値リテラル → ival（0 / 1）
    ND_BINOP,    // 二項演算（比較を含む）→ op, lhs, rhs
    ND_LOGICAL,  // and / or（★ 短絡評価するので別ノード）→ op, lhs, rhs
    ND_UNARY,    // 単項演算（not を含む）→ op, lhs
    ...
} NodeKind;
```

**🤔 なぜ `and` / `or` を `ND_BINOP` にしないのか**

見た目は二項演算ですが、**コード生成がまったく違う**からです。

| | `a + b` | `a and b` |
|---|---|---|
| 右辺の評価 | 必ず評価する | **評価しないことがある** |
| 生成される命令 | 1 個（`add`） | 分岐 + 2 ブロック |
| 基本ブロック | 増えない | **増える** |

`ND_BINOP` のままにすると `gen_expr` の中で
「op が AND/OR なら別処理」という分岐が必要になります。
**ノード種別で分けておけば、`switch` の別の枝になるだけです。**

これは「ノード種別は**構文の形**ではなく**生成のしかた**で分ける」という判断です。

```c
// ast.h — 演算子
typedef enum {
    // 二項（算術・ビット）
    OP_ADD, OP_SUB, OP_MUL, OP_TRUEDIV, OP_FLOORDIV, OP_MOD,
    OP_BITAND, OP_BITOR, OP_BITXOR, OP_SHL, OP_SHR,
    // 比較（★ この 6 つは連続して並べること。is_compare() が範囲で判定する）
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    // 論理
    OP_AND, OP_OR,
    // 単項
    OP_NEG, OP_POS, OP_BITNOT, OP_NOT,
} OpKind;
```

**⚠️ 比較演算子 6 個を連続して並べる**のは、こう書けるようにするためです。

```c
static bool is_compare(OpKind op) { return OP_EQ <= op && op <= OP_GE; }
```

enum の順序に意味を持たせるのは行儀が悪いとも言えますが、
`switch` で 6 個並べるより短く、**足し忘れが起きません**。
コメントで「連続して並べること」と明記しておきます。

---

## 6.5 構文解析器：階層の「上」に 4 段積む

### ✍️ キーワードを消費する部品

`and` / `or` / `not` は記号ではなく**キーワード**なので、`consume()` が使えません。
対になる部品を足します。

```c
// 現在のトークンが指定したキーワードなら消費して返す。違えば NULL。
static Token *consume_kw(Parser *p, const char *kw) {
    if (tok_is_kw(peek(p), kw)) return advance(p);
    return NULL;
}
```

`tok_is_kw()` は第5章で用意済みです。**部品が揃っていると章の作業が減ります。**

### ✍️ 4 つの関数

```c
// comparison ::= bitor_expr [ compop bitor_expr ]
//
// ⚠️ 連鎖しません（1 回だけ）。ループではなく if で書くのがポイント。
static Node *comparison(Parser *p) {
    Node *lhs = bitor_expr(p);

    Token *t = peek(p);
    int op = compare_op(t);
    if (op < 0) return lhs;      // 比較演算子がない → そのまま返す
    advance(p);

    Node *rhs = bitor_expr(p);

    // ★ ここで「もう 1 個比較演算子が続いていないか」を見る（6.6 節）
    if (compare_op(peek(p)) >= 0) { ... }

    return new_binop_node(t, (OpKind)op, lhs, rhs);
}

// not_expr ::= "not" not_expr | comparison
//
// ★ 右結合（自分自身を再帰で呼ぶ）。"not not x" が書ける。
static Node *not_expr(Parser *p) {
    Token *t = peek(p);
    if (consume_kw(p, "not")) return new_unary_node(t, OP_NOT, not_expr(p));
    return comparison(p);
}

// and_expr ::= not_expr { "and" not_expr }
//
// ★ 左結合（while ループで lhs を上書き）。第2章の add_expr と同じ形。
static Node *and_expr(Parser *p) {
    Node *lhs = not_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume_kw(p, "and"))
            lhs = new_logical_node(t, OP_AND, lhs, not_expr(p));
        else
            return lhs;
    }
}

// or_expr ::= and_expr { "or" and_expr }
static Node *or_expr(Parser *p) {
    Node *lhs = and_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume_kw(p, "or"))
            lhs = new_logical_node(t, OP_OR, lhs, and_expr(p));
        else
            return lhs;
    }
}

// expr ::= or_expr
static Node *expr(Parser *p) { return or_expr(p); }
```

**第2章で書いた `add_expr` / `unary` と、まったく同じ形です。**
左結合は `for` ループ、右結合は自己再帰。
**同じ道具を 4 回使い回しているだけ**で、新しい技法は 1 つも出てきません。

### 📖 変更点は `expr()` の 1 行だけ

```c
// Before（第5章）
static Node *expr(Parser *p) { return bitor_expr(p); }

// After（第6章）
static Node *expr(Parser *p) { return or_expr(p); }
```

`bitor_expr` 以下は**呼ばれる場所が変わっただけ**で、中身は無変更です。
括弧の中（`primary`）が `expr()` を呼び戻しているので、
`(a and b)` のような式も自動的に通ります。

---

## 6.6 比較の連鎖を禁止する

### 📖 Python との意図的な差異

Python では `1 < 2 < 3` が `(1 < 2) and (2 < 3)` の意味になります（比較連鎖）。
**Mython v1 では構文エラーにします**（言語仕様 4.1）。

**🤔 なぜ禁止するのか**

素直に実装すると `1 < 2 < 3` は `(1 < 2) < 3` = `True < 3` となり、
**bool と int の比較**になって型エラーになります。
つまり「放っておいてもエラーにはなる」のですが、
**エラーメッセージが意味不明**になります。

```
error: 型 'bool' と 'int' に演算子 '<' は適用できません   ← なぜ bool が出てくる？
```

そこで**構文の段階で捕まえて、専用のメッセージを出します**。

```c
    // ★ 比較の連鎖を禁止する（言語仕様 4.1）
    Token *t2 = peek(p);
    if (compare_op(t2) >= 0) {
        Diag d = {0};
        d.message = "比較演算子を連鎖させることはできません";
        d.primary.tok = t2;
        d.primary.label = "2 つ目の比較演算子です";
        d.related.tok = t;
        d.related.label = "1 つ目の比較演算子はここです";
        d.hint = "Python と違い連鎖比較は使えません。'and' で繋いでください"
                 "（例: a < b and b < c）";
        diag_fail(&d);
    }
```

**第3章で作った「関連する位置」がまた効きます。**
1 つ目と 2 つ目の比較演算子を両方示せば、何が起きたか一目で分かります。

**★ 「エラーになるからいい」ではなく「良いエラーになるか」を考える。**
これは第3章から一貫している姿勢です。

---

## 6.7 意味解析：比較は bool を返す

### ✍️ `op_supports` を書き換える

第5章の `op_supports` は `int` 専用でした。

```c
// Before（第5章）
static bool op_supports(OpKind op, Type *t) {
    if (t->kind == TY_INT) return op != OP_TRUEDIV;
    return false;
}
```

```c
// After（第6章）
static bool op_supports(OpKind op, Type *t) {
    // 比較は int どうし・bool どうしのどちらでも使える（型が同じことは検査済み）。
    // 言語仕様 4.3 / type-system.md 5.5
    if (is_compare(op)) return true;

    if (t->kind == TY_INT) return op != OP_TRUEDIV;
    return false;  // ★ bool に算術・ビット演算は使えない
}
```

たった 1 行足しただけで、`True + True` が正しく弾かれます。
**第5章で「両辺の型が等しいか」→「その型がその演算子を支持するか」の
2 段構えにしておいた構造が、そのまま活きています。**

### ✍️ 返す型を変える

```c
static Type *check_binop(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);
    Type *r = check_expr(s, n->rhs);

    if (!type_equal(l, r)) { /* ... 第5章のまま ... */ }
    if (!op_supports(n->op, l)) { /* ... 第5章のまま ... */ }
    if (/* 0 除算 */) { /* ... 第5章のまま ... */ }

    // ★ 比較は bool を返す。算術は両辺と同じ型を返す。
    return is_compare(n->op) ? ty_bool : l;
}
```

**変更は最後の 1 行だけです。**

### ✍️ 論理演算の検査

```c
// and / or は両辺が bool のみ（言語仕様 4.4）。
// Python と違い int を真偽値として扱いません（truthiness なし）。
static Type *check_logical(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);
    Type *r = check_expr(s, n->rhs);

    if (l->kind != TY_BOOL) return bool_required(n, n->lhs, l);
    if (r->kind != TY_BOOL) return bool_required(n, n->rhs, r);
    return ty_bool;
}
```

エラーの出し方は 3 か所（`and` の左、`and` の右、`not`）で同じなので、
**関数にまとめます**。第2章の `span_token`、第4章の `advance_newline` と同じ
「3 回目でまとめる」判断です。

```c
static Type *bool_required(Node *op_node, Node *operand, Type *actual) {
    Diag d = {0};
    d.message = diag_fmt("演算子 '%s' には bool が必要です", op_symbol(op_node->op));
    d.primary.tok = operand->tok;
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(actual));
    d.related.tok = op_node->tok;
    d.related.label = diag_fmt("演算子 '%s' はここです", op_symbol(op_node->op));
    d.hint = "Mython は int を真偽値として扱いません"
             "（言語仕様 4.4）。比較を書いてください（例: x != 0）";
    diag_fail(&d);
}
```

**⚠️ ヒントで「どう直すか」まで書く**のが第3章からの約束です。
`if x:` と書きたかった人に `x != 0` を教えます。

### ✍️ not の検査

```c
static Type *check_unary(Sema *s, Node *n) {
    Type *t = check_expr(s, n->lhs);

    if (n->op == OP_NOT) {
        if (t->kind != TY_BOOL) return bool_required(n, n->lhs, t);
        return ty_bool;
    }

    // - + ~ は int のみ
    if (t->kind != TY_INT)
        error_at(n->tok, "型 '%s' に単項演算子 '%s' は適用できません", type_name(t),
                 op_symbol(n->op));
    return t;
}
```

### 📖 truthiness を採用しないという決定

Python では `if xs:`（空リストが偽）が書けます。Mython では書けません。

| | Python | Mython |
|---|---|---|
| `1 and 2` | `2`（値を返す） | **型エラー** |
| `if xs:` | 空なら偽 | **型エラー**（`if len(xs) > 0:` と書く） |
| `not 0` | `True` | **型エラー** |

**🤔 なぜ捨てるのか**

`a and b` が「最後に評価した値」を返すと、**型が一意に決まりません**。

```python
x = 1 and "hello"     # x の型は int? str?
```

静的型付け言語では、この式に型を付けられません。
`bool` に固定すれば `and` / `or` の型は常に `bool` です
（[../spec/language-spec.md](../spec/language-spec.md) 8 節の差異表 #6）。

**言語仕様の制限が、型検査器とコード生成器の両方を単純にしています。**
第5章の「型注釈を必須にする」と同じ構図です。

---

## 6.8 コード生成：i1 と i8 の使い分け

### 📖 なぜ 2 つの表現が要るのか

> **R5. `bool` はメモリ上 `i8`、レジスタ上 `i1`。境界で `zext` / `trunc` する。**
> — [../design/ir-conventions.md](../design/ir-conventions.md) 4 節

**🤔 なぜ `alloca i1` にしないのか**

LLVM の `i1` は「1 ビット」ですが、メモリ上のバイト境界は 1 バイトです。
`alloca i1` は合法ですが、実際には 1 バイト確保され、
**7 ビットの中身が未定義**になります。
C の `_Bool` や他言語との相互運用（第9章のランタイム連携）で
`i8` に揃えておかないと、`0` でも `1` でもない値が入ることがあります。

**規約で先に決めておけば、迷う場所がありません。**

### ✍️ 型を返す関数を 2 つに分ける

```c
// 値（レジスタ）としての型
static const char *llvm_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_BOOL: return "i1";   // ← レジスタ上は i1
        default: UNREACHABLE();
    }
}

// メモリ（alloca / load / store）としての型（規約 R5）
static const char *llvm_mem_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_BOOL: return "i8";   // ← メモリ上は i8
        default: UNREACHABLE();
    }
}
```

**★ 「値の型」と「メモリの型」を別の関数にする**のがこの節の要点です。
1 つの関数で済ませようとすると、呼び出し側ごとに
「今はどっちの意味か」を考えることになり、必ず間違えます。

### ✍️ 境界で変換する

読み書きの 2 か所だけが「境界」です。

```c
// 変数を読む（load）：メモリ i8 → レジスタ i1
static char *gen_load(Emitter *e, Type *ty, const char *ptr) {
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = load %s, ptr %s\n", t, llvm_mem_type(ty), ptr);
    if (ty->kind != TY_BOOL) return t;

    char *t2 = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i8 %s to i1\n", t2, t);
    return t2;
}

// 変数に書く（store）：レジスタ i1 → メモリ i8
static void gen_store(Emitter *e, Type *ty, const char *val, const char *ptr) {
    if (ty->kind == TY_BOOL) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i8\n", t, val);
        val = t;
    }
    sb_printf(&e->fn, "  store %s %s, ptr %s\n", llvm_mem_type(ty), val, ptr);
}
```

**この 2 つの関数の中だけに `zext` / `trunc` を閉じ込めます。**
他の場所には 1 つも現れません。第9章で `float` や `str` が入っても、
変換が必要ならここに足すだけです。

### ✍️ 比較命令

```c
// 比較演算子に対応する icmp の述語。
//
// ⚠️ int は符号付きなので slt / sle / sgt / sge。
//    bool は符号なしで比べます（下の落とし穴を参照）。
static const char *icmp_pred(OpKind op, Type *operand_type) {
    bool sign = operand_type->kind == TY_INT;
    switch (op) {
        case OP_EQ: return "eq";
        case OP_NE: return "ne";
        case OP_LT: return sign ? "slt" : "ult";
        case OP_LE: return sign ? "sle" : "ule";
        case OP_GT: return sign ? "sgt" : "ugt";
        case OP_GE: return sign ? "sge" : "uge";
        default: UNREACHABLE();
    }
}
```

### ⚠️ 落とし穴：`i1` の符号付き比較は逆になる

**`False < True` を `icmp slt i1` で書くと `False` になります。**

`i1` は 1 ビットの整数です。2 の補数で解釈すると：

| 値 | 符号なし | 符号付き（1 ビット） |
|---|---|---|
| `0`（False） | 0 | 0 |
| `1`（True） | 1 | **-1** |

つまり `icmp slt i1 0, 1` は「0 < -1」を聞いていることになり、**偽**です。

```llvm
; ✗ False < True が False になる
%t = icmp slt i1 0, 1        ; → 0（偽）

; ✅ 符号なしで比べる
%t = icmp ult i1 0, 1        ; → 1（真）
```

**`eq` / `ne` には符号が無い**ので、この問題は `<` `<=` `>` `>=` だけで起きます。

`bool` の大小比較を使う場面はほぼありませんが、
**「型ごとに命令が変わる」最初の例**なので、正しく実装してテストを置きます
（`cmp_bool_order.my`）。第9章で `float` が入ると
`icmp` ではなく `fcmp` になり、この分岐がもう一段増えます。

### ⚠️ もう一つの落とし穴：オペランドの型は結果の型ではない

```c
// ✗ 間違い：n->type は bool（比較の結果）なので "i1" になってしまう
sb_printf(&e->fn, "  %s = icmp %s %s %s, %s\n", t, pred, llvm_type(n->type), l, r);

// ✅ 正しい：比べているのは「左辺の型」
Type *ot = n->lhs->type;
sb_printf(&e->fn, "  %s = icmp %s %s %s, %s\n", t, pred, llvm_type(ot), l, r);
```

第5章までは「二項演算の結果の型 == オペランドの型」だったので、
`llvm_type(n->type)` で正しく動いていました。
**比較演算子で初めてこの前提が崩れます。**

**⚠️ そしてこの間違いは、エラーにならないことがあります。**

`4 < 10` に対して `icmp slt i1 4, 10` を出したらどうなるか、実際に試しました。

```bash
$ cat bad.ll
define i32 @main() {
entry:
  %a = icmp slt i1 4, 10
  %b = zext i1 %a to i32
  ret i32 %b
}
$ llvm-as bad.ll -o - | llvm-dis - | grep icmp
  %a = icmp slt i1 false, false      ← 4 と 10 が i1 に切り詰められた！
$ lli bad.ll; echo $?
0                                    ← 正しくは 1（4 < 10 は真）
```

**エラーは 1 つも出ず、黙って間違った答えを返します。**
`4`（下位 1 ビットが 0）も `10`（同じく 0）も `false` に切り詰められ、
`false < false` = 偽になったのです。

オペランドがレジスタなら、さすがに捕まります。

```
error: '%v' defined with type 'i64' but expected 'i1'
```

**★ 「定数だけだと黙って通ってしまう」のが最悪のパターンです。**

実際にこの間違い（`n->lhs->type` を `n->type` に変える）を仕込んで
`make test` を走らせたところ、**8 件が落ちました**。

```
FAIL  bool_goal.my        FAIL  cmp_precedence.my
FAIL  cmp_all_ops.my      FAIL  sc_and_evaluates_rhs.my
FAIL  cmp_lt.my           FAIL  sc_and_skips_rhs.my
FAIL  cmp_negative.my     FAIL  sc_or_skips_rhs.my
```

落ち方は 2 種類ありました。

| 落ち方 | 例 | 理由 |
|---|---|---|
| **LLVM がエラーにする** | `bool_goal.my`（`x < 10`） | オペランドがレジスタなので型が合わないと弾かれる |
| **黙って違う答えを返す** | `cmp_lt.my`（`3 < 5`） | 定数が i1 に切り詰められる（`3`→`true`, `5`→`true`） |

`cmp_negative.my`（`-1 < 0`）が捕まえてくれるのは、
**`-1` が定数ではなくレジスタになる**からです
（第2章のとおり、単項マイナスは `sub i64 0, 1` を出します）。

**★ 「テストが本当に何かを守っているか」は、わざと壊して確かめる。**
これは第4章で `# TOKENS:` を検証したときと同じやり方です。

### ✍️ not

```c
        case OP_NOT: {
            // not x は x XOR true（i1 の全ビット反転）
            char *t = new_tmp(e);
            sb_printf(&e->fn, "  %s = xor i1 %s, true\n", t, v);
            return t;
        }
```

`~x`（`xor i64 %x, -1`）と同じ発想です。**幅が 1 ビットになっただけ。**

### ✍️ 最後の式が bool のとき

`@mython_main` は `i64` を返します。プログラムの最後の式が `bool` だと型が合いません。

```c
    // 最後の式が bool なら i64 に広げてから返す
    if (ast->type->kind == TY_BOOL) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, last);
        last = t;
    }
    sb_printf(&e->fn, "  ret i64 %s\n", last);
```

これで `True` を書いたプログラムが終了コード 1 になります。
（第8章で `def main() -> int:` になれば、この暫定処理は消えます。）

---

## 6.9 コード生成：基本ブロックと終端フラグ

### 📖 IR にフォールスルーは無い

> **R6. すべての基本ブロックは、必ず 1 つの終端命令で終わる。**

C なら「次の行に進む」だけのところでも、IR では**明示的に `br` を書きます**。

```llvm
; ✗ エラー：終端命令がない
entry:
  %t0 = add i64 1, 2
next:
  ...

; ✅ 正しい
entry:
  %t0 = add i64 1, 2
  br label %next        ; ← 書かないとエラー
next:
  ...
```

**⚠️ ここが初心者の最大の落とし穴です。** そして、
**忘れても気づきにくい**（生成が終わってから LLVM に怒られる）のがたちの悪いところです。

### ✍️ 1 つの関数で吸収する

規約 R7 が用意している解決策をそのまま実装します。

```c
typedef struct {
    ...
    int label_counter;  // ラベルの連番（関数ごとにリセット）
    bool terminated;    // 現在のブロックが終端命令を出力済みか
} Emitter;

// ラベルを出力する。
//
// ★ 直前のブロックが終端していなければ、暗黙のジャンプを補う。
//   この 1 つの関数が、基本ブロックの落とし穴のほとんどを吸収します。
static void emit_label(Emitter *e, const char *label) {
    if (!e->terminated) sb_printf(&e->fn, "  br label %%%s\n", label);
    sb_printf(&e->fn, "%s:\n", label);
    e->terminated = false;
}

// 分岐命令を出す（出したら終端済みにする）
static void emit_br(Emitter *e, const char *label) {
    sb_printf(&e->fn, "  br label %%%s\n", label);
    e->terminated = true;
}

static void emit_cond_br(Emitter *e, const char *cond, const char *then_l,
                         const char *else_l) {
    sb_printf(&e->fn, "  br i1 %s, label %%%s, label %%%s\n", cond, then_l, else_l);
    e->terminated = true;
}
```

**「終端したか」を追跡する変数を 1 つ持つだけ**で、
「`br` を書き忘れた」も「終端の後に命令を置いた」も起きなくなります。

第7章の if / while、第8章の return は**この 3 つの関数の上に載ります**。
この章で作っておくのが目的です。

### ⚠️ ラベルは必ず連番にする

```llvm
and.rhs.0:      ; 1 個目の and
and.rhs.1:      ; 2 個目の and（ネストしていても衝突しない）
```

同じ関数内で同じラベル名を 2 回使うと LLVM がエラーにします。
`label_counter` から採番し、**その and / or が使う番号を最初に確保**します。

```c
    int id = e->label_counter++;   // ★ 最初に確保する
```

**⚠️ 使うたびに `e->label_counter++` すると、
同じ and の中で番号がずれます**（`and.rhs.0` に `br` して `and.rhs.1:` を出す、など）。
必ず先頭で 1 回だけ取ります。

---

## 6.10 コード生成：短絡評価

### 📖 何を作るのか

```python
a and b
```

- `a` が偽なら、**`b` を評価せずに** 偽
- `a` が真なら、`b` の値がそのまま結果

```python
a or b
```

- `a` が真なら、**`b` を評価せずに** 真
- `a` が偽なら、`b` の値がそのまま結果

**「評価しない」を実現するには、命令を飛び越える必要があります。** だから分岐が要ります。

### ✍️ 結果を alloca に置く方式

設計文書（[../design/ir-conventions.md](../design/ir-conventions.md) 6.6 節）で
決めたとおりに書きます。

```c
static char *gen_logical(Emitter *e, Node *n) {
    int id = e->label_counter++;
    const char *kind = n->op == OP_AND ? "and" : "or";

    char rhs_l[32], end_l[32], res[32];
    snprintf(rhs_l, sizeof(rhs_l), "%s.rhs.%d", kind, id);
    snprintf(end_l, sizeof(end_l), "%s.end.%d", kind, id);
    snprintf(res, sizeof(res), "%%%s.result.%d", kind, id);

    // 結果を入れる箱（★ alloca は entry ブロックへ：規約 R1）
    sb_printf(&e->allocas, "  %s = alloca i8\n", res);

    // ① 左辺を評価して、その値を結果として置いておく
    char *l = gen_expr(e, n->lhs);
    gen_store(e, ty_bool, l, res);

    // ② 右辺を評価すべきか分岐する
    //    and: 左が真なら右へ / or: 左が偽なら右へ（真偽が逆）
    if (n->op == OP_AND)
        emit_cond_br(e, l, rhs_l, end_l);
    else
        emit_cond_br(e, l, end_l, rhs_l);

    // ③ 右辺（飛ばされることがあるブロック）
    emit_label(e, rhs_l);
    char *r = gen_expr(e, n->rhs);
    gen_store(e, ty_bool, r, res);
    emit_br(e, end_l);

    // ④ 合流点
    emit_label(e, end_l);
    return gen_load(e, ty_bool, res);
}
```

**★ `gen_load` / `gen_store` をそのまま使えています。**
6.8 節で `i1` ↔ `i8` の変換を関数に閉じ込めておいたので、
この関数には `zext` も `trunc` も 1 つも出てきません。

### 📖 `phi` を使わない理由（規約 R3）

教科書的には、合流点で `phi` 命令を使います。

```llvm
and.end.0:
  %r = phi i1 [ false, %entry ], [ %t2, %and.rhs.0 ]   ; ← 使わない
```

`phi` は「どのブロックから来たか」を書く必要があり、
**生成側が前のブロックのラベルを覚えていなければなりません**。
ネストすると管理が急激に面倒になります。

「alloca に置いて最後に読む」方式なら、**その面倒がゼロ**です。
そして `mem2reg` が**この `alloca` を `phi` に変換してくれます**
（6.11 節で実測します）。

> **私たちは「全部 alloca」で素朴に書く。LLVM が SSA に直す。**
> — [../design/ir-conventions.md](../design/ir-conventions.md) 1 節

第5章では「再代入」でこの規約の恩恵を受けました。
**この章では「制御フローの合流」で受けます。** こちらが本命です。

### ✍️ alloca 専用バッファ

ここで第5章の `collect_allocas`（AST を歩いて変数の alloca を集める）だけでは
足りなくなります。

**🤔 なぜ足りないのか**

`and.result.0` は**ソースコードに現れない、コンパイラが自分で作った領域**です。
AST を歩いても見つかりません。**生成してみて初めて必要だと分かります。**

そこで **alloca 専用のバッファ**を用意し、生成中に溜めて、最後に entry へ差し込みます。

```c
typedef struct {
    StrBuf header;   // source_filename, target triple
    StrBuf globals;  // グローバル変数・文字列定数
    StrBuf decls;    // declare（外部関数宣言）
    StrBuf body;     // 完成した関数定義

    // ── 生成中の関数用（関数ごとにリセット）──
    StrBuf allocas;  // ★ entry ブロックに置く alloca（規約 R1）
    StrBuf fn;       // 関数本体の命令列
    ...
} Emitter;
```

```c
static void gen_mython_main(Emitter *e, Node *ast) {
    ...
    collect_allocas(e, ast);   // ① 変数の alloca（第5章のまま）→ e->allocas

    char *last = NULL;         // ② 本体 → e->fn（この間に e->allocas が増える）
    for (Node *s = ast->body; s; s = s->next) { ... }
    ...
    // ③ 組み立て：entry の直後に alloca をまとめて差し込む
    sb_printf(&e->body, "define i64 @mython_main() {\nentry:\n");
    sb_printf(&e->body, "%s", sb_str(&e->allocas));
    sb_printf(&e->body, "%s", sb_str(&e->fn));
    sb_printf(&e->body, "}\n");
}
```

**★ 「後から前に戻って書き足したい」ときはバッファを分ける。**
第1章で 4 つのバッファに分けた理由がここでも効いています
（当時は「文字列リテラルを見つけたら globals に追記する」例で説明しました）。

第5章の `collect_allocas` は**書き込み先を `e->body` から `e->allocas` に変えるだけ**です。
第10章・第11章で一時領域が増えても、同じ仕組みで足ります。

---

## 6.11 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
全 87 件パス
```

26 件追加しました（正常系 15 件、短絡評価 3 件、エラー 8 件）。
ビルド警告 0 件、ASan/UBSan も全ケースでクリーンです。

### ✅ 達成目標

```bash
$ cat tests/cases/bool_goal.my
x: int = 5
flag: bool = x < 10 and not (x == 3)
flag
$ ./build/mythonc tests/cases/bool_goal.my -o t && ./t; echo $?
1
```

### ✅ AST

```bash
$ ./build/mythonc --dump-ast tests/cases/bool_goal.my
(block
  (vardecl x int
    (int 5)
  )
  (vardecl flag bool
    (logical and
      (binop <
        (var x)
        (int 10)
      )
      (unary not
        (binop ==
          (var x)
          (int 3)
        )
      )
    )
  )
  (var flag)
)
```

`logical` と `binop` が**別の種別として表示されている**ことに注目してください。
短絡評価するものとしないものが、木の上で区別できています。

### ✅ 最長一致

```bash
$ ./build/mythonc --dump-tokens t.my
...
  7  PUNCT     2:3    <=
```

`<=` が 1 個のトークンになっています（`<` と `=` に割れていません）。

### ✅ i1 と i8 の境界（規約 R5）

```bash
$ ./build/mythonc -S tests/cases/bool_var.my
```

```llvm
define i64 @mython_main() {
entry:
  %flag = alloca i8            ; ← メモリは i8
  %t0 = zext i1 true to i8     ; ← 書くとき i1 → i8
  store i8 %t0, ptr %flag
  %t1 = load i8, ptr %flag
  %t2 = trunc i8 %t1 to i1     ; ← 読むとき i8 → i1
  %t3 = zext i1 %t2 to i64     ; ← 最後の式なので i64 に広げて返す
  ret i64 %t3
}
```

**`zext` / `trunc` が `gen_load` / `gen_store` の中だけから出ている**ことが確認できます。

### ✅ 短絡評価の IR

```bash
$ ./build/mythonc -S tests/cases/bool_goal.my
```

```llvm
define i64 @mython_main() {
entry:
  %x = alloca i64
  %flag = alloca i8
  %and.result.0 = alloca i8        ; ★ コンパイラが自分で作った領域
  store i64 5, ptr %x
  %t0 = load i64, ptr %x
  %t1 = icmp slt i64 %t0, 10       ; ★ オペランドは i64（結果は i1）
  %t2 = zext i1 %t1 to i8
  store i8 %t2, ptr %and.result.0
  br i1 %t1, label %and.rhs.0, label %and.end.0
and.rhs.0:                         ; ★ 左辺が偽なら飛ばされる
  %t3 = load i64, ptr %x
  %t4 = icmp eq i64 %t3, 3
  %t5 = xor i1 %t4, true           ; not
  %t6 = zext i1 %t5 to i8
  store i8 %t6, ptr %and.result.0
  br label %and.end.0
and.end.0:
  %t7 = load i8, ptr %and.result.0
  %t8 = trunc i8 %t7 to i1
  %t9 = zext i1 %t8 to i8
  store i8 %t9, ptr %flag
  %t10 = load i8, ptr %flag
  %t11 = trunc i8 %t10 to i1
  %t12 = zext i1 %t11 to i64
  ret i64 %t12
}
```

**3 つの基本ブロックができました。** `alloca` は 3 つとも entry にあります（規約 R1）。

### ★ mem2reg が `phi` を書いてくれる

この章の見どころです。**私たちが書かなかった `phi` を、LLVM が入れてくれます。**

```bash
$ opt -passes=mem2reg -S t.ll
```

```llvm
define i64 @mython_main() {
entry:
  %t1 = icmp slt i64 5, 10
  %t2 = zext i1 %t1 to i8
  br i1 %t1, label %and.rhs.0, label %and.end.0

and.rhs.0:                                        ; preds = %entry
  %t4 = icmp eq i64 5, 3
  %t5 = xor i1 %t4, true
  %t6 = zext i1 %t5 to i8
  br label %and.end.0

and.end.0:                                        ; preds = %and.rhs.0, %entry
  %and.result.0.0 = phi i8 [ %t6, %and.rhs.0 ], [ %t2, %entry ]
  ...
}
```

**`%and.result.0` という `alloca` が、そのまま `phi i8` になりました。**

> **私たちは「全部 alloca」で素朴に書く。LLVM が SSA に直す。**

第5章では「再代入」で、この章では「**制御フローの合流**」でこの規約の恩恵を受けました。
支配辺境も `phi` 挿入アルゴリズムも、やはり 1 行も書いていません。

### ✅ `-O2` では全部消える

```llvm
define noundef i64 @mython_main() local_unnamed_addr #0 {
entry:
  ret i64 1
}
```

### ✅ 短絡評価が「本当に評価を飛ばす」ことの確認

**⚠️ この章にはまだ `print` も関数呼び出しも無いので、
「右辺が評価されたかどうか」を普通の方法では観測できません。**

そこで **0 除算（実行時 SIGFPE）を踏み台にします。**
右辺が評価されたらクラッシュするので、**正常終了すること自体が証拠**になります。

```python
# tests/cases/sc_and_skips_rhs.my
z: int = 0
False and (1 // z) > 0
```

```bash
$ ./build/mythonc tests/cases/sc_and_skips_rhs.my -o t && ./t; echo $?
0
```

右辺を評価してしまう版（`False` を `True` に変えたもの）で確かめると：

```bash
$ ./t; echo $?
136        ← SIGFPE（128 + 8）
```

**評価されれば 136、飛ばされれば 0。** 短絡評価が効いていることが確定します。

飛ばしすぎていないことも確認します（`sc_and_evaluates_rhs.my`）。

```python
z: int = 4
True and (8 // z) == 2      # → 右辺は評価される → True
```

### ✅ i1 の符号付き比較の落とし穴

手で IR を書いて確かめました。

```bash
$ lli slt.ll; echo $?     # icmp slt i1 0, 1
0                         ← False（間違い）
$ lli ult.ll; echo $?     # icmp ult i1 0, 1
1                         ← True（正しい）
```

`cmp_bool_order.my`（`False < True`）が期待どおり `ult` を出しています。

```bash
$ ./build/mythonc -S tests/cases/cmp_bool_order.my | grep icmp
  %t0 = icmp ult i1 false, true
```

### ✅ エラー：型が一致しません（**第5章から持ち越した宿題**）

```
error: 型が一致しません
  --> tests/cases/err_type_mismatch_decl.my:4:10
   |
 4 | x: int = True
   |          ^^^^ 型 'bool' の式
   |
note: 変数 'x' は 'int' 型として宣言されています
  --> tests/cases/err_type_mismatch_decl.my:4:1
   |
 4 | x: int = True
   | ^
```

**第5章で書いたまま一度も発火していなかったコードが、初めてテストされました。**
`related`（宣言の位置）もそのまま機能しています。

### ✅ エラー：int を真偽値として使えない

```
error: 演算子 'and' には bool が必要です
  --> tests/cases/err_and_needs_bool.my:4:1
   |
 4 | 1 and 2
   | ^ これは 'int' 型です
   |
note: 演算子 'and' はここです
  --> tests/cases/err_and_needs_bool.my:4:3
   |
 4 | 1 and 2
   |   ^^^
   |
   = ヒント: Mython は int を真偽値として扱いません（言語仕様 4.4）。比較を書いてください（例: x != 0）
```

**「どう直すか」（`x != 0`）まで書いてあります。**

### ✅ エラー：比較の連鎖

```
error: 比較演算子を連鎖させることはできません
  --> tests/cases/err_chained_cmp.my:5:7
   |
 5 | 1 < 2 < 3
   |       ^ 2 つ目の比較演算子です
   |
note: 1 つ目の比較演算子はここです
  --> tests/cases/err_chained_cmp.my:5:3
   |
 5 | 1 < 2 < 3
   |   ^
   |
   = ヒント: Python と違い連鎖比較は使えません。'and' で繋いでください（例: a < b and b < c）
```

「型 'bool' と 'int' に演算子 '<' は…」という**意味不明なエラーを回避できています**。

### ✅ エラーメッセージが自動で育った

型を 1 つ足しただけで、第5章のヒントが勝手に更新されました。

```
error: 未知の型名 'foo' です
   ...
   = ヒント: 現在使える型: int, bool
                            ~~~~~~ ← 自動で増えた
```

---

## 6.12 まとめと次章の予告

### できたこと

```
✅ TY_BOOL の追加（types.c の変更は 5 行）
✅ 比較演算子 6 種（== != < <= > >=）と最長一致
✅ True / False リテラル（字句解析器は無変更：第5章で予約済みだった）
✅ or / and / not — 階層の上に 4 段積む
✅ 比較連鎖の禁止（専用の診断メッセージ）
✅ int を真偽値扱いしない（truthiness なし）の型検査
✅ i1 / i8 の使い分けを gen_load / gen_store に閉じ込めた（規約 R5）
✅ 基本ブロックの 3 部品（emit_label / emit_br / emit_cond_br、規約 R6・R7）
✅ 短絡評価 — 初めて制御フローを生成した
✅ mem2reg が alloca を phi に変換することを実測確認
✅ alloca 専用バッファ（コンパイラが作る一時領域のため）
✅ 第5章で未テストだった「型が一致しません」をテスト
✅ テスト 87 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/types.h/c` | `TY_BOOL` / `ty_bool` / `type_from_name` / `type_name_list` |
| `src/lexer.c` | 記号表に `==` `!=` `<=` `>=` `<` `>` を追加（**それだけ**） |
| `src/ast.h/c` | `ND_BOOL` / `ND_LOGICAL`、比較 6 種と `OP_AND` / `OP_OR` / `OP_NOT`、`is_compare()`、ダンプ |
| `src/parser.c` | `consume_kw`、`compare_op`、`comparison` / `not_expr` / `and_expr` / `or_expr`、`True`/`False`、連鎖の禁止 |
| `src/sema.c` | `check_logical`、`bool_required`、`op_supports` に比較、`not` の検査 |
| `src/codegen.c` | `llvm_mem_type` / `gen_load` / `gen_store` / `icmp_pred` / `emit_*` / `gen_logical`、alloca バッファ |
| `tests/cases/` | 26 件追加 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch2 | 文法を**階層化**した | 上に 4 段積むだけ。下の階層は無変更 |
| ch2 | 記号表を**長さごとの段**に分けた | `<` を足しても `<<` が壊れない |
| ch2 | 単項マイナスは `sub i64 0, 1` | `-1 < 0` がレジスタになり、型取り違えを検出できた |
| ch3 | `Diag.related` | 「1 つ目の比較演算子はここ」「宣言はここ」 |
| ch5 | **未使用の予約語まで予約**した | `True` / `False` / `and` / `or` / `not` が字句解析器で無変更 |
| ch5 | 型検査を**2 段構え**にした | `op_supports` に 1 行足すだけで `True + True` を弾けた |
| ch5 | `type_name_list()` を 1 か所に | エラーメッセージが自動で育った |
| 設計 | `alloca` 方式（R1・R3） | **`phi` を 1 行も書かずに制御フローの合流を実現** |
| 設計 | truthiness を採用しない | `and` / `or` の型が常に `bool` に決まる |

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| `is` / `is not` / `in` / `not in`（`compop` の残り） | 第10章・第12章 |
| `bool` を `print` できない（`print` 自体が無い） | 第7章 |
| 短絡評価の観測に 0 除算を使っている（`print` があれば素直に書ける） | 第7章 |
| `1 // (2 - 2)` が実行時 SIGFPE | 第9章 |
| `float` の比較（`fcmp`）| 第9章 |
| トップレベルが式文の並び（暫定仕様） | 第8章 |
| `**` 未実装 | 第9章 |

### ✍️ commit する

```bash
git add -A
git commit -m "第6章: bool・比較演算・論理演算"
```

---

## 次章：第7章 制御構文（if / elif / else / while）

**達成目標** — FizzBuzz が書ける。

```python
i: int = 1
while i <= 15:
    if i % 15 == 0:
        print("FizzBuzz")
    elif i % 3 == 0:
        print("Fizz")
    else:
        print(i)
    i += 1
```

**やること**

| ファイル | 作業 |
|---|---|
| `parser.c` | `if_stmt` / `while_stmt`、**`INDENT` / `DEDENT` を消費する**（第4章の回収） |
| `ast.c` | `ND_IF` / `ND_WHILE` / `ND_BLOCK`（入れ子） |
| `sema.c` | **条件式が `bool` であることの検査**、ブロックスコープ（`scope_push` / `scope_pop`） |
| `codegen.c` | `emit_label` / `emit_cond_br` の本格運用、`break` / `continue` のジャンプ先管理 |
| `codegen.c` | `print` の暫定実装（`declare i32 @printf`） |

**★ 第4章で作った `INDENT` / `DEDENT` が、3 章越しに消費されます。**
「生成側だけ作って `--dump-tokens` で検証する」という第4章の判断が、ここで完結します。

**この章で作った 3 つの関数がそのまま土台になります。**
`if` は `emit_cond_br` + 2 ブロック、`while` は「条件ブロックへ戻る `br`」が増えるだけです。
**短絡評価が書けたなら、if / while はもう書けます。**

**⚠️ 予想される落とし穴**

- `elif` は「`else` の中に `if` が 1 個ある」形に脱糖するのが簡単（第5章の複合代入と同じ発想）
- `while` の**条件は毎回評価する**（ループの先頭にラベルを置く）
- `break` / `continue` のジャンプ先はスタックで管理する（ネストしたループ）
- ブロックスコープで宣言された変数の `alloca` は、やはり entry に集める（規約 R1）
- 条件式が `bool` でないときの診断は、この章の `bool_required` がそのまま使える

**予習**：[../design/ir-conventions.md](../design/ir-conventions.md) の
6.3 節（if 文）・6.4 節（while 文）・6.5 節（break / continue）。

### 🤔 第7章に入る前の練習問題

1. **`icmp_pred` の `ult` を `slt` に変えて** `make test` を走らせ、
   `cmp_bool_order.my` だけが落ちることを確認する（**必ず元に戻す**）
2. **`gen_logical` の `emit_cond_br` の 2 つのラベルを入れ替えて**、
   どのテストが落ちるか予想してから確かめる
3. **`e->label_counter++` を `id` に取らず、使うたびに `++` するように変えて**
   生成された IR を読む（どのラベルがずれるか）
4. `not not not True` の AST と IR を見て、**`xor` が 3 回出る**ことを確認する
   （LLVM の `-O2` は何回に減らすか？）
