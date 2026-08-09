# 第4章 インデント構文（NEWLINE / INDENT / DEDENT）

> **この章のゴール**
> 複数行のプログラムを読み、インデントからブロック構造を検出する。
>
> ```bash
> $ cat t.my
> 1 + 2
> 3 * 4
> $ ./build/mythonc t.my -o t && ./t; echo $?
> 12
> ```

**⚠️ Python 風言語を作るうえで最大の山場です。**
ただし、これから見るように「難しいのは字句解析器だけ」で、
構文解析器はまったく難しくなりません。**そこがこの技法の核心です。**

---

## 目次

- [4.1 問題：インデントでブロックを表すとは](#41-問題インデントでブロックを表すとは)
- [4.2 解決：仮想トークンを合成する](#42-解決仮想トークンを合成する)
- [4.3 この章の範囲（正直な話）](#43-この章の範囲正直な話)
- [4.4 実装：インデントスタック](#44-実装インデントスタック)
- [4.5 空行とコメント行を無視する](#45-空行とコメント行を無視する)
- [4.6 括弧の中では改行を無視する](#46-括弧の中では改行を無視する)
- [4.7 ファイル末尾の DEDENT](#47-ファイル末尾の-dedent)
- [4.8 メインループの再構成](#48-メインループの再構成)
- [4.9 構文解析器：複数文と ND_BLOCK](#49-構文解析器複数文と-nd_block)
- [4.10 テストランナーに `# TOKENS:` を追加](#410-テストランナーに-tokens-を追加)
- [4.11 動作確認](#411-動作確認)
- [4.12 まとめと次章の予告](#412-まとめと次章の予告)

---

## 4.1 問題：インデントでブロックを表すとは

C 系の言語はブロックを**記号**で表します。

```c
if (a) {        // ← '{' がブロックの開始
    x();
}               // ← '}' がブロックの終了
```

構文解析器から見ると簡単です。`{` というトークンが来たらブロック開始、
`}` が来たら終了。**トークンを見るだけで分かります。**

Python と Mython は**字下げ**で表します。

```python
if a:
    x()         # ← 4 個の空白でブロックの中だと分かる
y()             # ← 空白が減ったのでブロックが終わった
```

この方式を**オフサイドルール (off-side rule)** といいます。

### ⚠️ 素朴に実装すると構文解析器が地獄になる

「構文解析器がインデント幅を見ればいい」と考えると、こうなります。

```c
// ✗ こんな設計にすると破綻する
static Node *block(Parser *p, int parent_indent) {
    while (current_token_indent(p) > parent_indent) {
        ...
    }
}
```

すべての文法関数がインデント幅を引数で持ち回る必要があり、
「今どのブロックにいるか」を全員が気にすることになります。
**式の解析関数（`add_expr` など）までインデントを意識し始めたら設計が壊れています。**

---

## 4.2 解決：仮想トークンを合成する

### ★ 発想の転換

> **字句解析器が、ソース上に文字がないトークンを「合成」する。**

インデントが深くなったら `INDENT` トークンを、浅くなったら `DEDENT` トークンを
**字句解析器が勝手に作って**トークン列に混ぜます。

```python
1
    2
```

これを、こうトークン化します。

```
INT(1) NEWLINE INDENT INT(2) NEWLINE DEDENT EOF
              ~~~~~~              ~~~~~~
              ソース上に対応する文字がない！
```

### 📖 なぜこれで解決するのか

構文解析器から見ると、`INDENT` / `DEDENT` は `{` / `}` と**まったく同じ**です。

```
波括弧言語:   LBRACE  文 文 文  RBRACE
Mython:      INDENT  文 文 文  DEDENT
```

> **インデント言語の構文解析が、波括弧言語とまったく同じ難しさに落ちます。**

構文解析器はインデント幅を 1 度も見ません。
**難しさは全部、字句解析器の中に閉じ込められます。**
そして字句解析器の側では、これは「スタックを 1 本持つだけ」の問題になります。

これがこの章で学ぶ最も価値のある考え方です。

### 📖 3 つの仮想トークン

| トークン | 意味 | いつ出るか |
|---|---|---|
| `NEWLINE` | **論理行**の終わり | 括弧の外で改行に出会ったとき |
| `INDENT` | ブロックの開始 | 行のインデントが深くなったとき |
| `DEDENT` | ブロックの終了 | 行のインデントが浅くなったとき（**複数個出ることがある**） |

```c
typedef enum {
    TK_EOF, TK_INT, TK_PUNCT,

    // ── 仮想トークン（ソース上に対応する文字がない）──
    TK_NEWLINE,
    TK_INDENT,
    TK_DEDENT,
} TokenKind;
```

**⚠️ 3 つとも `len == 0` です。** 位置情報だけを持ち、実体がありません。
第3章で `if (under < 1) under = 1;`（下線を最低 1 本引く）という保険を
入れておいたのが、ここで効きます。

---

## 4.3 この章の範囲（正直な話）

### ⚠️ この章では INDENT / DEDENT を「消費する」構文がまだありません

`INDENT` を必要とするのは `if` / `while` / `def` のブロックですが、
それらは第7章・第8章です。**この章で作るのは「生成する側」だけです。**

すると疑問が出ます。**使う人がいないのに、どうやって正しさを確認するのか？**

### ★ 答え：`--dump-tokens` で検証する

```bash
$ ./build/mythonc --dump-tokens t.my
  0  INT       4:1    1
  1  NEWLINE   4:2
  2  INDENT    5:5
  3  INT       5:5    2
  ...
```

**第1章で `--dump-tokens` を作っておいた投資が、ここで回収されます。**

字句解析器は「入力文字列 → トークン列」の純粋な変換なので、
**構文解析器と完全に独立してテストできます。**
この章ではテストランナーに `# TOKENS:` という検証方法を追加します（4.10 節）。

### 📖 この章の言語仕様（暫定）

```ebnf
program ::= { stmt } EOF
stmt    ::= expr NEWLINE
```

トップレベルは**式文の並び**で、プログラムの値は**最後の式の値**です。

```python
1 + 2      # 評価されるが値は使われない
3 * 4      # ← この値 12 がプログラムの結果
```

**⚠️ これは第1章と同じ「暫定の足場」です。**
第8章で `def main() -> int:` が正式な入口になったら置き換えます。
仕様書（[../spec/language-spec.md](../spec/language-spec.md) 6.3）では
「トップレベルに実行文は書けない」と決めているので、これは仕様違反の暫定形です。

そして**インデントされた行はすべてエラー**になります。

```
error: 予期しないインデントです
   = ヒント: ブロックを作る構文（if / while / def）は第7章以降で実装します
```

**これも正しい振る舞いです。** Python も、ブロックを開かずに字下げすれば
`IndentationError: unexpected indent` を出します。

---

## 4.4 実装：インデントスタック

### 📖 データ構造：スタック 1 本

```c
#define MAX_INDENT_DEPTH 64

typedef struct {
    ...
    // インデント幅のスタック。常に先頭は 0（トップレベル）。
    //   例: 0 → 4 → 8 とネストしている状態なら {0, 4, 8}
    int indent_stack[MAX_INDENT_DEPTH];
    int indent_len;   // 積まれている段数（最低 1）

    int paren_depth;  // 括弧の深さ（4.6 節）
} Lexer;
```

初期化で**底に 0 を積んでおく**のが重要です。

```c
lx.indent_stack[0] = 0;
lx.indent_len = 1;
```

**🤔 なぜ底に 0 を置くのか**：トップレベルのインデント幅が 0 だからです。
これを置いておけば「スタックが空」という特殊ケースが消え、
`indent_stack[indent_len - 1]` が常に有効になります。
**特殊ケースを消すためにダミー要素を置く**のは頻出のテクニックです。

### 📖 アルゴリズム

行頭でインデント幅 `width` を測り、スタックの先頭 `top` と比べます。

| 比較 | 動作 |
|---|---|
| `width > top` | スタックに push、`INDENT` を **1 個** 出す |
| `width == top` | 何もしない（同じブロックの続き） |
| `width < top` | `top > width` の間 pop し、pop ごとに `DEDENT` を出す |

```c
static void emit_indent_tokens(Lexer *lx, int width) {
    int top = lx->indent_stack[lx->indent_len - 1];

    if (width > top) {
        if (lx->indent_len >= MAX_INDENT_DEPTH) { /* エラー */ }
        lx->indent_stack[lx->indent_len++] = width;
        tv_push(lx, TK_INDENT, lx->p, 0);
        return;
    }

    if (width < top) {
        while (lx->indent_len > 1 && lx->indent_stack[lx->indent_len - 1] > width) {
            lx->indent_len--;
            tv_push(lx, TK_DEDENT, lx->p, 0);
        }
        if (lx->indent_stack[lx->indent_len - 1] != width) { /* エラー */ }
    }
    // width == top なら何も出さない
}
```

### ⚠️ 落とし穴 1：INDENT は「1 個」

```python
1
        2      # 8 個の空白
```

**`INDENT` は 8 個でも 4 個でもなく 1 個です。**
「インデント 1 段 = `INDENT` 1 個」であり、**幅は関係ありません**。

言語仕様 2.4 で「同じブロック内で一貫していればよい」と決めているので、
幅が 2 でも 4 でも 8 でも動きます。スタックに積むのは幅ですが、
**トークンは段数に対応します。**

### ⚠️ 落とし穴 2：DEDENT は一度に複数個出る

**これがこの章で一番忘れやすい点です。**

```python
1              # indent 0
    2          # indent 4  → INDENT
        3      # indent 8  → INDENT
4              # indent 0  → DEDENT, DEDENT  ← 2 個！
```

深いネストから一気にトップレベルへ戻るときは、
**戻った段数ぶんの `DEDENT`** が必要です。
`while` ループで pop し続けるのがその実装です。

これを `if` で 1 個しか出さないように書くと、
第7章で `if` の入れ子が壊れます（ブロックが閉じられない）。

### ⚠️ 落とし穴 3：揃わないインデント

```python
1
        2      # indent 8
    3          # indent 4 ← 8 でも 0 でもない
```

`8 > 4` なので pop します。すると `top == 0` になりますが、`width == 4` です。
**スタックに 4 が存在しない**ので、どのブロックにも対応しません。エラーです。

```c
if (lx->indent_stack[lx->indent_len - 1] != width) {
    Token tmp = span_token(lx, lx->line_start, lx->p);
    error_at_hint(&tmp,
                  "外側のブロックのインデント幅と正確に一致させてください",
                  "インデントが揃っていません（どのブロックにも対応しません）");
}
```

**下線を「行頭から現在位置まで」に引く**ことで、
問題のインデント部分そのものを示せます。

```
error: インデントが揃っていません（どのブロックにも対応しません）
  --> tests/cases/err_indent_misaligned.my:5:1
   |
 5 |     3
   | ^^^^
   |
   = ヒント: 外側のブロックのインデント幅と正確に一致させてください
```

### ⚠️ 落とし穴 4：深さの上限

```c
if (lx->indent_len >= MAX_INDENT_DEPTH) {
    error_at(&tmp, "インデントが深すぎます（最大 %d 段）", MAX_INDENT_DEPTH - 1);
}
```

**固定長配列を使うなら、上限チェックは必須です。**
これを忘れるとバッファオーバーフローになります。
64 段もインデントするコードは存在しないので、上限を設けて明確なエラーにするのが正解です。

**🤔 なぜ動的配列にしないのか**：実用上 64 段で十分すぎるからです。
「無限に対応する」ために複雑さを増やす価値がありません。
ただし**上限を超えたときに安全に失敗すること**は必須です。

---

## 4.5 空行とコメント行を無視する

### 📖 問題

```python
1
    2

    3      # 空行のあとも同じブロックのはず
```

空行のインデント幅は 0 です。素朴に処理すると、
空行で `DEDENT` が出てブロックが終わってしまいます。

コメント行も同じです。

```python
1
    2
# 行頭のコメント（インデント 0）
    3
```

### ✍️ 解決：内容のある行が見つかるまで読み飛ばす

```c
// 行頭にいる状態で呼ぶ。
// 空行・コメントだけの行を読み飛ばし、実質的な内容がある行の
// インデント幅を返す。入力が終わったら -1 を返す。
static int scan_indent(Lexer *lx) {
    for (;;) {
        int width = 0;
        while (*lx->p == ' ') { width++; lx->p++; }

        if (*lx->p == '\t') { /* タブは字句エラー */ }

        if (*lx->p == '\n') {        // 空行 → インデントに影響させない
            advance_newline(lx);
            continue;
        }
        if (*lx->p == '#') {         // コメントだけの行 → 同じく影響させない
            while (*lx->p && *lx->p != '\n') lx->p++;
            if (*lx->p == '\n') { advance_newline(lx); continue; }
            return -1;               // ファイル末尾
        }
        if (*lx->p == '\0') return -1;

        return width;                // 内容のある行が見つかった
    }
}
```

**★ 空行では `NEWLINE` も出しません。** Python と同じ規則です。
これにより「`NEWLINE` が 2 個連続する」ケースが構造的に消えるので、
構文解析器で「余分な `NEWLINE` を読み飛ばす」処理が不要になります。

### ✅ 確認

```bash
$ cat tests/cases/tok_blank_and_comment.my
# TOKENS: INT NEWLINE EOF

    # インデントされたコメント（インデントに影響しない）

7

# 末尾のコメント
```

トークン列は `INT NEWLINE EOF` だけになります。
**インデントされたコメントがあっても `INDENT` は出ません。**

---

## 4.6 括弧の中では改行を無視する

### 📖 論理行と物理行

言語仕様 2.3 で、括弧の中では改行が無視されると決めています。

```python
(1 +
 2)          # 物理行 2 つ = 論理行 1 つ
```

`NEWLINE` は「**論理行**の終わり」なので、括弧の中の改行では出しません。

### ✍️ 実装：括弧の深さを数える

```c
static const char *OPEN_BRACKETS = "(";
static const char *CLOSE_BRACKETS = ")";

static int read_punct(Lexer *lx) {
    for (int i = 0; PUNCTS[i]; i++) {
        ...
        if (len == 1) {
            if (strchr(OPEN_BRACKETS, *lx->p)) {
                lx->paren_depth++;
            } else if (strchr(CLOSE_BRACKETS, *lx->p)) {
                if (lx->paren_depth > 0) lx->paren_depth--;
            }
        }
        ...
    }
}
```

第10章で `[` `]`、第12章で `{` `}` を足すときは、
**この 2 つの文字列に 1 文字ずつ加えるだけ**です。

### ⚠️ 深さを負にしない

```c
if (lx->paren_depth > 0) lx->paren_depth--;
```

対応しない `)` があったとき（例：`1 + 2)`）、深さを負にしてはいけません。

**もし負にすると、それ以降ずっと `paren_depth != 0` になり、
改行が一切無視されて、まったく別の場所で不可解なエラーになります。**

対応しない `)` は**構文解析器が報告する**のが正しい役割分担です。
字句解析器は「壊れた入力でも壊れた状態にならない」ことだけを守ります。

### 📖 括弧の中では行頭処理をしない

```c
if (at_line_start && lx.paren_depth == 0) {
    int width = scan_indent(&lx);
    ...
}
```

**`paren_depth == 0` の条件が重要です。**
括弧の中では論理行が続いているので、行頭であってもインデントを測りません。

```python
(1 +
        5)      # この 8 個の空白はインデントではない（ただの空白）
```

### ✅ 確認

```bash
$ ./build/mythonc --dump-tokens tests/cases/tok_indent_in_paren.my | awk '{print $2}'
PUNCT INT PUNCT INT PUNCT NEWLINE EOF
```

**`INDENT` が出ていません。** `NEWLINE` は閉じ括弧の後に 1 個だけです。

---

## 4.7 ファイル末尾の DEDENT

### 📖 問題

```python
1
    2
        3     ← ファイルがここで終わる
```

ファイルが深いインデントの中で終わったら、
**開いているブロックぶんの `DEDENT` を全部出す**必要があります。

出さないと、第7章で `block` 規則が「`DEDENT` が来るまで文を読む」と書いたときに、
**ブロックが永遠に閉じられません。**

### ✍️ 実装

```c
// ★ ファイル末尾では、開いているインデントぶんの DEDENT を全部出す。
while (lx.indent_len > 1) {
    lx.indent_len--;
    tv_push(&lx, TK_DEDENT, end_loc, 0);
}

tv_push(&lx, TK_EOF, end_loc, 0);
```

`indent_len > 1` が条件です（底の 0 は残す）。

### 📖 位置は EOF と同じ場所

第3章で作った「EOF は直前の行の末尾を指す」ロジックを、
末尾の `DEDENT` にも使い回します。

```c
const char *end_loc = lx.p;
if (lx.p == lx.line_start && lx.prev_line_start) {
    lx.line_start = lx.prev_line_start;
    lx.line = lx.prev_line;
    const char *eol = lx.prev_line_start;
    while (*eol && *eol != '\n') eol++;
    end_loc = eol;
}
```

### ✅ 確認

```bash
$ ./build/mythonc --dump-tokens tests/cases/tok_indent_eof.my | awk '{print $2}'
INT NEWLINE INDENT INT NEWLINE INDENT INT NEWLINE DEDENT DEDENT EOF
```

**`DEDENT` が 2 個出て、その後に `EOF`。** 正しい形です。

---

## 4.8 メインループの再構成

第2章までのメインループは「1 文字見て種類ごとに分岐」という単純な形でした。
`INDENT` の処理が入ると、**「行頭かどうか」という状態**が必要になります。

### ✍️ 実装

```c
// 次に読むのが「論理行の先頭」かどうか。最初は当然そう。
bool at_line_start = true;

while (*lx.p) {
    // ── ① 行頭処理：インデントを測って INDENT / DEDENT を出す ──
    if (at_line_start && lx.paren_depth == 0) {
        int width = scan_indent(&lx);
        if (width < 0) break;          // 空行・コメントだけで入力が終わった
        emit_indent_tokens(&lx, width);
        at_line_start = false;
        continue;
    }
    at_line_start = false;

    // ── ② 改行：NEWLINE を出して行頭に戻る ──
    if (*lx.p == '\n') {
        if (lx.paren_depth == 0) {
            tv_push(&lx, TK_NEWLINE, lx.p, 0);
            at_line_start = true;      // ★ 次の周回で ① が走る
        }
        advance_newline(&lx);
        continue;
    }

    // ── ③ 以降は第2章までと同じ（空白・タブ・コメント・数値・記号）──
    ...
}
```

### 📖 状態遷移

```
       ┌──────────────────────────┐
       │  at_line_start = true    │
       └────────────┬─────────────┘
                    │ ① scan_indent + emit_indent_tokens
                    ▼
       ┌──────────────────────────┐
       │  at_line_start = false   │◀─┐
       │  （行の中身を読む）        │  │ 数値・記号・空白・コメント
       └────────────┬─────────────┘──┘
                    │ ② '\n' に出会う（括弧の外）
                    └──────────────▶ at_line_start = true に戻る
```

### 📖 `advance_newline` を関数に切り出した

改行を読み進める処理が 3 か所（`scan_indent` の空行、
`scan_indent` のコメント行、メインループの `'\n'`）に現れたので関数にしました。

```c
static void advance_newline(Lexer *lx) {
    lx->prev_line_start = lx->line_start;   // 第3章：EOF の位置決め用
    lx->prev_line = lx->line;

    lx->p++;
    lx->line++;
    lx->line_start = lx->p;
}
```

**⚠️ ここを 3 か所にコピペすると、必ず 1 か所だけ
`prev_line_start` の更新を忘れて、EOF の位置がおかしくなります。**
第2章の `span_token` と同じ「3 回目でまとめる」判断です。

---

## 4.9 構文解析器：複数文と ND_BLOCK

**ここが一番あっさりしています。それがこの章の成果です。**

### ✍️ AST：`ND_BLOCK` と `next`

```c
typedef enum {
    ND_INT, ND_BINOP, ND_UNARY,
    ND_BLOCK,   // 文のリスト → body（next で連結）
} NodeKind;

struct Node {
    ...
    Node *body;   // ND_BLOCK の中身（先頭の文）
    Node *next;   // 兄弟ノード
};
```

### 🤔 なぜ文はリストで、トークンは配列なのか

第1章でトークンは**配列**にしました。文は**単方向リスト**にします。矛盾しません。

| | 必要なアクセス | 選択 |
|---|---|---|
| トークン | 任意の位置へ O(1)（`peek_at(p, 2)` の先読み） | **配列** |
| 文 | 先頭から順に 1 回たどるだけ | **リスト** |

リストなら要素数を先に数える必要がなく、追加が `cur->next = s` だけで済みます。
**データ構造は「必要なアクセスパターン」から決めます。**

### ✍️ `stmt` と `program`

```c
// stmt ::= expr NEWLINE
static Node *stmt(Parser *p) {
    Node *e = expr(p);

    Token *t = peek(p);
    if (t->kind != TK_NEWLINE) {
        Diag d = {0};
        d.message = "式の後に余分なトークンがあります";
        d.primary.tok = t;
        d.primary.label = "ここから先が解釈できません";
        d.hint = "1 行に書けるのは 1 つの式です（改行で区切ってください）";
        diag_fail(&d);
    }
    advance(p);   // NEWLINE を消費
    return e;
}
```

```c
// program ::= { stmt } EOF
static Node *program(Parser *p) {
    Node head = {0};        // ★ ダミーの先頭ノード
    Node *cur = &head;

    Token *first = peek(p);

    while (peek(p)->kind != TK_EOF) {
        Token *t = peek(p);
        if (t->kind == TK_INDENT) { /* 予期しないインデント：エラー */ }
        if (t->kind == TK_DEDENT) { UNREACHABLE(); }

        cur->next = stmt(p);
        cur = cur->next;
    }

    if (!head.next) { /* 空のプログラム：エラー */ }

    Node *blk = new_node(ND_BLOCK, first);
    blk->body = head.next;
    return blk;
}
```

### 📖 ダミーの先頭ノード（sentinel）

```c
Node head = {0};
Node *cur = &head;
...
cur->next = stmt(p);
cur = cur->next;
...
blk->body = head.next;
```

**`head` はスタック上のダミーで、AST には入りません。**

これがないと「最初の要素かどうか」の分岐が必要になります。

```c
// ✗ ダミーなしだと分岐が必要
if (!first_node) { first_node = s; cur = s; }
else { cur->next = s; cur = s; }
```

**ダミー要素で特殊ケースを消す**のは、4.4 節でスタックの底に 0 を置いたのと同じ発想です。
連結リストを扱うときの定番テクニックです。

### 📖 `UNREACHABLE()` の使いどころ

```c
if (t->kind == TK_DEDENT) {
    // 字句解析器の不整合。ユーザーのミスでは起こり得ない。
    UNREACHABLE();
}
```

トップレベルで `DEDENT` が来るのは、
**`INDENT` を出していないのに `DEDENT` を出した**ということです。
これは字句解析器のバグであり、ユーザーの書き方の問題ではありません。

だから `error_at`（ユーザーへの診断）ではなく `UNREACHABLE()`
（コンパイラのバグ報告）を使います。

```
mythonc internal error: src/parser.c:NNN: 到達しないはずのコードに来ました
  これはコンパイラ自身のバグです。報告してください。
```

**ユーザーのミスと自分のバグを区別する**のは、第1章から守っている原則です。

### ✍️ コード生成

```c
case ND_BLOCK: {
    char *last = NULL;
    for (Node *s = n->body; s; s = s->next) last = gen_expr(e, s);
    if (!last) UNREACHABLE();   // 空ブロックは parser が弾いている
    return last;
}
```

**4 行です。** 文を順に生成して、最後の値を返すだけ。

---

## 4.10 テストランナーに `# TOKENS:` を追加

### 📖 なぜ必要か

`INDENT` / `DEDENT` を消費する構文がまだないので、
**実行結果（`# EXIT:`）ではトークン合成の正しさを検証できません。**

そこで「トークン列そのもの」を検証する仕組みを追加します。

### ✍️ テストケースの書き方

```python
# TOKENS: INT NEWLINE INDENT INT NEWLINE INDENT INT NEWLINE
# TOKENS: DEDENT DEDENT INT NEWLINE EOF
1
    2
        3
4
```

複数行書けます（空白 1 個で連結して比較）。長い列でも読みやすくなります。

### ✍️ 実装

```bash
want_tokens="$(sed -n 's/^# *TOKENS: *//p' "$case_file" \
               | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"

actual_tokens="$("$MYTHONC" --dump-tokens "$case_file" 2>/dev/null \
                 | awk '{print $2}' | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"

if [ "$actual_tokens" != "$want_tokens" ]; then
    report_fail "$name" "トークン列が期待と違います
期待: $want_tokens
実際: $actual_tokens"
fi
```

**`awk '{print $2}'` で種別の列だけを抜き出します。**
`--dump-tokens` の出力形式（`番号 種別 位置 値`）を利用しているので、
コンパイラ側に新しいオプションを足す必要がありません。

### ★ 検証ロジックをテストする

第3章と同じく、**わざと間違えて落ちることを確認**します。
`DEDENT` を 1 個だけ期待するように書き換えてみます。

```
FAIL  tok_indent_deep.my
      トークン列が期待と違います
      期待: INT NEWLINE INDENT INT NEWLINE INDENT INT NEWLINE DEDENT INT NEWLINE EOF
      実際: INT NEWLINE INDENT INT NEWLINE INDENT INT NEWLINE DEDENT DEDENT INT NEWLINE EOF
```

**差分が一目で分かります。** `DEDENT DEDENT` が正しいことも確認できました。

### 📖 この仕組みは第16章で本番運用される

```bash
# 第16章：Mython 版字句解析器の検証
./build/mythonc      --dump-tokens all_syntax.my > /tmp/c.txt
./build/stage1-lexer --dump-tokens all_syntax.my > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt
```

**今作った `# TOKENS:` は、その予習です。**
「字句解析器を単独で検証する」という発想が、セルフホストの土台になります。

---

## 4.11 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
全 46 件パス
```

10 件追加しました（トークン検証 5 件、実行 2 件、エラー 3 件）。
ビルド警告 0 件、ASan/UBSan もクリーンです。

### ✅ 仮想トークンの合成（位置情報つき）

```bash
./build/mythonc --dump-tokens tests/cases/tok_indent_deep.my
```

```
  0  INT       4:1    1
  1  NEWLINE   4:2    
  2  INDENT    5:5    
  3  INT       5:5    2
  4  NEWLINE   5:6    
  5  INDENT    6:9    
  6  INT       6:9    3
  7  NEWLINE   6:10   
  8  DEDENT    7:1    
  9  DEDENT    7:1    
 10  INT       7:1    4
 11  NEWLINE   7:2    
 12  EOF       7:2    
```

読みどころが 3 つあります。

1. **`INDENT` と直後の `INT` が同じ位置（5:5）**
   仮想トークンは実体がないので、次のトークンの位置を借りています。
2. **`DEDENT` が 2 個、同じ位置（7:1）に並んでいる**
   一気に 2 段戻ったことが見えます。
3. **`NEWLINE` は行末の位置（4:2）**
   `1` の直後を指しています。

### ✅ 複数文の AST

```bash
./build/mythonc --dump-ast tests/cases/multi_stmt.my
```

```
(block
  (binop +
    (int 1)
    (int 2)
  )
  (binop *
    (int 3)
    (int 4)
  )
)
```

`ND_BLOCK` の下に 2 つの文が並んでいます。

### ✅ 生成された IR

```bash
./build/mythonc -S tests/cases/multi_stmt.my | sed -n '/mython_main/,/^}/p'
```

```llvm
define i64 @mython_main() {
entry:
  %t0 = add i64 1, 2
  %t1 = mul i64 3, 4
  ret i64 %t1
}
```

**`%t0` が計算されているのに使われていません。**
現時点では式に副作用がないので、最後以外の値は捨てられます。

### ✅ LLVM が無駄を消す

```bash
$(brew --prefix llvm)/bin/opt -O2 -S /tmp/m.ll | sed -n '/@mython_main/,/^}/p'
```

```llvm
define noundef i64 @mython_main() local_unnamed_addr #0 {
entry:
  ret i64 12
}
```

**使われない `add` が消え、`mul` は定数畳み込みされました。**

**⚠️ 「値が捨てられる文」を生成してよいのか？**
今は問題ありません。第5章で代入（`x = 1`）が入ると、
「順に実行する」ことに意味が出ます。
言語仕様 5.8 では副作用のない式文を警告対象としていますが、
警告の実装は型検査器（第5章）が必要なのでそこで扱います。

### ✅ 実行

```bash
./build/mythonc tests/cases/multi_stmt.my -o /tmp/t && /tmp/t; echo $?
```

```
12
```

### ✅ 括弧による行継続

```bash
./build/mythonc tests/cases/tok_paren_continuation.my -o /tmp/t && /tmp/t; echo $?
```

`(1 +\n 2)` に対して `3`。**改行をまたいで 1 つの式として解析されています。**

### ✅ エラー：予期しないインデント

```bash
./build/mythonc tests/cases/err_unexpected_indent.my -o /tmp/x
```

```
error: 予期しないインデントです
  --> tests/cases/err_unexpected_indent.my:5:5
   |
 5 |     2
   |     ^ この行が余分に字下げされています
   |
   = ヒント: ブロックを作る構文（if / while / def）は第7章以降で実装します
```

### ✅ エラー：インデントが揃っていない

```bash
./build/mythonc tests/cases/err_indent_misaligned.my -o /tmp/x
```

```
error: インデントが揃っていません（どのブロックにも対応しません）
  --> tests/cases/err_indent_misaligned.my:5:1
   |
 5 |     3
   | ^^^^
   |
   = ヒント: 外側のブロックのインデント幅と正確に一致させてください
```

**インデント部分そのものに下線が引かれています。**

### ✅ 深さの上限が安全に失敗する

64 段以上インデントしたファイルを与えると：

```
error: インデントが深すぎます（最大 63 段）
```

**バッファオーバーフローではなく明確なエラーになります。**
ASan でも検証済みです。

---

## 4.12 まとめと次章の予告

### できたこと

```
✅ 仮想トークン NEWLINE / INDENT / DEDENT の合成
✅ インデントスタック（底に 0 を置く / 上限チェック）
✅ DEDENT を一度に複数個出す
✅ 揃わないインデントの検出
✅ 空行・コメント行をインデント計算から除外
✅ 括弧の中では改行を無視（論理行）
✅ ファイル末尾で開いている DEDENT を全部出す
✅ 複数文の解析（ND_BLOCK / Node.next）
✅ テストランナーに # TOKENS: を追加
✅ テスト 46 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/lexer.h` | `TK_NEWLINE` / `TK_INDENT` / `TK_DEDENT` |
| `src/lexer.c` | インデントスタック、`scan_indent`、`emit_indent_tokens`、`advance_newline`、括弧の深さ、末尾 DEDENT、メインループ再構成 |
| `src/ast.h/c` | `ND_BLOCK`、`body` / `next`、ブロックのダンプ |
| `src/parser.c` | `stmt`、`program`（複数文・ダミー先頭ノード・INDENT エラー） |
| `src/codegen.c` | `ND_BLOCK` の生成（4 行） |
| `tests/run_tests.sh` | `# TOKENS:` 対応 |
| `tests/cases/` | 10 件追加 |
| `docs/spec/grammar.md` | ch4 の実装範囲を正確に記述 |

### この章で学んだ設計上の教訓

1. **難しさを 1 か所に閉じ込める**
   仮想トークンにより、インデントの複雑さが字句解析器の中だけに収まった。
   構文解析器は 1 度もインデント幅を見ない。
   **これが「インデント言語は難しい」を「スタック 1 本」に変えた。**

2. **ダミー要素で特殊ケースを消す**
   インデントスタックの底の `0`、文リストのダミー先頭ノード。
   どちらも「空のとき」の分岐を消すために置いている。

3. **データ構造は必要なアクセスパターンから決める**
   トークンは配列（任意位置 O(1) の先読み）、文はリスト（先頭から 1 回）。
   一貫性より適合性。

4. **壊れた入力でも壊れた状態にしない**
   `paren_depth` を負にしない。負にすると、遠く離れた場所で
   原因不明のエラーが出る。誤りの報告は適切な担当（構文解析器）に任せる。

5. **消費者がいなくても生成側は検証できる**
   `--dump-tokens` があるから、`INDENT` を使う構文が無くても正しさを確認できた。
   第1章のデバッグ道具への投資が回収された。

### ✍️ commit する

```bash
git add -A
git commit -m "第4章: インデント構文（NEWLINE / INDENT / DEDENT）"
```

---

## 次章：第5章 変数と型検査パスの導入

**この章もまた大きな節目です。パスが 3 段から 4 段に増えます。**

**達成目標**

```python
x: int = 1
x = x + 2
x
```

→ 終了コード 3。そして型が合わないとコンパイルエラー。

**やること**

| ファイル | 作業 |
|---|---|
| `src/types.{h,c}` | **新規**。`Type` 構造体、`type_equal`、プリミティブ型のシングルトン |
| `src/sema.{h,c}` | **新規**。**意味解析パス**。スコープ、シンボルテーブル、型検査 |
| `lexer.c` | `TK_IDENT` / `TK_KEYWORD`、`=` `+=` などの記号 |
| `parser.c` | `peek_at()` を復活（`x : int` と `x = 1` の判別に 2 トークン先読み） |
| `parser.c` | `var_decl` / `assign_stmt` / `ND_VAR` |
| `codegen.c` | **`alloca` / `store` / `load`** — ついに「全部 alloca 方式」の本番 |

**学ぶ中心概念**

- **意味解析パス**とは何か（構文的に正しいが意味が通らないものを弾く）
- **シンボルテーブルとスコープ**（内側から外側へ名前を解決する）
- **`alloca` / `store` / `load`** — SSA の制約を回避する設計
  → [../design/ir-conventions.md](../design/ir-conventions.md) 第1節が本番を迎えます
- 型注釈を必須にすると型検査器が「調べるだけ」になり実装が単純になること

**⚠️ 予想される落とし穴**

- `x: int` と `x = 1` の判別に **2 トークン先読み**が必要
  → 第1章でトークンを配列にした理由がここで効きます
- `alloca` は**必ず entry ブロックに置く**（規約 R1）
- 型検査で「`/` は int に使えない」という検査を codegen から sema へ移す
  （第2章からの持ち越し課題）

**予習**：[../spec/type-system.md](../spec/type-system.md) の第 2〜7 節と、
[../reference/llvm-ir-primer.md](../reference/llvm-ir-primer.md) の第 5 節（alloca / store / load）。
**IR 入門をまだ手を動かしていない人は、ここで必ずやってください。**

### 🤔 第5章に入る前の練習問題

1. **`OPEN_BRACKETS` に `[` を、`CLOSE_BRACKETS` に `]` を追加**して、
   `PUNCTS` にも `[` `]` を足すと何が起きるか試す
   （`[1 +\n 2]` が 1 論理行になるはず。第10章の予習）
2. **インデント幅を 2 にしたコードが動くか**確認する
   （言語仕様 2.4 は「同じブロック内で一貫していればよい」）
3. **`emit_indent_tokens` の `while` を `if` に変えて** `make test` を走らせ、
   どのテストが落ちるか確認する（`tok_indent_deep.my` が守っているはず）
4. **`paren_depth` を負にできるようにして** `1 + 2)` の後に
   インデントされた行を書くとどうなるか観察する（**元に戻すのを忘れずに**）
