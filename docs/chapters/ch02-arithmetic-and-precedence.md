# 第2章 四則演算と演算子の優先順位

> **この章のゴール**
> 演算子の優先順位と結合性を正しく扱う電卓を作る。
>
> ```bash
> $ cat t.my
> 2 * 3 + (10 - 4) * 6
> $ ./build/mythonc t.my -o t && ./t; echo $?
> 42
> ```

第1章で通した管に、**式**を流し込みます。
この章で学ぶ「優先順位を関数の呼び出し階層で表現する」技法は、
再帰下降構文解析の核心であり、**第6章（比較・論理演算）でそのまま再利用します**。

---

## 目次

- [2.1 この章で作る文法](#21-この章で作る文法)
- [2.2 なぜ関数の階層が優先順位になるのか](#22-なぜ関数の階層が優先順位になるのか)
- [2.3 左結合と右結合の書き分け](#23-左結合と右結合の書き分け)
- [2.4 ① 字句解析器に記号を追加する](#24--字句解析器に記号を追加する)
- [2.5 AST に二項・単項ノードを追加する](#25-ast-に二項単項ノードを追加する)
- [2.6 ② 構文解析器：優先順位の階層を作る](#26--構文解析器優先順位の階層を作る)
- [2.7 ④ コード生成：演算子を LLVM 命令に対応させる](#27--コード生成演算子を-llvm-命令に対応させる)
- [2.8 設計判断：`/` と `**` と 0 除算](#28-設計判断--と--と-0-除算)
- [2.9 テストを書く](#29-テストを書く)
- [2.10 動作確認](#210-動作確認)
- [2.11 踏んだバグ：`**` の検査位置](#211-踏んだバグ-の検査位置)
- [2.12 まとめと次章の予告](#212-まとめと次章の予告)

---

## 2.1 この章で作る文法

```ebnf
program     ::= expr EOF

expr        ::= bitor_expr
bitor_expr  ::= bitxor_expr { "|"  bitxor_expr }
bitxor_expr ::= bitand_expr { "^"  bitand_expr }
bitand_expr ::= shift_expr  { "&"  shift_expr }
shift_expr  ::= add_expr    { ("<<" | ">>") add_expr }
add_expr    ::= mul_expr    { ("+" | "-")   mul_expr }
mul_expr    ::= unary       { ("*" | "/" | "//" | "%") unary }
unary       ::= ("-" | "+" | "~") unary | power
power       ::= primary [ "**" unary ]        ← 第9章で実装
primary     ::= INT | "(" expr ")"
```

さらに整数リテラルの基数を増やします。

| 記法 | 例 | 値 |
|---|---|---|
| 10 進 | `42`, `1_000` | 42, 1000 |
| 16 進 | `0xFF`, `0xff` | 255 |
| 8 進 | `0o77` | 63 |
| 2 進 | `0b1100` | 12 |

**8 個の関数**（`bitor_expr` 〜 `primary`）を書くだけで、
14 種類の演算子の優先順位が正しく処理されます。

---

## 2.2 なぜ関数の階層が優先順位になるのか

**この章で最も重要な考え方です。** 丁寧に追いかけます。

### 📖 ルール

> **優先順位の「弱い」演算子を上（先に呼ばれる関数）に、
> 「強い」演算子を下（後から呼ばれる関数）に置く。**

```
expr        弱い ↑   （後で結合する = 木の浅い位置に来る）
  ↓
add_expr           + -
  ↓
mul_expr           * // %
  ↓
unary              -x ~x
  ↓
primary     強い ↓   （先に結合する = 木の深い位置に来る）
```

### 📖 `1 + 2 * 3` を追いかける

```
add_expr() が呼ばれる
│
├─ まず mul_expr() を呼ぶ（左辺を読むため）
│  │
│  ├─ unary() → power() → primary() → INT(1) を読んだ
│  │
│  └─ 次のトークンは "+"。
│     mul_expr のループ条件は * / // % なので、"+" は該当しない。
│     → ループに入らず、1 をそのまま返す
│
├─ add_expr に戻る。次は "+" なのでループに入る！
│  │
│  └─ 右辺のために mul_expr() を呼ぶ
│     │
│     ├─ unary() → ... → INT(2) を読んだ
│     │
│     └─ 次は "*"。mul_expr のループ条件に該当する！
│        │
│        └─ 右辺のために unary() → ... → INT(3)
│           → BinOp(*, 2, 3) を作る
│
└─ BinOp(+, 1, BinOp(*, 2, 3)) を作る
```

できる木：

```
      +
     / \
    1   *
       / \
      2   3
```

### ✅ 実際に確認する

```bash
./build/mythonc --dump-ast tests/cases/precedence.my
```

```
(binop +
  (int 1)
  (binop *
    (int 2)
    (int 3)
  )
)
```

**`*` が木の深い位置にあります。**
木を評価するときは葉から順に計算されるので、`2 * 3` が先に計算されます。

### 📖 なぜこうなるのか（一文でいうと）

> **`mul_expr` は `*` を見つけたら自分で処理してしまうが、
> `+` を見つけたら「自分の仕事ではない」と判断して呼び出し元に返す。**

だから `+` は必ず `mul_expr` より外側（＝木の浅い位置）で処理されます。
**「どの関数がその演算子を処理する責任を持つか」を階層で分担している**のです。

### 📖 括弧はどう働くのか

```c
static Node *primary(Parser *p) {
    if (consume(p, "(")) {
        Node *n = expr(p);        // ★ 階層の一番上に戻る
        expect_punct(p, ")");
        return n;
    }
    ...
}
```

**`primary`（最強の階層）から `expr`（最弱の階層）を再帰的に呼び戻す**のがポイントです。
括弧の中では優先順位がリセットされ、閉じ括弧に出会うまで自由に式を読めます。

```bash
./build/mythonc --dump-ast tests/cases/parens.my     # (1 + 2) * 3
```

```
(binop *
  (binop +
    (int 1)
    (int 2)
  )
  (int 3)
)
```

**`+` が深い位置に移りました。** 括弧が優先順位を覆したのです。

---

## 2.3 左結合と右結合の書き分け

同じ優先順位の演算子が並んだときの結合方向です。

| 種類 | 例 | 意味 | 書き方 |
|---|---|---|---|
| **左結合** | `100 - 3 - 1` | `(100-3)-1` = 96 | **`while` ループ** |
| **右結合** | `- -5` | `-(-5)` = 5 | **再帰呼び出し** |

### 📖 左結合＝ループ

```c
static Node *add_expr(Parser *p) {
    Node *lhs = mul_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "+"))      lhs = new_binop_node(t, OP_ADD, lhs, mul_expr(p));
        else if (consume(p, "-")) lhs = new_binop_node(t, OP_SUB, lhs, mul_expr(p));
        else return lhs;
    }
}
```

**`lhs` を上書きし続ける**のが左結合の正体です。

```
1 周目: lhs = 100          次に "-" を見つけた → lhs = (100 - 3)
2 周目: lhs = (100 - 3)    次に "-" を見つけた → lhs = ((100 - 3) - 1)
3 周目: 演算子がない        → return
```

### ✅ 確認

```bash
./build/mythonc --dump-ast tests/cases/left_assoc_sub.my    # 100 - 3 - 1
```

```
(binop -
  (binop -
    (int 100)
    (int 3)
  )
  (int 1)
)
```

**左側が深くなっています**（= 先に計算される）。これが左結合です。

**⚠️ もし右結合に間違えると** `100 - (3 - 1)` = 98 になります。
テスト `left_assoc_sub.my` は 96 を期待しているので、間違いに気づけます。

除算でも確認します。`8 // 4 // 2` は `(8//4)//2 = 1` であって `8//(4//2) = 4` ではありません。
テスト `left_assoc_div.my` がこれを守っています。

### 📖 右結合＝再帰

```c
static Node *unary(Parser *p) {
    Token *t = peek(p);
    if (consume(p, "-")) return new_unary_node(t, OP_NEG, unary(p));
    //                                                   ~~~~~~~~
    //                                        ★ 自分自身を再帰で呼ぶ
    if (consume(p, "+")) return new_unary_node(t, OP_POS, unary(p));
    if (consume(p, "~")) return new_unary_node(t, OP_BITNOT, unary(p));
    return power(p);
}
```

**自分と同じ階層を再帰で呼ぶ**のが右結合の正体です。

### ✅ 確認

```bash
./build/mythonc --dump-ast tests/cases/unary_neg.my     # - -5
```

```
(unary -
  (unary -
    (int 5)
  )
)
```

**右側が深くなっています。** `-(-5)` = 5 です。

### 🤔 `**` の右結合（第9章の予習）

`**` も右結合です。`2 ** 3 ** 4` は `2 ** (3 ** 4)` になります。
第9章ではこう書きます。

```c
static Node *power(Parser *p) {
    Node *base = primary(p);
    Token *t = peek(p);
    if (consume(p, "**"))
        return new_binop_node(t, OP_POW, base, unary(p));
    //                                         ~~~~~~~~
    //                          ★ 自分より上（弱い方）の階層を呼び戻す
    return base;
}
```

**`unary` を呼び戻すのがポイントです。** これにより `2 ** -1` のように
指数に単項マイナスが書けるようになり、同時に右結合になります。

### 🤔 `-2 ** 3` は `-8` か `(-2)**3 = -8` か

Python では `-2 ** 3` は `-(2 ** 3)` = -8 です（`**` が単項マイナスより強い）。
今の階層（`unary` → `power` → `primary`）はこれを**正しく実現します**。

```
unary() が "-" を消費 → unary() を再帰 → power() → primary() で 2 を読む
                                         → power() が "**" を見つけて POW(2, 3) を作る
→ NEG(POW(2, 3)) = -8   ✅
```

**優先順位の階層を正しく組むと、こういう細かい意味も自動的に正しくなります。**

---

## 2.4 ① 字句解析器に記号を追加する

### ✍️ `TK_PUNCT` を追加

```c
typedef enum {
    TK_EOF,
    TK_INT,
    TK_PUNCT,  // ← 追加。記号（+ - * // ( ) など）
} TokenKind;
```

### 📖 記号の判定：最長一致が命

```c
// ★ 長い記号を先に並べること。
static const char *PUNCTS[] = {
    // 2 文字
    "//", "**", "<<", ">>",
    // 1 文字
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "(", ")",
    NULL,
};

static int read_punct(Lexer *lx) {
    for (int i = 0; PUNCTS[i]; i++) {
        size_t len = strlen(PUNCTS[i]);
        if (strncmp(lx->p, PUNCTS[i], len) == 0) {
            tv_push(lx, TK_PUNCT, lx->p, (int)len);
            lx->p += len;
            return 1;
        }
    }
    return 0;
}
```

### ⚠️ 最大の落とし穴：`//` を `/` 2 個に読んでしまう

**配列の順序が意味を持ちます。** 上から順に試すので、
もし `"/"` を `"//"` より先に書いてしまうと：

```
"8 // 4"  →  [INT 8] [PUNCT /] [PUNCT /] [INT 4]
```

パーサは `8 / (/4)` を読もうとして「式が必要です」というエラーになります。
**症状から原因（字句解析器の配列の順序）を推測するのは非常に困難です。**

> **記号は必ず長いものから先に並べる。** これを**最長一致の原則**といいます。

第6章で `==` `!=` `<=` `>=` を、第5章で `+=` `//=` を追加するときも同じ原則が効きます。
（`<=` を `<` より後に書くと `<= ` が `<` と `=` に割れます。）

### 📖 記号の比較：文字列を複製しない

```c
bool tok_is(Token *tok, const char *op) {
    size_t len = strlen(op);
    return tok->kind == TK_PUNCT && (size_t)tok->len == len &&
           memcmp(tok->loc, op, len) == 0;
}
```

`Token` に記号の文字列を `xstrndup` で持たせる方法もありますが、
**`loc` と `len` で元のソースを直接比較すれば確保が要りません。**

使い方はこうなります。

```c
if (tok_is(peek(p), "+")) { ... }
```

### 📖 基数つき整数リテラル

```c
int base = 10;
if (lx->p[0] == '0' && lx->p[1] != '\0') {
    char c = lx->p[1];
    if (c == 'x' || c == 'X') base = 16;
    else if (c == 'o' || c == 'O') base = 8;
    else if (c == 'b' || c == 'B') base = 2;
    if (base != 10) lx->p += 2;   // 接頭辞を読み飛ばす
}

// 桁を集める（'_' は飛ばす）
while (is_digit_of(*lx->p, base) || *lx->p == '_') { ... }

// 接頭辞だけで数字がない（0x や 0b）を弾く
if (n == 0) error_at(&tmp, "数字がありません（基数 %d のリテラル）", base);

// strtoll に base を渡すだけで変換できる
long long v = strtoll(digits, &end, base);
```

**`strtoll` が基数を引数で受け取るので、変換部分は 1 行も変わりません。**
自分で `16 進 → 10 進` の計算を書く必要はありません。

### 📖 リファクタリング：`span_token`

第1章ではエラーごとに一時 `Token` を手で組み立てていて、同じコードが 4 か所ありました。

```c
// Before（4 か所に重複）
Token tmp = {0};
tmp.file = lx->file;
tmp.line_start = lx->line_start;
tmp.line = lx->line;
tmp.col = (int)(start - lx->line_start) + 1;
tmp.len = (int)(lx->p - start);
error_at(&tmp, "...");
```

これを関数にまとめました。

```c
// After
static Token span_token(Lexer *lx, const char *start, const char *end) {
    Token t = {0};
    t.file = lx->file;
    t.line_start = lx->line_start;
    t.line = lx->line;
    t.col = (int)(start - lx->line_start) + 1;
    t.len = (int)(end - start);
    if (t.len < 1) t.len = 1;
    return t;
}

// 使う側
Token tmp = span_token(lx, start, lx->p);
error_at(&tmp, "整数リテラルが int の範囲 (64bit) を超えています");
```

**🤔 なぜ今リファクタするのか**：この章でエラー箇所が 6 個に増えます。
重複が 4 個までは我慢できますが、6 個になると必ず 1 個だけ直し忘れます。
**「3 回目の重複でまとめる」**のが良い目安です。

---

## 2.5 AST に二項・単項ノードを追加する

### ✍️ ノード種別と演算子の enum

```c
typedef enum {
    ND_INT,    // 整数リテラル → ival
    ND_BINOP,  // 二項演算   → op, lhs, rhs
    ND_UNARY,  // 単項演算   → op, lhs
} NodeKind;

typedef enum {
    // 二項
    OP_ADD, OP_SUB, OP_MUL,
    OP_TRUEDIV,   // /   ← int には使えない。仕様 4.2
    OP_FLOORDIV,  // //
    OP_MOD, OP_BITAND, OP_BITOR, OP_BITXOR, OP_SHL, OP_SHR,
    // 単項
    OP_NEG, OP_POS, OP_BITNOT,
} OpKind;
```

### 🤔 なぜトークンの文字列ではなく enum で持つのか

`Node` に `char *op = "+"` を持たせて、コード生成で `strcmp` する方法もあります。
しかし enum にすると：

1. **`switch` の網羅性をコンパイラが検査してくれる**
   新しい演算子を追加したとき、`switch` に `case` を足し忘れると
   `-Wall` が警告してくれます（`default: UNREACHABLE()` があるので実行時にも捕まる）。
2. **`strcmp` が不要**で速い
3. **`OP_NEG` と `OP_SUB` を区別できる** — どちらもソースでは `-` ですが、
   意味は全く違います。文字列で持つと区別できません。

### 📖 単項演算は `lhs` だけを使う

```c
Node *new_unary_node(Token *tok, OpKind op, Node *operand) {
    Node *n = new_node(ND_UNARY, tok);
    n->op = op;
    n->lhs = operand;   // ★ 単項演算は lhs だけを使う（rhs は NULL のまま）
    return n;
}
```

`rhs` は `xmalloc`（`calloc`）のおかげで自動的に `NULL` です。
**「使わないフィールドは 0/NULL」という保証が、第1章の設計判断から効いています。**

### 📖 S 式ダンプを木構造に拡張

```c
case ND_BINOP:
    printf("(binop %s\n", op_symbol(n->op));
    dump(n->lhs, depth + 1);
    dump(n->rhs, depth + 1);
    for (int i = 0; i < depth; i++) printf("  ");
    printf(")\n");
    break;
```

インデント付きで再帰的に出力します。
**これが優先順位・結合性のデバッグに決定的に役立ちます。**
「なぜ答えが違うのか」は、ほぼ必ず木の形を見れば分かります。

---

## 2.6 ② 構文解析器：優先順位の階層を作る

### ✍️ `consume` を追加

```c
// 現在のトークンが指定した記号なら消費して返す。違えば NULL。
static Token *consume(Parser *p, const char *op) {
    if (tok_is(peek(p), op)) return advance(p);
    return NULL;
}

// 指定した記号なら消費、違えばエラー。
static Token *expect_punct(Parser *p, const char *op) {
    Token *t = peek(p);
    if (!tok_is(t, op)) error_at(t, "'%s' が必要です", op);
    return advance(p);
}
```

この 2 つと第1章の `peek` / `advance` の**合計 4 つの部品だけ**で、
8 個の文法関数すべてが書けます。

### ✍️ 階層を実装する

6 つの二項演算の関数は**すべて同じ形**です。

```c
static Node *bitor_expr(Parser *p) {
    Node *lhs = bitxor_expr(p);              // ← 1 つ下の階層を呼ぶ
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "|")) lhs = new_binop_node(t, OP_BITOR, lhs, bitxor_expr(p));
        else return lhs;
    }
}
```

**この定型を 6 回書くだけです。** 違うのは
「呼ぶ下位関数」と「扱う演算子」だけです。

```
bitor_expr  → bitxor_expr    "|"
bitxor_expr → bitand_expr    "^"
bitand_expr → shift_expr     "&"
shift_expr  → add_expr       "<<" ">>"
add_expr    → mul_expr       "+" "-"
mul_expr    → unary          "*" "/" "//" "%"
```

**⚠️ 退屈だと感じても、マクロや関数テーブルで抽象化しないことを推奨します。**
素直に 6 回書いたほうが読めます。優先順位を変えたいときも、
呼び出し先を 1 か所書き換えるだけで済みます。

### 📖 演算子優先順位パーサ（別の解法）

同じことを 1 つの関数でやる **Precedence Climbing** という技法もあります。

```c
// 参考：この教材では採用しない書き方
static Node *parse_binary(Parser *p, int min_prec) {
    Node *lhs = unary(p);
    while (precedence(peek(p)) >= min_prec) { ... }
    return lhs;
}
```

**🤔 なぜ採用しないのか**：短く書けますが、
「文法定義とコードの 1:1 対応」が失われます。
この教材では**文法規則 1 つ ＝ 関数 1 つ**を守ることを優先します。
[../spec/grammar.md](../spec/grammar.md) を見ながらコードを追えることのほうが、
行数の節約より価値があります。

---

## 2.7 ④ コード生成：演算子を LLVM 命令に対応させる

### ✍️ 演算子 → 命令の対応表

```c
static const char *llvm_binop(Node *n) {
    switch (n->op) {
        case OP_ADD: return "add";
        case OP_SUB: return "sub";
        case OP_MUL: return "mul";
        case OP_FLOORDIV: return "sdiv";  // 符号付き除算
        case OP_MOD: return "srem";       // 符号付き剰余
        case OP_BITAND: return "and";
        case OP_BITOR: return "or";
        case OP_BITXOR: return "xor";
        case OP_SHL: return "shl";
        case OP_SHR: return "ashr";        // 算術シフト（符号を保つ）
        ...
    }
}
```

### ⚠️ 符号の落とし穴：必ず `s` の付く命令を使う

**`int` は符号付き**なので、符号付き用の命令を選ばなければなりません。

| 使う | 使わない | 間違えると |
|---|---|---|
| `sdiv` | `udiv` | `-7 // 2` が巨大な正の数になる |
| `srem` | `urem` | 負数の剰余が壊れる |
| `ashr` | `lshr` | `-4 >> 1` が巨大な正の数になる |

LLVM の整数型（`i64`）**自身は符号の情報を持ちません**。
符号を決めるのは**命令**です。これが LLVM IR の設計で最初に戸惑う点です。

テスト `neg_floordiv.my` と `shift_arith.my` がこれを守っています。

### 📖 二項演算の生成

```c
case ND_BINOP: {
    if ((n->op == OP_FLOORDIV || n->op == OP_MOD) &&
        n->rhs->kind == ND_INT && n->rhs->ival == 0)
        error_at(n->rhs->tok, "0 で除算しています");

    // ★ 左辺 → 右辺の順に生成する（仕様 4.5：評価順は左から右）
    const char *inst = llvm_binop(n);
    char *l = gen_expr(e, n->lhs);
    char *r = gen_expr(e, n->rhs);
    char *t = new_tmp(e);
    sb_printf(&e->body, "  %s = %s i64 %s, %s\n", t, inst, l, r);
    return t;
}
```

**`gen_expr` は「値の置き場所の名前」を返す**という第1章の約束が効いています。
即値（`"3"`）でもレジスタ（`"%t0"`）でも、同じ `char *` として扱えるので
場合分けが不要です。

### ⚠️ 単項マイナス：LLVM に整数の `neg` 命令はない

```c
case ND_UNARY: {
    char *v = gen_expr(e, n->lhs);

    if (n->op == OP_POS) return v;   // +x は何もしない

    char *t = new_tmp(e);
    if (n->op == OP_NEG) {
        // ⚠️ 整数の neg 命令は存在しない。0 からの減算で表現する。
        sb_printf(&e->body, "  %s = sub i64 0, %s\n", t, v);
    } else if (n->op == OP_BITNOT) {
        // ~x は全ビット反転 = x XOR -1
        sb_printf(&e->body, "  %s = xor i64 %s, -1\n", t, v);
    }
    return t;
}
```

**`fneg`（浮動小数用）は存在しますが、整数用の `neg` はありません。**
`sub i64 0, %x` と書きます。

`~x` も専用命令がないので `x XOR -1` で表現します
（`-1` は全ビットが 1 なので、XOR すると全ビット反転）。

**`+x` は命令を 1 つも出しません。** 値をそのまま返すだけです。
「何もしない演算子」を素直に何もしないよう実装できるのは、
`gen_expr` が「値の名前を返す」設計だからです。

---

## 2.8 設計判断：`/` と `**` と 0 除算

この章では 3 つの判断をしました。**すべて仕様書と整合させています。**

### ① `/` を整数に使うとコンパイルエラー

仕様 4.2 で `int / int` は**エラー**（`//` を使う）と決めています。
Python では float を返しますが、Mython には暗黙の型変換がないためです。

```c
case OP_TRUEDIV:
    error_at(n->tok,
             "整数の除算に '/' は使えません。'//' を使ってください"
             "（'/' は float 専用です）");
```

**⚠️ この検査は本来「型検査」の仕事です。**
第5章で `sema.c` を作ったら、この検査はそちらへ移します。
今はまだ型検査パスが無いので、暫定的にコード生成で弾いています。
**この種の「暫定的な置き場所」はコメントに明記しておくのが重要です。**

### ② `**` は第9章に延期

**仕様書を実装に合わせて変更しました。**

`int ** int` を正しく実装するには、**負の指数を実行時エラーにする**必要があります。
しかしエラーを報告する仕組み（`panic`）は第9章のランタイムで作ります。
中途半端な意味（負の指数で 0 を返すなど）を今決めてしまうと、
後で互換性を壊す変更が必要になります。

そこで `**` は**構文としては認識するがエラーにする**という扱いにしました。

```
error: 演算子 '**' はまだ未対応です（第9章で実装予定）
  --> tests/cases/err_pow_unsupported.my:2:3
  |
2 | 2 ** 10
  |   ^^
```

**🤔 なぜ「未知の記号」エラーで済ませないのか**：
`2 ** 10` と書いた人が知りたいのは「`**` が今は使えない」ことです。
「解釈できない文字 `*`」と言われても原因が分かりません。
**未実装は、未実装だと明示するのが親切です。**

右結合の学習は単項演算子（`- -5`）で行えるので、
この章の教育的な目標は損なわれていません。

### ③ 0 除算：リテラルのみコンパイル時に検出

```c
if ((n->op == OP_FLOORDIV || n->op == OP_MOD) &&
    n->rhs->kind == ND_INT && n->rhs->ival == 0)
    error_at(n->rhs->tok, "0 で除算しています");
```

```
error: 0 で除算しています
  --> tests/cases/err_div_zero.my:2:6
  |
2 | 1 // 0
  |      ^
```

**⚠️ 既知の制限を正直に書きます。**

| 式 | 今の挙動 |
|---|---|
| `1 // 0` | ✅ コンパイルエラー |
| `1 // (2 - 2)` | ❌ **実行時に SIGFPE でクラッシュする** |

右辺が式の場合は検出できません。実行時チェックには
**分岐（第7章）とエラー報告のランタイム（第9章）**が必要です。

**🤔 なぜ今むりに直さないのか**：
実行時チェックを入れるには基本ブロックの分割が必要で、それは第7章の主題です。
ここで前借りすると、第7章で学ぶべきことが薄まります。
**「既知の制限」として記録し、担当する章で解決する**のが正しい進め方です。

---

## 2.9 テストを書く

**23 件追加**し、合計 34 件になりました。

### 正常系：優先順位と結合性を「間違いを検出できる形」で書く

**★ テストの値は「間違えたら別の値になる」ように選びます。**

| テスト | 式 | 期待 | 間違えると |
|---|---|---|---|
| `precedence.my` | `1 + 2 * 3` | 7 | 左から計算すると 9 |
| `parens.my` | `(1 + 2) * 3` | 9 | 括弧を無視すると 7 |
| `left_assoc_sub.my` | `100 - 3 - 1` | 96 | **右結合なら 98** |
| `left_assoc_div.my` | `8 // 4 // 2` | 1 | **右結合なら 4** |
| `bitwise_precedence.my` | `1 \| 2 & 1` | 1 | `&` を弱くすると 3 |
| `shift.my` | `1 + 4 << 3` | 40 | `<<` を強くすると 33 |

**⚠️ `2 + 2` のようなテストは価値が低い**です。
結合性を間違えても同じ答えになってしまうからです。
**「バグがあれば必ず落ちる」テストを選ぶ**のが設計技術です。

### 符号の扱いを守るテスト

```python
# tests/cases/neg_floordiv.my
# EXIT: 253
# ⚠️ Python 非互換: -7 // 2 は Python では -4、Mython では -3（sdiv の切り捨て）
# 終了コードは下位8bitなので -3 は 253 になる
-7 // 2
```

```python
# tests/cases/shift_arith.my
# EXIT: 254
# 算術シフト(ashr)なので符号が保たれる: -4 >> 1 = -2 → 下位8bitで 254
-4 >> 1
```

**この 2 件が `udiv` / `lshr` の誤用を防ぎます。**

**⚠️ 負の値のテストは終了コードで書きにくい**（`-3` は `253` になる）ので、
コメントで理由を明記しています。第7章で `print` が使えるようになれば
`# OUTPUT: -3` と直接書けるようになります。

### エラー系（6 件）

```
err_truediv.my         7 / 2         → '//' を使ってください
err_div_zero.my        1 // 0        → 0 で除算しています
err_unclosed_paren.my  (1 + 2        → ')' が必要です
err_no_expr.my         1 + * 2       → 式が必要です
err_pow_unsupported.my 2 ** 10       → '**' はまだ未対応です
err_empty_hex.my       0x            → 数字がありません
```

---

## 2.10 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
────────────────────────────────
全 34 件パス
```

警告 0 件でビルドでき、ASan/UBSan でも全ケースがクリーンです。

### ✅ ① トークン列

```bash
./build/mythonc --dump-tokens tests/cases/precedence.my
```

```
  0  INT       3:1    1
  1  PUNCT     3:3    +
  2  INT       3:5    2
  3  PUNCT     3:7    *
  4  INT       3:9    3
  5  EOF       4:1    
```

### ✅ ④ 生成された IR

```bash
./build/mythonc -S tests/cases/precedence.my
```

```llvm
; Generated by mythonc
source_filename = "tests/cases/precedence.my"
target triple = "x86_64-apple-macosx26.0.0"

define i64 @mython_main() {
entry:
  %t0 = mul i64 2, 3
  %t1 = add i64 1, %t0
  ret i64 %t1
}

define i32 @main() {
entry:
  %t0 = call i64 @mython_main()
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
```

**`mul` が `add` より先に出ています。**
AST の深い位置から先に生成されるので、自然に正しい順序になります。

`1 + 2 * 3` の `1` と `2` と `3` が**即値としてそのまま命令に埋まっている**ことにも注目してください。
`ND_INT` が命令を出さずに文字列を返す設計の効果です。

### ✅ もう少し複雑な式

```bash
./build/mythonc -S tests/cases/complex_expr.my | sed -n '/mython_main/,/^}/p'
```

`2 * 3 + (10 - 4) * 6` の生成結果：

```llvm
define i64 @mython_main() {
entry:
  %t0 = mul i64 2, 3
  %t1 = sub i64 10, 4
  %t2 = mul i64 %t1, 6
  %t3 = add i64 %t0, %t2
  ret i64 %t3
}
```

**一時レジスタが `%t0` から順に振られ、衝突していません。**
最後の `add` で 2 つの部分結果を合わせています。

### ✅ LLVM の最適化に見せてみる

```bash
./build/mythonc -S tests/cases/complex_expr.my -o /tmp/c.ll
$(brew --prefix llvm)/bin/opt -O2 -S /tmp/c.ll | sed -n '/@mython_main/,/^}/p'
```

```llvm
define noundef i64 @mython_main() local_unnamed_addr #0 {
entry:
  ret i64 42
}
```

**5 命令が 1 命令になりました。** LLVM が全部コンパイル時に計算したのです。

**これが「素朴に生成して LLVM に任せる」戦略の実例です。**
私たちは定数畳み込みを 1 行も書いていません。

### ✅ エラーメッセージ

```bash
./build/mythonc tests/cases/err_no_expr.my -o /tmp/x
```

```
error: 式が必要です
  --> tests/cases/err_no_expr.my:2:5
  |
2 | 1 + * 2
  |     ^
```

**`*` の位置を正確に指しています。**
第1章で `Token` に位置情報を持たせた投資が回収されています。

### ⚠️ まだ不十分なエラー：閉じ括弧忘れ

```bash
./build/mythonc tests/cases/err_unclosed_paren.my -o /tmp/x
```

```
error: ')' が必要です
  --> tests/cases/err_unclosed_paren.my:3:1
  |
3 | 
  | ^
```

**メッセージは正しいが、指している位置が空行です。**
EOF トークンの位置を指しているためで、人間には役に立ちません。

理想はこうです。

```
error: 閉じ括弧 ')' がありません
  --> err_unclosed_paren.my:2:1
  |
2 | (1 + 2
  | ^ この '(' に対応する ')' が見つかりません
```

**開き括弧の位置を覚えておいて、そちらを指す**必要があります。
これは**第3章（エラー報告と診断メッセージ）の主題**なので、そこで直します。
既知の課題として記録しておきます。

---

## 2.11 踏んだバグ：`**` の検査位置

**実際に踏んだので記録します。** テストが救ってくれた例です。

### 症状

`**` の未対応エラーを `unary()` の入口に置きました。

```c
static Node *unary(Parser *p) {
    Token *t = peek(p);
    if (consume(p, "-")) return new_unary_node(t, OP_NEG, unary(p));
    ...
    if (tok_is(t, "**"))                      // ← ここに置いた
        error_at(t, "演算子 '**' はまだ未対応です");
    return primary(p);
}
```

テスト `err_pow_unsupported.my` が落ちました。

```
FAIL  err_pow_unsupported.my
      期待: 演算子 '**' はまだ未対応です
      実際: error: 式の後に余分なトークンがあります
        --> ...:2:3
        |
      2 | 2 ** 10
        |   ^^
```

### 原因

**`unary()` は「オペランドの先頭」でしか呼ばれません。**

`2 ** 10` を解析する流れ：

```
mul_expr → unary  ← ここで "2" を見る。"**" ではないので素通り
                     → primary が 2 を読んで返る
         → mul_expr のループ条件（* / // %）に "**" は該当しない → 返る
add_expr → ... 誰も "**" を処理しない
program  → 「EOF のはずなのに "**" がある」→ 余分なトークンエラー
```

**`**` は「基数を読み終えた後」に現れる後置的な演算子**なので、
オペランドの先頭を見る `unary()` の入口では絶対に出会えません。

### 修正

文法どおり `power` の位置に置きました。

```c
// power ::= primary [ "**" unary ]
static Node *power(Parser *p) {
    Node *base = primary(p);          // ← 先に基数を読む

    Token *t = peek(p);
    if (tok_is(t, "**"))              // ← 読み終えた後に確認する
        error_at(t, "演算子 '**' はまだ未対応です（第9章で実装予定）");

    return base;
}

static Node *unary(Parser *p) {
    ...
    return power(p);   // primary ではなく power を呼ぶ
}
```

### 📖 学べること

1. **文法定義に従って関数を配置すれば、この間違いは起きなかった。**
   [../spec/grammar.md](../spec/grammar.md) には最初から
   `power ::= postfix [ "**" unary ]` と書いてありました。
   「まだ実装しないから関数を作らなくていい」と判断したのが誤りでした。

2. **エラーメッセージのテストが設計ミスを捕まえた。**
   もし `**` のテストを書いていなければ、
   「`2 ** 10` が変なエラーを出す」ことに気づかないまま第9章まで進んでいました。

3. **前置演算子と後置演算子は、扱う場所が違う。**
   - 前置（`-x` `~x` `not x`）→ オペランドの**前**で判定 → `unary()` の入口
   - 後置（`**` `f(x)` `a[i]` `a.b`）→ オペランドを**読んだ後**で判定 → `power` / `postfix`

   第10章（`a[i]`）と第12章（`a.b`）で `postfix` を書くときに、この区別が再び効きます。

---

## 2.12 まとめと次章の予告

### できたこと

```
✅ 14 種類の演算子（+ - * // % & | ^ << >> 単項- 単項+ ~ 括弧）
✅ 優先順位を関数階層で表現（8 個の文法関数）
✅ 左結合（ループ）と右結合（再帰）の書き分け
✅ 16/8/2 進整数リテラル
✅ 符号付き命令の正しい選択（sdiv / srem / ashr）
✅ 仕様準拠のエラー（/ の禁止、0 除算、** 未対応）
✅ テスト 34 件（正常系 23 + エラー系 11）全パス
✅ 警告 0 件、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/lexer.h/c` | `TK_PUNCT`、`tok_is`、`read_punct`、基数対応、`span_token` |
| `src/ast.h/c` | `ND_BINOP` / `ND_UNARY`、`OpKind`、`op_symbol`、木のダンプ |
| `src/parser.c` | `consume` / `expect_punct`、8 個の文法関数 |
| `src/codegen.c` | `llvm_binop`、`ND_BINOP` / `ND_UNARY` の生成 |
| `docs/spec/*.md` | `**` の担当章を ch2 → ch9 に変更 |
| `tests/cases/` | 23 件追加 |

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 閉じ括弧忘れのエラー位置が EOF を指す | 第3章 |
| `/` の検査がコード生成にある（型検査の仕事） | 第5章 |
| `1 // (2 - 2)` が実行時に SIGFPE | 第9章 |
| `**` 未実装 | 第9章 |
| `-9223372036854775808` が「範囲外」エラーになる（単項マイナスは別トークンなので、リテラル単体では i64 の範囲を超える） | 第9章（要検討） |

### ✍️ commit する

```bash
git add -A
git commit -m "第2章: 四則演算と演算子の優先順位"
```

---

## 次章：第3章 エラー報告と診断メッセージ

**達成目標**

エラーの「見せ方」を専門化し、開発を支える診断基盤を作ります。

```
error: 閉じ括弧 ')' がありません
  --> tests/cases/err_unclosed_paren.my:2:1
   |
 2 | (1 + 2
   | ^ この '(' に対応する ')' が見つかりません
   |
   = ヒント: 括弧の対応を確認してください
```

**やること**

| ファイル | 作業 |
|---|---|
| `src/diag.h/c`（新規） | `util.c` から診断機能を独立させる |
| — | 行番号の桁揃え、複数行にまたがる下線 |
| — | 「関連する位置」の表示（開き括弧の位置など） |
| — | `= ヒント:` 行の追加 |
| `parser.c` | 開き括弧のトークンを覚えて `expect_punct` に渡す |
| `tests/` | エラーメッセージの整形テスト |

**学ぶ中心概念**

- 診断の 3 要素（どこで / 何が問題か / どうすればよいか）
- 「主要な位置」と「関連する位置」を分けて示す設計
- なぜエラー回復（複数エラーの報告）を v1 でやらないのか

**🤔 なぜ第3章をエラー報告に使うのか**

機能追加を止めて 1 章まるごと使うのは、遠回りに見えます。
しかし**ここから先、コンパイラのユーザーは自分自身です。**
第4章のインデント処理、第5章の型検査で、あなたは何十回もエラーを見ます。
その表示が親切かどうかが、**残り 17 章の開発速度を決めます。**

### 🤔 第3章に入る前の練習問題

1. **`**` を仮に「左結合」で実装したら何が起きるか**、紙の上で考える
   `2 ** 3 ** 4` の木がどう変わるか。答え合わせは第9章で。
2. **`PUNCTS` 配列の `"//"` を `"/"` の後ろに移動**してビルドし、
   `8 // 4` がどんなエラーになるか確認する（**元に戻すのを忘れずに**）
3. **`ashr` を `lshr` に変えて** `make test` を走らせ、
   どのテストが落ちるか確認する（`shift_arith.my` が守っているはず）
4. **`0o8` を書いたらどうなるか**試す。期待どおりのエラーになるか？
