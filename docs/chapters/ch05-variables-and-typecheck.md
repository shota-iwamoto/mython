# 第5章 変数と型検査パスの導入

> **この章のゴール**
> 変数が使えるようになり、**意味解析パス（3 つ目のパス）**が加わる。
>
> ```bash
> $ cat t.my
> x: int = 1
> x = x + 2
> x
> $ ./build/mythonc t.my -o t && ./t; echo $?
> 3
> ```

**この章は 2 つの大きな節目です。**

1. **パスが 3 段から 4 段に増える** — `src/sema.c`（意味解析）の新設
2. **`alloca` / `store` / `load` の本番** — 設計文書で決めた「全部 alloca 方式」がついに動く

---

## 目次

- [5.1 パイプラインに 3 段目が入る](#51-パイプラインに-3-段目が入る)
- [5.2 型の表現：シングルトンにする](#52-型の表現シングルトンにする)
- [5.3 字句解析器：識別子とキーワード](#53-字句解析器識別子とキーワード)
- [5.4 構文解析器：2 トークン先読みの回収](#54-構文解析器2-トークン先読みの回収)
- [5.5 代入文の見分け方](#55-代入文の見分け方)
- [5.6 複合代入の脱糖](#56-複合代入の脱糖)
- [5.7 意味解析：スコープとシンボルテーブル](#57-意味解析スコープとシンボルテーブル)
- [5.8 意味解析：型検査](#58-意味解析型検査)
- [5.9 コード生成：全部 alloca 方式の本番](#59-コード生成全部-alloca-方式の本番)
- [5.10 責務の移動：`/` の検査を sema へ](#510-責務の移動-の検査を-sema-へ)
- [5.11 動作確認](#511-動作確認)
- [5.12 まとめと次章の予告](#512-まとめと次章の予告)

---

## 5.1 パイプラインに 3 段目が入る

```
ソース ─▶ ① 字句解析 ─▶ ② 構文解析 ─▶ ③ 意味解析 ─▶ ④ コード生成 ─▶ LLVM
                                        ~~~~~~~~~~
                                        ★ 今回追加
```

### 📖 意味解析は何を答えるのか

各パスが答える質問を並べると、役割の違いが見えます。

| パス | 答える質問 | 弾く例 |
|---|---|---|
| ① 字句解析 | この文字の並びは**単語**に分けられるか | `1 @@ 2` |
| ② 構文解析 | 単語の並びは**文として成り立つ**か | `1 + * 2` |
| **③ 意味解析** | **成り立つ文だが、意味は通るか** | `y = 1`（y は未宣言） |
| ④ コード生成 | 意味の通る木を**どう命令列にする**か | （エラーを出さないのが理想） |

```python
y = 1        # 構文は完璧に正しい。でも y は宣言されていない
x: foo = 1   # 構文は完璧に正しい。でも foo という型は存在しない
7 / 2        # 構文は完璧に正しい。でも int に '/' は使えない
```

**これらは全部「構文解析器では検出できない」誤り**です。
だから 3 つ目のパスが必要になります。

### 📖 sema は AST を書き換えない

```c
void sema(Node *ast);   // 戻り値なし
```

意味解析パスがやることは 2 つだけです。

1. **誤りを見つけたらエラーを出して終了する**
2. **各ノードの `type` フィールドを埋める**

```c
struct Node {
    ...
    // ★ この式の型。**意味解析パス (sema.c) が埋めます。**
    //   構文解析の直後は必ず NULL です。
    Type *type;
};
```

コード生成器はこの `type` を見て命令を選びます。

```c
sb_printf(&e->body, "  %s = %s %s %s, %s\n", t, inst, llvm_type(n->type), l, r);
//                                               ~~~~~~~~~~~~~~~~~~~~
//                                               sema が埋めた型を使う
```

**🤔 なぜ AST を書き換えないのか**：「検査するだけ」に徹すると、
パスの責務が明確になり、`--dump-ast` の出力が構文解析の結果そのままになります。
第6章以降で型変換の挿入（`int` → `float`）が必要になったら書き換えも入りますが、
Mython は暗黙変換を持たないのでその必要がありません。**仕様の単純さが実装の単純さになっています。**

---

## 5.2 型の表現：シングルトンにする

### ✍️ `src/types.h`

```c
typedef enum {
    TY_INT,  // int → i64
    // ── 以降の章で追加していく ──
    // TY_BOOL,   // 第6章
    // TY_FLOAT, TY_STR, TY_NONE,   // 第9章
} TypeKind;

struct Type {
    TypeKind kind;
};

extern Type *ty_int;      // ★ シングルトン
void types_init(void);
bool type_equal(Type *a, Type *b);
const char *type_name(Type *t);
Type *type_from_name(const char *name);
```

### 🤔 なぜシングルトンにするのか

`int` 型のオブジェクトを毎回 `xmalloc` するのは無駄です。
起動時に 1 個だけ作り、ポインタを共有します。

```c
void types_init(void) { ty_int = new_type(TY_INT); }
```

副作用として、**プリミティブ型どうしの比較がポインタ比較で済みます**。

```c
bool type_equal(Type *a, Type *b) {
    if (a == b) return true;        // シングルトンなので大半はここで済む
    if (a->kind != b->kind) return false;
    // 第10章：list[T] なら要素型を再帰比較する
    return true;
}
```

### ⚠️ それでも `type_equal()` を必ず通す

`a == b` で直接比較したくなりますが、**やってはいけません。**
第10章で `list[int]` のような複合型が入ると、
同じ意味の型が別のオブジェクトとして作られます。

```c
// ✗ これは list[int] == list[int] で false になる
if (a == b) { ... }

// ✅ 常にこう書く
if (type_equal(a, b)) { ... }
```

**「今は動くが将来壊れる書き方」を最初から禁止する**のが、
長いプロジェクトを完走させるコツです。

### 📖 型名の解決は sema の仕事

```c
Type *type_from_name(const char *name) {
    if (strcmp(name, "int") == 0) return ty_int;
    return NULL;  // 未知の型名
}
```

構文解析器は `x: int = 1` の `int` を**文字列として記録するだけ**です。

```c
struct Node {
    ...
    char *type_name;   // 「int」のような文字列
};
```

**🤔 なぜ parser が `Type *` に解決しないのか**
名前から型への解決は「意味」の話であって「構文」の話ではありません。
parser は「そこに識別子が書かれている」ことだけを記録し、
それが有効な型かどうかの判断は sema に任せます。
**パスの責務を混ぜない**ための分離です。

---

## 5.3 字句解析器：識別子とキーワード

### ✍️ 識別子

```c
static int is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_ident_cont(char c) { return is_ident_start(c) || is_digit(c); }

static void read_ident(Lexer *lx) {
    const char *start = lx->p;
    while (is_ident_cont(*lx->p)) lx->p++;
    int len = (int)(lx->p - start);

    TokenKind kind = is_keyword(start, len) ? TK_KEYWORD : TK_IDENT;
    Token *t = tv_push(lx, kind, start, len);
    t->text = xstrndup(start, (size_t)len);   // ★ NUL 終端した複製を持たせる
}
```

**⚠️ 数値の判定より後に置きます。**
`123abc` のような入力で、先に識別子判定をすると `123` が読めません。

### 📖 記号と違い、名前は複製する

第2章では記号を複製せず `loc` / `len` で比較していました
（`tok_is(t, "+")`）。識別子は**複製します**。

| | 比較の頻度 | 方法 |
|---|---|---|
| 記号 | 多い（`tok_is`） | `loc`/`len` で `memcmp`。複製なし |
| 識別子 | 多い（変数名の照合） | **`strcmp` したいので複製する** |

シンボルテーブルは `strcmp(v->name, name)` で名前を照合します。
毎回 `memcmp(loc, name, len)` と長さ比較を書くより、
**NUL 終端文字列を 1 度作るほうが圧倒的に扱いやすい**です。

### ✍️ キーワードは「今使わないものまで」予約する

```c
static const char *KEYWORDS[] = {
    // 言語仕様 v1 で使う語
    "and", "as", "break", "class", "continue", "def", "elif", "else",
    "extern", "False", "for", "if", "import", "in", "is", "None",
    "not", "or", "pass", "return", "True", "while",
    // 将来のために予約（使うとエラーになる）
    "assert", "const", "del", "except", "finally", "from", "global", "lambda",
    "match", "nonlocal", "raise", "try", "with", "yield",
    NULL,
};
```

**🤔 なぜ未使用の語まで予約するのか**（言語仕様 2.5）

今 `class` を変数名に使えるようにしてしまうと、
第12章で `class` 構文を入れたときに**既存のコードが壊れます**。
最初から予約しておけば、**後方互換を壊さずに機能を追加**できます。

言語設計では「後から禁止する」のは非常に高コストです。
最初に広く予約しておくのが定石です。

### 📖 予約語が式の位置に来たら専用のエラーを出す

```c
if (t->kind == TK_KEYWORD)
    error_at_hint(t, "予約語は変数名として使えません（言語仕様 2.5）",
                  "'%s' は予約語です", t->text);
```

```
error: 'class' は予約語です
  --> tests/cases/err_keyword_as_var.my:4:1
   |
 4 | class + 1
   | ^^^^^
   |
   = ヒント: 予約語は変数名として使えません（言語仕様 2.5）
```

「式が必要です」だけだと、**なぜ `class` が使えないのか分かりません。**
第3章で学んだ「未実装・禁止は、そうだと明示する」原則です。

### ✍️ 新しい記号と最長一致

```c
static const char *PUNCTS[] = {
    // 3 文字
    "//=",
    // 2 文字
    "//", "**", "<<", ">>", "+=", "-=", "*=", "%=",
    // 1 文字
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "(", ")", ":", "=",
    NULL,
};
```

**⚠️ 第2章の最長一致の原則がまた効きます。**

- `"//="` は `"//"` より先（さもないと `//=` が `//` と `=` に割れる）
- `"+="` は `"+"` より先
- **3 文字の記号が登場したので、配列の先頭に新しい段が増えました**

---

## 5.4 構文解析器：2 トークン先読みの回収

### 📖 問題：`x: int = 1` と `x = 1` はどちらも IDENT で始まる

```
x : int = 1      →  変数宣言
x = 1            →  代入
x + 1            →  式文
```

**1 トークン目（`x`）だけでは区別できません。**

### ✍️ 解決：`peek_at()` で 2 個目を見る

```c
// n 個先のトークンを覗く（消費しない）。
static Token *peek_at(Parser *p, int n) {
    int i = p->pos + n;
    if (i >= p->toks.len) i = p->toks.len - 1;  // EOF より先は EOF を返す
    return &p->toks.data[i];
}
```

```c
// IDENT の次が ':' なら変数宣言。2 トークン先読みで判別する。
if (peek(p)->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) return var_decl(p);
```

### ★ 第1章の設計判断が回収された

第1章で、トークンを**リンクリストではなく配列**にしました。その理由がこれです。

> 3. **任意の先読みが O(1)** … 第5章で「`x` の次が `:` か」を判定するのに
>    `peek_at(p, 1)` が必要。リンクリストだと辿る必要があります。
>
> — 第1章 1.6 節

配列なら `toks[pos + 1]` で即座にアクセスできます。
リンクリストなら `tok->next` を辿る必要があり、
「2 個先」「3 個先」が増えるたびにコードが汚れます。

**⚠️ `i >= p->toks.len` のクランプを忘れないこと。**
ファイル末尾付近で `peek_at(p, 2)` を呼ぶと配列外アクセスになります。
EOF を返すようにしておけば、**構造的に範囲外アクセスが起きません。**
第1章で `advance()` が EOF で止まる設計にしたのと同じ発想です。

### ✍️ `var_decl`

```c
// var_decl ::= IDENT ":" type "=" expr
static Node *var_decl(Parser *p) {
    Token *name_tok = advance(p);  // IDENT
    advance(p);                    // ":"

    Token *ty_tok = peek(p);
    if (ty_tok->kind != TK_IDENT)
        error_at_hint(ty_tok, "型注釈には型名を書きます（例: x: int = 0）",
                      "型名が必要です");
    advance(p);

    // 初期化式は必須（言語仕様 5.1：未初期化変数を作らせない）
    if (!tok_is(peek(p), "=")) {
        Diag d = {0};
        d.message = "変数宣言には初期化式が必要です";
        d.primary.tok = peek(p);
        d.primary.label = "ここに '= 初期値' が必要です";
        d.hint = "Mython では未初期化の変数を作れません（例: x: int = 0）";
        diag_fail(&d);
    }
    advance(p);  // "="

    Node *n = new_node(ND_VARDECL, name_tok);
    n->name = name_tok->text;
    n->type_name = ty_tok->text;
    n->rhs = expr(p);
    return n;
}
```

### 📖 初期化を文法レベルで強制する意味

```
error: 変数宣言には初期化式が必要です
  --> t.my:1:7
   |
 1 | x: int
   |       ^ ここに '= 初期値' が必要です
   |
   = ヒント: Mython では未初期化の変数を作れません（例: x: int = 0）
```

**「未初期化変数が存在しない」という保証を、文法で作っています。**

これにより後続のすべての章で「この変数は初期化されているか？」を
考える必要がなくなります（[../design/memory-model.md](../design/memory-model.md) 第6節）。
C の未初期化変数バグの類が、**言語仕様レベルで不可能**になります。

---

## 5.5 代入文の見分け方

### 📖 問題：左辺を読み切るまで代入かどうか分からない

```python
x = 1          # 代入
x + 1          # 式文
xs[0] = 1      # 代入（第10章）
p.f = 1        # 代入（第12章）
f() = 1        # 誤り
```

`xs[0] = 1` の場合、`xs[0]` を読み終わるまで
「代入文なのか式文なのか」判断できません。

### ✍️ 解決：式として読んで、後から役割を決める

```c
static Node *simple_stmt(Parser *p) {
    // IDENT の次が ':' なら変数宣言
    if (peek(p)->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) return var_decl(p);

    Node *lhs = expr(p);                        // ① まず式として読む
    Token *t = peek(p);

    int aug = aug_op(t);
    if (!tok_is(t, "=") && aug < 0) return lhs;  // ② '=' が無い → 式文だった

    // ③ '=' があった → 今読んだ式は代入先だった
    if (lhs->kind != ND_VAR) {
        Diag d = {0};
        d.message = "この式には代入できません";
        d.primary.tok = lhs->tok;
        d.primary.label = "代入先にできるのは変数だけです";
        d.hint = "添字 xs[0] やフィールド p.f への代入は第10章・第12章で対応します";
        diag_fail(&d);
    }
    ...
}
```

**これは「式を先に読んで後から役割を決める」という定番の実装テクニック**です
（[../spec/grammar.md](../spec/grammar.md) 第4節）。

第10章で `xs[0] = 1` を実装するときは、
**この関数の `lhs->kind != ND_VAR` の条件を緩めるだけ**で対応できます。
構造を先に正しくしておくと、後の拡張が 1 行で済みます。

```
error: この式には代入できません
  --> tests/cases/err_assign_to_literal.my:3:1
   |
 3 | 1 = 2
   | ^ 代入先にできるのは変数だけです
```

---

## 5.6 複合代入の脱糖

### ✍️ `x += e` を `x = x + e` に書き換える

```c
static int aug_op(Token *t) {
    if (tok_is(t, "+=")) return OP_ADD;
    if (tok_is(t, "-=")) return OP_SUB;
    if (tok_is(t, "*=")) return OP_MUL;
    if (tok_is(t, "//=")) return OP_FLOORDIV;
    if (tok_is(t, "%=")) return OP_MOD;
    return -1;
}
```

```c
// ★ 複合代入は脱糖する（言語仕様 5.2）
//     x += e  →  x = x + e
if (aug >= 0)
    rhs = new_binop_node(t, (OpKind)aug, new_var_node(lhs->tok, lhs->name), rhs);
```

### 📖 脱糖（desugaring）とは

**「便利な書き方」を「基本的な構文」に書き換えること**です。

脱糖のうれしさは、**意味解析とコード生成が何も知らなくて済む**ことです。
`ND_AUGASSIGN` のような新しいノード種別を作らないので、
`sema.c` と `codegen.c` に 1 行も追加が要りません。

### ✅ 確認：AST を見る

```bash
$ printf 'x: int = 10\nx += 5\nx\n' > /tmp/aug.my
$ ./build/mythonc --dump-ast /tmp/aug.my
```

```
(block
  (vardecl x int
    (int 10)
  )
  (assign
    (var x)
    (binop +
      (var x)      ← ★ 左辺の x が右辺にも現れている
      (int 5)
    )
  )
  (var x)
)
```

**`x += 5` が `x = x + 5` になっていることが目で確認できます。**
`--dump-ast` は脱糖の検証にも使えます。

### ⚠️ 「左辺は 1 回だけ評価」の約束

言語仕様 5.2 は「左辺は 1 回だけ評価する」と定めています。
今の実装は**左辺のコピーを 2 つ作る**ので、2 回評価されます。

変数なら 2 回読んでも同じ値なので問題ありません。しかし：

```python
xs[f()] += 1      # 第10章
```

これを素朴に脱糖すると `xs[f()] = xs[f()] + 1` になり、**`f()` が 2 回呼ばれます**。
副作用があれば結果が変わります。

第10章で添字への代入を実装するときは、
**添字の値を一時変数に取り出してから使う**書き換えが必要になります。
コメントに明記しておきました。

**🤔 今直さない理由**：今は左辺が変数のみなので、2 回評価しても観測不能です。
「起きない問題」を先に解くと、コードが複雑になるだけで検証もできません。
**問題が起きる章で、テストと一緒に直す**のが正しい順序です。
（第3章の「起きないと判断した前提が崩れた」経験から、
**理由をコメントに残す**ことは徹底しています。）

---

## 5.7 意味解析：スコープとシンボルテーブル

### ✍️ データ構造

```c
typedef struct VarEntry VarEntry;
struct VarEntry {
    char *name;
    Type *type;
    Token *decl_tok;  // ★ 宣言された位置（再宣言エラーで「前の宣言はここ」を示す）
    VarEntry *next;
};

typedef struct Scope Scope;
struct Scope {
    Scope *parent;   // 外側のスコープ（グローバルなら NULL）
    VarEntry *vars;  // このスコープで宣言された変数
};
```

### 🤔 なぜハッシュテーブルではなく線形リストなのか

1 つのスコープに宣言される変数は普通 10 個程度です。
線形探索で十分速く、**コードは 5 行で済みます**。

```c
static VarEntry *lookup(Sema *s, const char *name) {
    for (Scope *sc = s->scope; sc; sc = sc->parent)      // 内側から外側へ
        for (VarEntry *v = sc->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0) return v;
    return NULL;
}
```

**「まず動かす、測ってから直す」**が原則です
（[../spec/type-system.md](../spec/type-system.md) 7.2）。
第20章でコンパイル速度が問題になったら、そのとき測って直します。

### 📖 2 種類の検索を用意する

```c
static VarEntry *lookup_local(Sema *s, const char *name);  // 現在のスコープだけ
static VarEntry *lookup(Sema *s, const char *name);        // 内側から外側へ
```

用途が違います。

| 関数 | 用途 |
|---|---|
| `lookup_local` | **再宣言の検査**（同じスコープに既にあるか） |
| `lookup` | **名前解決**（どこかで宣言されているか） |

`lookup` で再宣言を検査してしまうと、
外側のスコープに同名があるだけでエラーになります（第7章で問題化する）。
**この 2 つを分けておくのが重要です。**

### ⚠️ 登録の「順序」が意味を決める

```c
static void check_vardecl(Sema *s, Node *n) {
    ...
    // ③ 初期化式の型を検査
    Type *actual = check_expr(s, n->rhs);
    ...
    // ④ スコープに登録する。
    //    ★ 順序が重要：初期化式を検査した「後」に登録します。
    declare(s, n->name, declared, n->tok);
}
```

**初期化式を先に検査し、その後で変数を登録します。**

こうすると `x: int = x + 1` が正しくエラーになります。

```
error: 未定義の名前 'x' です
  --> tests/cases/err_self_init.my:3:10
   |
 3 | x: int = x + 1
   |          ^ この名前は宣言されていません
```

もし先に登録してしまうと、**自分自身を参照して未初期化の値を読めてしまいます。**

**★ 「たった 2 行の順序」が言語の意味を決めている例です。**
C では `int x = x + 1;` が未定義動作になりますが、
Mython では**コンパイル時に弾けます**。

---

## 5.8 意味解析：型検査

### ✍️ `check_expr` の約束

```c
// check_expr の約束：
//   「式を検査し、n->type を埋めて、その型を返す」
static Type *check_expr(Sema *s, Node *n) {
    Type *t;
    switch (n->kind) {
        case ND_INT: t = ty_int; break;
        case ND_VAR: t = check_var(s, n); break;
        case ND_BINOP: t = check_binop(s, n); break;
        case ND_UNARY: t = check_unary(s, n); break;
        default: UNREACHABLE();
    }
    n->type = t;  // ★ コード生成器はこれを見る
    return t;
}
```

**`gen_expr`（コード生成）と対になる構造です。**

| 関数 | 約束 |
|---|---|
| `check_expr(s, n)` | 検査して `n->type` を埋め、**型**を返す |
| `gen_expr(e, n)` | 命令を出力し、**値の置き場所の名前**を返す |

どちらも「AST を再帰的に降りて 1 つの値を返す」形です。
**この対称性を意識すると、両方が書きやすくなります。**

### 📖 二項演算の検査は 2 段構え

```c
static Type *check_binop(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);
    Type *r = check_expr(s, n->rhs);

    // ① 両辺の型が等しいか
    if (!type_equal(l, r)) { /* 型が違うエラー */ }

    // ② その型がその演算子を支持するか
    if (!op_supports(n->op, l)) { /* 演算子が使えないエラー */ }

    return l;  // 算術演算は両辺と同じ型を返す
}
```

**この順序が重要です**（[../spec/type-system.md](../spec/type-system.md) 5.3）。

- ①を先にすると「`int` と `str` は足せません」という**的確なメッセージ**が出せる
- ②を先にすると、両辺の型が違う場合にどちらを報告すべきか迷う

第6章で `bool`、第9章で `str` / `float` が入ると、
`op_supports` に分岐が増えるだけで、この構造は変わりません。

```c
static bool op_supports(OpKind op, Type *t) {
    if (t->kind == TY_INT) {
        return op != OP_TRUEDIV;   // 言語仕様 4.2：int に '/' は使えない
    }
    return false;
}
```

### 📖 型注釈を必須にした効果がここで出る

**型検査器は「推論」を一切していません。「照合」だけです。**

```c
// 宣言された型（注釈から）と、実際の型（式から）を突き合わせるだけ
Type *declared = type_from_name(n->type_name);
Type *actual = check_expr(s, n->rhs);
if (!type_equal(actual, declared)) { /* エラー */ }
```

もし型推論があると、
「`x = []` の要素型は後続の `x.append(1)` から決まる」のような
**制約を集めて後で解く**仕組み（単一化アルゴリズム）が必要になります。

**型注釈を必須にする**という言語仕様の決定
（[../spec/language-spec.md](../spec/language-spec.md) 3.3）が、
型検査器を数百行から数十行に減らしています。

> **言語仕様の単純さは、そのまま実装の単純さになる。**

これは 0.4 節と 3.3 節で「後で効果を実感します」と予告していた点です。

### 📖 エラーメッセージに「関連する位置」を使う

第3章で作った `Diag.related` が本格的に活躍します。

```c
static void check_assign(Sema *s, Node *n) {
    ...
    if (!type_equal(actual, v->type)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = v->decl_tok;          // ★ 宣言の位置を示す
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   v->name, type_name(v->type));
        diag_fail(&d);
    }
}
```

**`VarEntry` に `decl_tok` を持たせておいたのはこのためです。**

再宣言のエラーでも使います。

```
error: 変数 'x' は既に宣言されています
  --> tests/cases/err_redeclare.my:6:1
   |
 6 | x: int = 2
   | ^ ここで再宣言されています
   |
note: 最初の宣言はここです
  --> tests/cases/err_redeclare.my:5:1
   |
 5 | x: int = 1
   | ^
   |
   = ヒント: 既存の変数に代入するなら型注釈を外してください（例: x = 1）
```

**「前の宣言はどこか」を示すのは、第3章で括弧の対応を示したのと同じ発想です。**
シンボルテーブルに位置情報を持たせるという設計判断が、
そのまま診断の質になります。

### ⚠️ 型がまだ 1 つしかないので「型不一致」は起きない

正直に書きます。**第5章では `int` しか型がないので、
「型が一致しません」のエラーを起こす方法がありません。**

```python
x: int = 1      # 型不一致にできない
```

だからこの検査は**第6章（`bool`）で初めてテストできます**。
それでも今書いておく理由は：

1. 構造を先に作ることで、第6章の作業が「型を 1 つ足す」だけになる
2. `type_equal` を通す習慣がコードに定着する

**⚠️ ただし「テストできないコードを書いた」ことは事実です。**
第6章で必ずテストを追加します。
（この章でテストできる検査は 9 種類あります：未定義の名前・未定義の名前への代入・
代入できない式・再宣言・未知の型名・初期化式が無い・予約語を変数名に使用・
最後の文が式でない・`/` の禁止。「型の不一致」だけがテストできません。）

---

## 5.9 コード生成：全部 alloca 方式の本番

**[../design/ir-conventions.md](../design/ir-conventions.md) 第1節で決めた設計が、
ついに動きます。**

### 📖 おさらい：なぜ変数をメモリに置くのか

LLVM IR のレジスタは **SSA**（1 回しか代入できない）です。

```llvm
%x = add i64 1, 0
%x = add i64 2, 0    ; ✗ エラー: %x が 2 回定義されている
```

しかし Mython では再代入できます。

```python
x: int = 1
x = x + 2
```

**変数を `alloca` でスタックに置けば、`store` は何回でもできます。**
`phi` 命令を自分で作る必要がなくなります。

### ✍️ 3 つの命令

```c
// 宣言・代入 → store
case ND_VARDECL: {
    char *val = gen_expr(e, n->rhs);
    sb_printf(&e->body, "  store %s %s, ptr %s\n", llvm_type(n->type), val,
              var_ptr(n->name));
    return NULL;
}

// 読み出し → load
case ND_VAR: {
    char *t = new_tmp(e);
    sb_printf(&e->body, "  %s = load %s, ptr %s\n", t, llvm_type(n->type),
              var_ptr(n->name));
    return t;
}
```

### ✍️ alloca は entry ブロックに集める（規約 R1）

```c
static void collect_allocas(Emitter *e, Node *n) {
    if (!n) return;

    if (n->kind == ND_VARDECL)
        sb_printf(&e->body, "  %s = alloca %s\n", var_ptr(n->name),
                  llvm_type(n->type));

    collect_allocas(e, n->lhs);
    collect_allocas(e, n->rhs);
    for (Node *s = n->body; s; s = s->next) collect_allocas(e, s);
}
```

```c
static void gen_mython_main(Emitter *e, Node *ast) {
    sb_printf(&e->body, "define i64 @mython_main() {\nentry:\n");

    collect_allocas(e, ast);      // ① まず全変数の alloca

    char *last = NULL;            // ② 本体
    for (Node *s = ast->body; s; s = s->next) {
        char *v = gen_stmt(e, s);
        if (v) last = v;
    }
    sb_printf(&e->body, "  ret i64 %s\n", last);
    sb_printf(&e->body, "}\n");
}
```

**🤔 なぜ本体の途中で `alloca` を出さないのか**

途中に書いても動きますが、**ループの中にあると反復ごとにスタックを消費します**。

```llvm
while.body:
  %x = alloca i64        ; ✗ 100 万回ループすると 8MB 消費する
```

entry にまとめるのが LLVM の作法で、`mem2reg` が最適化しやすい形でもあります。
第7章で `while` を実装したときに、この判断が効きます。

**本体を生成する「前」に AST を歩いて全部集める**方式にしたので、
第7章でブロックが入っても再帰が勝手に見つけてくれます。

### 📖 変数の IR 名

```c
static char *var_ptr(const char *name) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%%%s", name);   // x → %x
    return sb_str(&sb);
}
```

**シャドーイングを禁止している**（言語仕様 5.1）ので、
変数名がそのまま一意な IR 名になります。同名の変数が同時に存在しません。

**⚠️ もしシャドーイングを許していたら**、
`%x` `%x.1` `%x.2` のような連番管理が必要でした。
**言語仕様の制限が実装を単純にしている**もう一つの例です。

### ✅ 生成される IR

```bash
$ ./build/mythonc -S tests/cases/var_basic.my | sed -n '/mython_main/,/^}/p'
```

```llvm
define i64 @mython_main() {
entry:
  %x = alloca i64            ; ① 箱を作る
  store i64 1, ptr %x        ; x = 1
  %t0 = load i64, ptr %x     ; x を読む
  %t1 = add i64 %t0, 2       ; x + 2
  store i64 %t1, ptr %x      ; x = (x + 2)   ← 2 回目の store！
  %t2 = load i64, ptr %x     ; 最後の式文 x
  ret i64 %t2
}
```

**`%x` に 2 回 `store` しています。** SSA の制約を回避できています。

### ✅ mem2reg が全部消す

```bash
$ $(brew --prefix llvm)/bin/opt -passes=mem2reg -S /tmp/v.ll
```

```llvm
define i64 @mython_main() {
entry:
  %t1 = add i64 1, 2
  ret i64 %t1
}
```

**`alloca` / `store` / `load` が完全に消えました。**

```bash
$ $(brew --prefix llvm)/bin/opt -O2 -S /tmp/v.ll
```

```llvm
define noundef i64 @mython_main() local_unnamed_addr #0 {
entry:
  ret i64 3
}
```

**設計文書で約束したとおりになりました。**

> **私たちは「全部 alloca」で素朴に書く。LLVM が SSA に直す。**
>
> — [../design/ir-conventions.md](../design/ir-conventions.md) 第1節

支配辺境の計算も `phi` 挿入アルゴリズムも、1 行も書いていません。
**これがコンパイラ理論の難所を回避できた瞬間です。**

---

## 5.10 責務の移動：`/` の検査を sema へ

第2章で「型検査の仕事だが、まだ sema が無いので codegen で弾く」と
書いた検査を、本来の場所に移しました。

### Before（第2章）

```c
// codegen.c
static const char *llvm_binop(Node *n) {
    switch (n->op) {
        ...
        case OP_TRUEDIV:
            error_at_hint(n->tok, "...", "整数の除算に '/' は使えません");
    }
}
```

### After（第5章）

```c
// codegen.c
static const char *llvm_binop(Node *n) {
    switch (n->op) {
        ...
        // OP_TRUEDIV はここに来ません。
        // ★ 第2章ではこの関数で弾いていましたが、第5章で意味解析パスを
        //   作ったので、本来の担当である sema.c へ移しました。
        //   コード生成器は「検査済みの正しい AST」だけを受け取る、
        //   という役割分担がここで確立します。
        default:
            UNREACHABLE();
    }
}
```

```c
// sema.c
static bool op_supports(OpKind op, Type *t) {
    if (t->kind == TY_INT) return op != OP_TRUEDIV;
    return false;
}
```

0 除算の検査も同様に移しました。

### ★ 確立された役割分担

> **コード生成器は「検査済みの正しい AST」だけを受け取る。**
> **不正な入力に出会ったら、それはコンパイラのバグ（`UNREACHABLE()`）。**

この境界が明確になると、コード生成器が**純粋に機械的な変換**になります。
第7章以降でコード生成が複雑になっても、
「ユーザーのミスかもしれない」という心配をしなくて済みます。

**⚠️ 暫定的な置き場所には、必ずコメントで「いつ移すか」を書いておくこと。**
第2章で書いておいたので、この章で回収漏れがありませんでした。

---

## 5.11 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
全 61 件パス
```

15 件追加しました（変数の動作 6 件、エラー 9 件）。
ビルド警告 0 件、ASan/UBSan もクリーンです。

### ✅ 変数の基本

```bash
$ ./build/mythonc tests/cases/var_basic.my -o /tmp/t && /tmp/t; echo $?
3
```

### ✅ 複合代入

```python
x: int = 10
x += 5      # 15
x -= 3      # 12
x *= 4      # 48
x //= 2     # 24
x %= 100    # 24
x
```

→ `24`

**⚠️ このテストを書いたとき、期待値を 14 と書き間違えました。**
テストが `期待 14, 実際 24` と報告してくれたので、
手計算を検算し直して**テストのほうが誤りだった**と分かりました。
**テストが自分の計算ミスを捕まえた**例です。

### ✅ トークン列

```bash
$ ./build/mythonc --dump-tokens tests/cases/var_basic.my
  0  IDENT     2:1    x
  1  PUNCT     2:2    :
  2  IDENT     2:4    int
  3  PUNCT     2:8    =
  4  INT       2:10   1
  5  NEWLINE   2:11   
  6  IDENT     3:1    x
  7  PUNCT     3:3    =
  8  IDENT     3:5    x
  9  PUNCT     3:7    +
 10  INT       3:9    2
 11  NEWLINE   3:10   
 12  IDENT     4:1    x
 13  NEWLINE   4:2    
 14  EOF       4:2    
```

`int` が `IDENT` であることに注目してください。**型名はキーワードではありません。**
だから将来 `int` という名前の変数を作ることも技術的には可能です
（推奨しませんが、言語仕様上は禁止していません）。

### ✅ AST

```bash
$ ./build/mythonc --dump-ast tests/cases/var_use_in_init.my
(block
  (vardecl a int
    (int 3)
  )
  (vardecl b int
    (binop +
      (var a)
      (int 4)
    )
  )
  (var b)
)
```

### ✅ エラー：未定義の変数

```
error: 未定義の名前 'y' です
  --> tests/cases/err_undefined_var.my:5:5
   |
 5 | x + y
   |     ^ この名前は宣言されていません
   |
   = ヒント: 使う前に宣言してください（例: y: int = 0）
```

**ヒントに変数名を埋め込んでいます**（`diag_fmt` の活用）。
汎用的な文言より、具体的なほうが親切です。

### ✅ エラー：自分自身での初期化

```
error: 未定義の名前 'x' です
  --> tests/cases/err_self_init.my:3:10
   |
 3 | x: int = x + 1
   |          ^ この名前は宣言されていません
   |
   = ヒント: 使う前に宣言してください（例: x: int = 0）
```

**5.7 節の「登録の順序」が正しく働いています。**

### ✅ エラー：未知の型名

```
error: 未知の型名 'foo' です
  --> tests/cases/err_unknown_type.my:4:1
   |
 4 | x: foo = 1
   | ^ この型は存在しません
   |
   = ヒント: 現在使える型: int
```

**「現在使える型」を列挙します**（`type_name_list()`）。
第6章で `bool` が増えたら、この一覧も自動的に増えます。

### ✅ エラー：`/` の検査が sema に移っても同じ品質

```
error: 整数の除算に '/' は使えません
  --> tests/cases/err_truediv.my:3:3
   |
 3 | 7 / 2
   |   ^
   |
   = ヒント: 切り捨て除算の '//' を使ってください（Mython には暗黙の型変換がないため、'/' は float 専用です）
```

**責務を移動しても、ユーザーから見た品質は変わっていません。**
リファクタリングの成功条件です（テストが守ってくれました）。

### ✅ エラー：最後の文が式でない

```
error: プログラムの最後は式でなければなりません
  --> tests/cases/err_last_stmt_not_expr.my:4:3
   |
 4 | x = 2
   |   ^ この文は値を持ちません
   |
   = ヒント: 最後に値となる式を書いてください（第8章で `def main() -> int:` と return に置き換わります）
```

**暫定仕様であることをヒントで明示しています。**
「なぜこんな制限があるのか」と「いつ無くなるのか」が分かれば、
利用者（＝自分）は納得できます。

---

## 5.12 まとめと次章の予告

### できたこと

```
✅ src/types.{h,c} 新設（Type、シングルトン、type_equal）
✅ src/sema.{h,c} 新設（3 つ目のパス）
✅ スコープとシンボルテーブル（lookup / lookup_local の使い分け）
✅ 識別子とキーワード（未使用の予約語も先に確保）
✅ 2 トークン先読み（peek_at）— 第1章の配列設計を回収
✅ 変数宣言・代入・式文の判別
✅ 複合代入の脱糖（+= -= *= //= %=）
✅ alloca / store / load — 「全部 alloca 方式」の本番
✅ mem2reg で alloca が消えることを実測確認
✅ '/' と 0 除算の検査を codegen から sema へ移動
✅ テスト 61 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/types.h/c` | **新規**。Type、シングルトン、type_equal、type_from_name |
| `src/sema.h/c` | **新規**。スコープ、シンボルテーブル、型検査 |
| `src/lexer.h/c` | `TK_IDENT` / `TK_KEYWORD`、`read_ident`、キーワード表、`:` `=` `+=` 等 |
| `src/ast.h/c` | `ND_VAR` / `ND_VARDECL` / `ND_ASSIGN`、`type` / `name` / `type_name` |
| `src/parser.c` | `peek_at` 復活、`var_decl`、`simple_stmt`、複合代入の脱糖 |
| `src/codegen.c` | `llvm_type`、`var_ptr`、`gen_stmt`、`collect_allocas`、検査の削除 |
| `src/main.c` | `types_init()` と `sema()` の呼び出し |
| `tests/cases/` | 15 件追加 |

### この章で回収された設計判断

**第1章から積み上げた投資が、まとめて効きました。**

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | トークンを**配列**にした | `peek_at(p, 1)` で 2 トークン先読み |
| ch1 | 全ノードに `Token *tok` | 型エラーの位置を示せる |
| ch1 | `UNREACHABLE()` | 「検査済み AST」の前提を表明する |
| ch3 | `Diag.related`（関連位置） | 「変数の宣言はここ」「前の宣言はここ」 |
| ch3 | `diag_fmt` | 変数名や型名を埋め込んだヒント |
| 設計 | `alloca` 方式（ir-conventions R1） | `phi` を 1 行も書かずに再代入を実現 |
| 設計 | 型注釈を必須（language-spec 3.3） | 型検査器が「照合」だけで済む |
| 設計 | シャドーイング禁止（5.1） | IR 名の連番管理が不要 |
| ch2 | 「後で sema に移す」コメント | 回収漏れなく移動できた |

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 「型が一致しません」の検査がテストできない（型が 1 つしかない） | **第6章** |
| 複合代入の左辺が 2 回評価される（今は観測不能） | 第10章 |
| トップレベルが式文の並び（暫定仕様） | 第8章 |
| `1 // (2 - 2)` が実行時 SIGFPE | 第9章 |
| 真のグローバル変数（`@g.x`）— 今は暗黙 main のローカル | 第8章 |
| `**` 未実装 | 第9章 |

### ✍️ commit する

```bash
git add -A
git commit -m "第5章: 変数と型検査パスの導入"
```

---

## 次章：第6章 bool・比較演算・論理演算

**達成目標**

```python
x: int = 5
flag: bool = x < 10 and not (x == 3)
flag
```

**やること**

| ファイル | 作業 |
|---|---|
| `types.c` | `TY_BOOL` と `ty_bool` を追加（`type_from_name` に "bool"） |
| `lexer.c` | `==` `!=` `<` `<=` `>` `>=` を追加（**最長一致に注意**） |
| `parser.c` | `or_expr` / `and_expr` / `not_expr` / `comparison` を階層の**上**に積む |
| `ast.c` | `ND_BOOL`、`ND_LOGICAL`、比較演算子の `OpKind` |
| `sema.c` | 比較は `bool` を返す / `and` `or` `not` は `bool` のみ |
| `codegen.c` | `icmp`、**`i1` と `i8` の使い分け**、**短絡評価のための基本ブロック** |

**学ぶ中心概念**

- **`i1` と `i8` の使い分け**（規約 R5：メモリは `i8`、レジスタは `i1`、境界で `zext`/`trunc`）
- **短絡評価**（`and` / `or` は右辺を評価しないことがある）
  → **初めて基本ブロックを分岐させます**（規約 R6・R7 の実践）
- 比較演算子は**符号付き**（`icmp slt`、`ult` ではない）
- 型が 2 つになるので、**ついに「型が一致しません」をテストできる**

**⚠️ 予想される落とし穴**

- `<=` を `<` より先に並べる（最長一致）。`==` は `=` より先
- `bool` を `alloca i8` にして、読むとき `trunc`、書くとき `zext`
- 短絡評価で**基本ブロックを終端し忘れる**（規約 R6）
  → 第3章で用意した「終端済みフラグ」の考え方が必要になります
- `and` / `or` の結果を Mython は `bool` に固定する（Python と違い値を返さない）

**予習**：[../design/ir-conventions.md](../design/ir-conventions.md) の
4 節（bool の変換規約）と 6.6 節（短絡評価）。
[../reference/llvm-ir-primer.md](../reference/llvm-ir-primer.md) 第 7 節（条件分岐）も
手を動かしておくと楽になります。

### 🤔 第6章に入る前の練習問題

1. **`collect_allocas` の再帰から `n->rhs` の行を消して** `make test` を走らせ、
   どのテストが落ちるか確認する（変数宣言の初期化式の中に宣言は無いので落ちない。
   では**いつ必要になるか**考えてみる）
2. **`check_vardecl` で `declare()` を `check_expr()` より前に移動**して、
   `x: int = x + 1` が通ってしまうことを確認する（**必ず元に戻す**）
3. **`type_equal` を `a == b` に置き換えて** テストが通ることを確認し、
   なぜ第10章で壊れるのかを考える
4. **`-O0` でコンパイルした実行ファイル**と `-O2` のものでサイズを比べてみる
   （`./build/mythonc -O2 t.my -o t2`）
