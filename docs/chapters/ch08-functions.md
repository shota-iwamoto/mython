# 第8章 関数定義と呼び出し

> **この章のゴール**
> 関数が定義でき、再帰が動く。そして**暫定の足場が 3 つとも消える**。
>
> ```bash
> $ cat t.my
> def fib(n: int) -> int:
>     if n < 2:
>         return n
>     return fib(n - 1) + fib(n - 2)
>
> def main() -> int:
>     print(fib(10))
>     return 0
> $ ./build/mythonc t.my -o t && ./t
> 55
> ```

**この章は「片付けの章」です。**
第1章から積み上げてきた足場を、まとめて本物に置き換えます。

| 足場 | いつ作ったか | この章でどうなるか |
|---|---|---|
| 暗黙の `main`（トップレベルが実行される） | 第1章 | **撤去**。`def main() -> int:` が唯一の入口 |
| プログラムの値＝最後の式 | 第1章 | **撤去**。`main` の `return` が終了コード |
| `print` は文 | 第7章 | **撤去**。組み込み関数になる |

**⚠️ この章は既存のテスト 118 件を全部書き換えます。**
足場を外すとは、そういうことです（14 節で扱います）。

---

## 目次

- [8.1 プログラムの形が変わる](#81-プログラムの形が変わる)
- [8.2 AST とパーサ：def](#82-ast-とパーサdef)
- [8.3 パーサ：関数呼び出し](#83-パーサ関数呼び出し)
- [8.4 None 型の導入](#84-none-型の導入)
- [8.5 意味解析：シグネチャの事前登録](#85-意味解析シグネチャの事前登録)
- [8.6 意味解析：呼び出しの検査](#86-意味解析呼び出しの検査)
- [8.7 意味解析：return と戻り型](#87-意味解析return-と戻り型)
- [8.8 意味解析：全経路で return するか](#88-意味解析全経路で-return-するか)
- [8.9 グローバル変数](#89-グローバル変数)
- [8.10 コード生成：関数](#810-コード生成関数)
- [8.11 コード生成：呼び出しと main ラッパ](#811-コード生成呼び出しと-main-ラッパ)
- [8.12 print が組み込み関数になる](#812-print-が組み込み関数になる)
- [8.13 テスト 118 件の移行](#813-テスト-118-件の移行)
- [8.14 動作確認](#814-動作確認)
- [8.15 まとめと次章の予告](#815-まとめと次章の予告)

---

## 8.1 プログラムの形が変わる

### 📖 トップレベルに実行文は書けない

```ebnf
program ::= { func_def | global_var | NEWLINE } EOF
```

トップレベルに書けるのは**宣言だけ**です（言語仕様 6.3）。

```python
# ✅ 書ける
counter: int = 0            # グローバル変数

def helper(x: int) -> int:
    return x * 2

def main() -> int:
    return helper(21)

# ✗ 書けない
print(1)                    # 実行文
1 + 2                       # 式文
```

**🤔 なぜ禁止するのか**（言語仕様 6.3）

トップレベルに実行文を許すと「**モジュールの初期化順序**」という厄介な問題が生まれます。
`import a` と `import b` のどちらの初期化が先に走るのか、
循環参照したらどうなるのか——Python はこれで実際に苦労しています。

**コンパイル言語では `main` を唯一の入口にするのが単純で予測可能です。**
第13章（モジュール）でこの判断の効果が出ます。

### 📖 これまでのプログラムはこう変わる

```python
# 第7章まで                    # 第8章から
x: int = 1                     def main() -> int:
x = x + 2                          x: int = 1
x                                  x = x + 2
                                   return x
```

**終了コードの決まり方も変わります。**

| | 第7章まで | 第8章から |
|---|---|---|
| プログラムの値 | 最後の式文の値 | `main` の `return` の値 |
| 入口 | ファイルの先頭から順に | `def main()` |

---

## 8.2 AST とパーサ：def

### ✍️ ノードとフィールド

```c
typedef enum {
    ...
    ND_FUNC,    // 関数定義 → name, params, type_name（戻り型）, body
    ND_PARAM,   // 仮引数   → name, type_name
    ND_CALL,    // 呼び出し → name, args
    ND_RETURN,  // return   → lhs（省略なら NULL）
} NodeKind;

struct Node {
    ...
    Node *params;  // ND_FUNC の仮引数リスト（next で連結）
    Node *args;    // ND_CALL の実引数リスト（next で連結）
};
```

**★ 引数も「next で繋いだリスト」にします。**
第4章で文のリストに使った方式と同じで、要素数を先に数える必要がありません。

### ✍️ `func_def`

```c
// func_def ::= "def" IDENT "(" [ param_list ] ")" "->" type ":" block
static Node *func_def(Parser *p) {
    Token *kw = advance(p);  // "def"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "def の後には関数名を書きます（例: def f() -> int:）",
                      "関数名が必要です");
    advance(p);

    Node *n = new_node(ND_FUNC, kw);
    n->name = name_tok->text;

    Token *open = peek(p);
    if (!consume(p, "(")) { /* 「'(' が必要です」 */ }

    // 仮引数
    Node head = {0};
    Node *cur = &head;
    if (!tok_is(peek(p), ")")) {
        for (;;) {
            cur->next = param(p);
            cur = cur->next;
            if (!consume(p, ",")) break;
        }
    }
    expect_close(p, ")", open);
    n->params = head.next;

    // 戻り型は必須（言語仕様 3.3）
    if (!consume(p, "->")) { /* 「'->' と戻り型が必要です」 */ }
    Token *ret_tok = peek(p);
    ...
    n->type_name = ret_tok->text;

    expect_colon(p, "def の宣言");
    n->body = block(p);
    return n;
}
```

**⚠️ `->` は新しい記号です。** 字句解析器の記号表に足します。

```c
static const char *PUNCTS[] = {
    // 3 文字
    "//=",
    // 2 文字
    "//", "**", "<<", ">>", "+=", "-=", "*=", "%=",
    "==", "!=", "<=", ">=",
    "->",                            // ← 追加（第8章）
    // 1 文字
    ..., ",",                        // ← 追加（引数の区切り）
    NULL,
};
```

**⚠️ 最長一致（4 度目）**：`->` は `-` より先。
2 文字の段に入れるだけで自動的に守られます。**段の設計が 4 章にわたって効いています。**

### 📖 戻り型は省略できない

Python では `-> int` を省略できますが、Mython では**必須**です（言語仕様 3.3）。

```python
def f():          # ✗ エラー
def f() -> None:  # ✅ 「値を返さない」と明示する
```

**🤔 なぜ必須にするのか**
第5章で型注釈を必須にしたのと同じ理由です。
省略を許すと**戻り型の推論**が必要になり、再帰関数では
「`fib` の戻り型を知るには `fib` の本体を見る必要があり、
本体を見るには `fib` の戻り型が必要」という循環に陥ります。

**注釈が必須なら、型検査器は「照合」だけで済みます。**

---

## 8.3 パーサ：関数呼び出し

### ✍️ 呼び出しは「後置演算子」

```ebnf
postfix ::= primary { "(" [ arg_list ] ")" }
```

呼び出しは `primary` の**後ろ**に付きます。優先順位は最も強い部類です。

```c
// postfix ::= primary { "(" [ arg_list ] ")" }
//
// ★ ループにしておくと f(1)(2) のような形も構文上は読めます。
//   （第10章で xs[0] や p.f を足すときも、このループに追加するだけ）
static Node *postfix(Parser *p) {
    Node *n = primary(p);

    for (;;) {
        Token *open = peek(p);
        if (!consume(p, "(")) return n;

        // 呼べるのは名前だけ（第一級関数は v1 未対応）
        if (n->kind != ND_VAR) {
            /* 「この式は呼び出せません」 */
        }

        Node *call = new_node(ND_CALL, n->tok);
        call->name = n->name;

        Node head = {0};
        Node *cur = &head;
        if (!tok_is(peek(p), ")")) {
            for (;;) {
                cur->next = expr(p);
                cur = cur->next;
                if (!consume(p, ",")) break;
            }
        }
        expect_close(p, ")", open);
        call->args = head.next;
        n = call;
    }
}
```

`power()` が `primary()` を呼んでいたところを `postfix()` に差し替えます。

```c
// Before（第2章）        After（第8章）
Node *base = primary(p);  Node *base = postfix(p);
```

**★ 階層に 1 段挟むだけです。** 第6章で上に 4 段積んだのと同じ要領で、
今度は下の方に 1 段入れました。**文法を階層化しておくと、どこにでも挿し込めます。**

### ✍️ 式文は「呼び出しだけ」

```python
def main() -> int:
    print(1)      # ✅ 呼び出し
    1 + 2         # ✗ 計算して捨てるだけ。書き間違いの可能性が高い
    return 0
```

型システム 6 節の表に従い、**式文になれるのは呼び出しだけ**にします。

```c
    // 式文になれるのは呼び出しだけ（type-system.md 6 節）
    if (lhs->kind != ND_CALL) {
        Diag d = {0};
        d.message = "この式は文として書けません";
        d.primary.tok = lhs->tok;
        d.primary.label = "計算した値がどこにも使われていません";
        d.hint = "結果を変数に代入するか、関数呼び出しにしてください";
        diag_fail(&d);
    }
```

**★ これは第7章まで「プログラムの値＝最後の式」だったので許されていた形です。**
足場を外したので、本来の厳しさに戻します。

---

## 8.4 None 型の導入

`print(1)` は値を返しません。**「値がない」ことを表す型**が要ります。

```c
typedef enum {
    TY_INT,
    TY_BOOL,
    TY_NONE,  // 値を返さない（LLVM の void）← 第8章
} TypeKind;
```

| Mython | LLVM（値） | LLVM（メモリ） |
|---|---|---|
| `None` | `void` | — |

**⚠️ `None` 型の変数は作れません。** メモリ上の表現がないからです。

```python
x: None = f()     # ✗ エラー
```

`llvm_mem_type(ty_none)` が呼ばれたら、それはコンパイラのバグなので
`UNREACHABLE()` にします。**「起きてはいけないこと」を表明しておくのが第1章からの方針です。**

> **R9. 戻り型が `None` の関数は、本体の最後に `ret void` を必ず出力する。**

---

## 8.5 意味解析：シグネチャの事前登録

### ⚠️ これがこの章の最重要ポイントです

```python
def main() -> int:
    return fib(10)      # ← ここで fib はまだ「見て」いない

def fib(n: int) -> int:
    ...
```

**関数は、定義より前から呼べなければなりません。** 再帰も同じ問題です。

```python
def fib(n: int) -> int:
    return fib(n - 1)   # ← 自分自身。まだ登録が終わっていない
```

### ✍️ 解決：意味解析を 2 パスにする

```c
void sema(Node *ast) {
    Sema s = {0};
    scope_push(&s);   // グローバルスコープ

    // ── パス 1：シグネチャと グローバル変数を全部登録する ──
    // ★ 本体を見る前に登録するので、前方参照も再帰も自然に通ります。
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) declare_func(&s, d);
        else if (d->kind == ND_VARDECL) declare_global(&s, d);
    }

    // ── パス 2：本体を検査する ──
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_FUNC) check_func(&s, d);

    check_main(&s);   // main の有無とシグネチャ
    scope_pop(&s);
}
```

**★ 「先に全部登録してから中身を見る」** — これがコンパイラの定石です。
C 言語がプロトタイプ宣言を要求するのは、この 2 パスを**人間にやらせている**からです。

**Mython では前方宣言が不要**になります。コンパイラが 1 回余計に走査するだけで済むからです。

### ✍️ 関数表

```c
typedef struct FuncSig FuncSig;
struct FuncSig {
    char *name;
    Type *ret;
    Type **params;   // 引数の型の配列
    char **pnames;   // 引数名（エラーメッセージ用）
    int nparams;
    Token *tok;      // 定義位置（「関数はここで定義されています」用）
    FuncSig *next;
};
```

**`tok` を持たせておく**のは第5章の `VarEntry.decl_tok` と同じ理由です。
「引数の個数が違います」というエラーで、**定義側の位置も示せます**。

### ✍️ main の検査

```c
// main は引数なし・戻り型 int でなければならない（言語仕様 6.1）
static void check_main(Sema *s) {
    FuncSig *m = lookup_func(s, "main");
    if (!m) { /* 「main 関数がありません」＋ 書き方のヒント */ }
    if (m->nparams != 0) { /* 「main は引数を取れません」 */ }
    if (m->ret->kind != TY_INT) { /* 「main の戻り型は int でなければなりません」 */ }
}
```

**「main がありません」は、初心者が最初に出会うエラー**になります。
ヒントに**書き方そのもの**を載せます。

```
   = ヒント: プログラムの入口として次を定義してください:
             def main() -> int:
                 return 0
```

---

## 8.6 意味解析：呼び出しの検査

型システム 5.7 節が**検査の順序**まで決めてくれています。

```c
static Type *check_call(Sema *s, Node *n) {
    // print は組み込み（8.12 節）
    if (strcmp(n->name, "print") == 0) return check_print_call(s, n);

    // ① 定義されているか
    FuncSig *f = lookup_func(s, n->name);
    if (!f) { /* 「未定義の関数 'x' です」 */ }

    // ③ 引数の個数
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams) {
        Diag d = {0};
        d.message = diag_fmt("関数 '%s' は %d 個の引数を取りますが、%d 個渡されました",
                             f->name, f->nparams, nargs);
        d.primary.tok = n->tok;
        d.related.tok = f->tok;                    // ★ 定義側も示す
        d.related.label = "この関数はここで定義されています";
        diag_fail(&d);
    }

    // ④ 各引数の型
    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        Type *at = check_expr(s, a);
        if (!type_equal(at, f->params[i])) {
            /* 「第 N 引数: 型 'bool' を 'int' に渡せません」 */
        }
    }
    return f->ret;
}
```

**② の「呼び出し可能か」は、この章では不要です。**
`postfix()` が「呼べるのは名前だけ」と構文で保証しており、
名前は関数表にしかないからです（第一級関数は v1 未対応）。

---

## 8.7 意味解析：return と戻り型

```c
static void check_return(Sema *s, Node *n) {
    Type *want = s->cur_func->ret;    // ★ 今どの関数を検査中か

    if (!n->lhs) {                     // return（値なし）
        if (want->kind != TY_NONE) { /* 「値を返さなければなりません」 */ }
        return;
    }

    Type *got = check_expr(s, n->lhs);
    if (want->kind == TY_NONE) { /* 「None を返す関数は値を返せません」 */ }
    if (!type_equal(got, want)) {
        Diag d = {0};
        d.message = "return の型が戻り型と一致しません";
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = diag_fmt("関数 '%s' の戻り型は '%s' です", ...);
        diag_fail(&d);
    }
}
```

**★ `Sema` に「今どの関数を検査中か」が加わりました。**
第5章で `Sema` 構造体を作ったとき、
「第8章で『今どの関数を検査中か』が加わります」とコメントに書いてありました。**予告どおりです。**

---

## 8.8 意味解析：全経路で return するか

### 📖 これは制御フロー解析の入口です

```python
def f(x: int) -> int:
    if x > 0:
        return 1
    # ← ここに到達したら？ 何を返す？
```

戻り型が `None` 以外の関数は、**すべての経路で `return` しなければなりません**
（型システム 6.1）。

### ✍️ AST の上を歩く

```c
// この文を実行したら、必ず関数から抜けるか？
//
// ⚠️ 保守的に判定します。「実際には到達しない」経路でも return を要求します。
//    コンパイラが人間より賢くなろうとすると必ず破綻します（type-system.md 6.1）。
static bool always_returns(Node *n) {
    if (!n) return false;

    switch (n->kind) {
        case ND_RETURN:
            return true;

        case ND_IF:
            // else が無ければ、条件が偽のとき素通りする
            return n->els && always_returns(n->body) && always_returns(n->els);

        case ND_BLOCK:
            // 1 つでも「必ず抜ける」文があればよい（その後は到達不能）
            for (Node *st = n->body; st; st = st->next)
                if (always_returns(st)) return true;
            return false;

        // while True: は break が無ければ抜けないが、v1 では判定しない
        default:
            return false;
    }
}
```

**⚠️ `while` は常に `false`** です。`while True:` が抜けないことは分かりますが、
`break` の有無まで見る必要があり、v1 では**やらない**と決めました
（型システム 6.1 に明記済み）。

### 📖 codegen の `terminated` と同じ発想

第6章で作った `e->terminated`（現在の基本ブロックが終端済みか）と、
この `always_returns()` は**同じことを別の場所でやっています**。

| | いつ | どこで |
|---|---|---|
| `always_returns()` | 意味解析 | **AST の上**（構造を見る） |
| `e->terminated` | コード生成 | **命令列の上**（出力を見る） |

**なぜ両方要るのか**：前者は**ユーザーに教える**ため、後者は**正しい IR を出す**ためです。
役割が違うので、片方では代用できません。

---

## 8.9 グローバル変数

```python
counter: int = 0

def main() -> int:
    counter = counter + 1
    return counter
```

```llvm
@g.counter = global i64 0
```

**⚠️ 初期化式はコンパイル時定数のみ**（言語仕様 6.2 の v1 制限）。

```c
    // グローバルの初期化式はリテラルだけ（v1 制限）
    if (n->rhs->kind != ND_INT && n->rhs->kind != ND_BOOL) {
        Diag d = {0};
        d.message = "グローバル変数の初期化式は定数でなければなりません";
        d.hint = "計算が必要なら main の中でローカル変数にしてください";
        diag_fail(&d);
    }
```

**🤔 なぜ制限するのか**
`a: int = b + 1` と `b: int = a + 1` を許すと、
「どちらを先に初期化するか」という**初期化順序問題**が起きます。
8.1 節でトップレベルの実行文を禁止したのと同じ理由です。

### ✍️ 名前の付け方

```c
// ローカル : %x, %x.1
// グローバル: @g.x
```

`@g.` を付けるのは、**C のシンボルや `@main` と衝突させない**ためです。
`VarEntry` に `is_global` を持たせ、`var_ptr()` が名前を組み立てます。

**⚠️ グローバルは `alloca` しません。** `collect_allocas()` で読み飛ばします。

---

## 8.10 コード生成：関数

```c
static void gen_func(Emitter *e, Node *n) {
    // 関数ごとに状態をリセットする（第1章から書いてあった規約）
    e->tmp_counter = 0;
    e->label_counter = 0;
    e->terminated = false;
    e->loop = NULL;
    sb_init(&e->allocas);
    sb_init(&e->fn);

    // main はラッパ方式（規約 7 節の方式 A）なので @mython_main として出す
    const char *ir_name = strcmp(n->name, "main") == 0 ? "mython_main" : n->name;

    // ① 引数を alloca にコピーする（規約 R8）
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->allocas, "  %%%s = alloca %s\n", pm->ir_name,
                  llvm_mem_type(pm->type));
        gen_store_named(e, pm->type, diag_fmt("%%%s.arg", pm->name), pm->ir_name);
    }

    // ② ローカル変数の alloca（第5章のまま）
    collect_allocas(e, n->body);

    // ③ 本体
    gen_stmt(e, n->body);

    // ④ 終端されていなければ終端する（規約 R6）
    if (!e->terminated) {
        if (n->type->kind == TY_NONE)
            sb_printf(&e->fn, "  ret void\n");    // 規約 R9
        else
            sb_printf(&e->fn, "  unreachable\n"); // 全経路 return は sema が保証済み
    }

    // ⑤ 組み立て
    sb_printf(&e->body, "define %s @%s(", llvm_type(n->type), ir_name);
    /* 引数リスト: i64 %n.arg, ... */
    sb_printf(&e->body, ") {\nentry:\n%s%s}\n", sb_str(&e->allocas), sb_str(&e->fn));
}
```

### 🤔 なぜ引数を `alloca` にコピーするのか（規約 R8）

`%n.arg` は SSA レジスタなので**代入できません**。

```python
def f(a: int) -> int:
    a = a + 1      # ← 引数に代入できる（Mython の仕様）
    return a
```

引数をローカル変数と同じ「箱」にしてしまえば、
第5章で書いた `store` / `load` がそのまま使えます。
**`mem2reg` がこの余分なコピーを消す**ので、性能上の損もありません。

### ⚠️ 値を返す関数の末尾に `unreachable` が要る

```python
def f(x: int) -> int:
    if x > 0:
        return 1
    else:
        return 2
```

両方の分岐が `return` で終わるので、`if.end.0` ブロックには**誰も来ません**。
しかし `emit_label()` はラベルを出力するので、
**中身が空で終端命令もないブロック**ができてしまいます（規約 R6 違反）。

```llvm
if.end.0:
                    ; ✗ 終端命令がない
}
```

`unreachable` を置いて終端します。

```llvm
if.end.0:
  unreachable       ; ✅ 「ここには来ない」と LLVM に伝える
}
```

**sema が「全経路で return」を保証しているので、これは嘘ではありません。**

---

## 8.11 コード生成：呼び出しと main ラッパ

```c
        case ND_CALL: {
            // 引数を左から順に評価する（言語仕様 4.5）
            ...
            if (n->type->kind == TY_NONE) {
                sb_printf(&e->fn, "  call void @%s(%s)\n", name, args);
                return NULL;      // 値がない
            }
            char *t = new_tmp(e);
            sb_printf(&e->fn, "  %s = call %s @%s(%s)\n", t, llvm_type(n->type),
                      name, args);
            return t;
        }
```

**⚠️ `void` の呼び出しに結果を代入してはいけません。**

```llvm
%t0 = call void @f()    ; ✗ エラー
call void @f()          ; ✅
```

`main` のラッパは第1章から変わりません。

```llvm
define i32 @main() {
entry:
  %t0 = call i64 @mython_main()
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
```

**★ 第1章で「方式 A（ラッパ）」を選んでおいたので、この章での変更はゼロです。**
ユーザーの `main` を他の関数と同じ規則で生成でき、
コード生成器に「main だけ特別」という分岐が入りません。

---

## 8.12 print が組み込み関数になる

第7章の `print` は**文**でした。ここで**組み込み関数**に変わります。

```c
// parser.c から print_stmt() を削除する。
// print( ... ) は postfix() が普通の呼び出しとして読む。
```

**パーサからは特別扱いが消えます。** 残るのは sema と codegen だけです。

```c
// 組み込み関数 print の検査。
// ⚠️ まだ int 専用です（言語仕様では str / bool / float のオーバーロードあり）。
static Type *check_print_call(Sema *s, Node *n) {
    /* 引数はちょうど 1 個 */
    Type *t = check_expr(s, n->args);
    if (t->kind != TY_INT) { /* 「print はまだ 'bool' 型を出力できません」 */ }
    return ty_none;      // ★ 値を返さない
}
```

**★ 型が `None` になったことで、`print(1)` を式文として書ける**ようになりました。
8.3 節で「式文になれるのは呼び出しだけ」と決めたので、辻褄が合います。

**⚠️ ユーザーが `print` という関数を定義したら？**
組み込みを先に見ているので、ユーザー定義が**無視されます**。
これは分かりにくいので、`declare_func` で弾きます。

```
error: 'print' は組み込み関数なので再定義できません
```

---

## 8.13 テスト 118 件の移行

### 📖 足場を外すとはどういうことか

既存のテストは全部この形でした。

```python
# EXIT: 3
x: int = 1
x = x + 2
x
```

トップレベルの実行文が禁止になったので、**全部が構文エラーになります**。

### ✍️ 機械的に書き換える

```python
# EXIT: 3
def main() -> int:
    x: int = 1
    x = x + 2
    return x
```

変換規則は 3 つだけです。

1. 期待値コメント（`# EXIT:` など）はそのまま残す
2. コード行を 4 桁字下げして `def main() -> int:` の中に入れる
3. **最後の式文**を `return <式>` に変える

スクリプトで一括変換し、**差分を目で確認**しました。

### ⚠️ 手で直したもの

| 種類 | 件数 | 対応 |
|---|---|---|
| `# TOKENS:` だけのテスト | 4 | **変更なし**（トークン列しか見ないので、コンパイルされない） |
| **bool を返していたテスト** | 18 | `return <bool 式>` は `main() -> int` に入らない。`if <式>: return 1` / `return 0` に変換 |
| `# TOKENS:` と `# EXIT:` の両方 | 2 | **TOKENS 専用に戻した**（下記） |
| `err_last_stmt_not_expr.my` / `err_last_stmt_while.my` | 2 | **削除**。「最後は式」という規則自体が無くなった |
| `err_empty.my` | 1 | 「空のプログラムです」→「main 関数がありません」 |
| `err_unexpected_indent.my` | 1 | トップレベルの字下げは今も誤り。ヒントの文言を更新 |
| `err_assign_to_literal.my` | 1 | `1 = 2` が `return 1 = 2` に変換されてしまったので手で戻した |
| `err_print_bool.my` | 1 | int 専用の制限は残る。ヒントの文言だけ更新 |
| `multi_stmt.my` | 1 | 「最後の式が値」を確かめるテストだったので、意味のある複数文に書き換え |

### ⚠️ bool を返していたテストの扱い

```python
# 第6章まで                    # 第8章から
True                           def main() -> int:
                                   if True:
                                       return 1
                                   return 0
```

**`main` の戻り型は `int` なので、bool をそのまま返せません。**
第7章までは「最後の式が bool なら `zext` して返す」という**暗黙の変換**を
コード生成器がやっていました。足場を外したので、その特別扱いも消えます。

**★ 暗黙の変換が 1 つ減って、言語として一貫しました。**
（Mython には暗黙の型変換がない、という言語仕様 3.5 の原則どおりになった。）

### ⚠️ TOKENS と EXIT を兼ねていたテスト

`tok_indent_in_paren.my` と `tok_paren_continuation.my` は、
**字句解析器の性質**（括弧の中では改行もインデントも無視する）を見るテストでした。

`def main() -> int:` で包むと、期待するトークン列の先頭に
`KEYWORD IDENT PUNCT PUNCT PUNCT IDENT PUNCT NEWLINE INDENT` が付いてしまい、
**何を確かめたいテストなのか読めなくなります**。

そこで **`# TOKENS:` 専用に戻しました**（コンパイルされないので `main` は不要）。
実行時の振る舞いは `paren_multiline.my` を新設して分けました。

**★ テストは「何を確かめているか」が読めることが第一です。**
機械的な変換を通しただけで済ませず、目で見て分ける必要がありました。

**★ 「テストが 118 件あるから足場を外せない」となったら、そこで負けです。**
足場は**外す前提で作る**もので、外すコストは最初から見込んでおきます。

---

## 8.14 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
全 148 件パス
```

既存 118 件を移行（2 件削除・1 件新設）し、新規に 31 件（正常系 14・エラー 17）
追加しました。ビルド警告 0 件、ASan/UBSan も全ケースでクリーンです。

### ✅ 達成目標：再帰

```python
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def main() -> int:
    print(fib(10))
    return 0
```

```bash
$ ./build/mythonc t.my -o t && ./t
55
```

### ✅ 前方参照と相互再帰

```python
def main() -> int:
    return double(10)      # ← まだ定義されていない関数を呼ぶ

def double(x: int) -> int:
    return x * 2
```

**通ります。** シグネチャを先に全部登録しているからです（8.5 節）。
相互再帰（`is_even` ⇄ `is_odd`）も同様に動きました。

**★ C ならプロトタイプ宣言が要る場面です。**
2 パスにするだけで、利用者から「宣言を書く義務」が消えます。

### ✅ 引数の受け渡し（規約 R8）

```bash
$ ./build/mythonc -S tests/cases/func_params.my
```

```llvm
define i64 @add(i64 %a.arg, i64 %b.arg) {
entry:
  %a = alloca i64            ; ★ 引数もローカル変数として扱う
  %b = alloca i64
  store i64 %a.arg, ptr %a   ; ★ 引数の値を alloca にコピー
  store i64 %b.arg, ptr %b
  %t0 = load i64, ptr %a
  %t1 = load i64, ptr %b
  %t2 = add i64 %t0, %t1
  ret i64 %t2
}
```

**設計文書（規約 7 節）の例と 1 文字も違いません。**

そして `mem2reg` がコピーを消します。

```bash
$ opt -passes=mem2reg -S t.ll
```

```llvm
define i64 @add(i64 %a.arg, i64 %b.arg) {
entry:
  %t2 = add i64 %a.arg, %b.arg
  ret i64 %t2
}
```

**alloca もコピーも全部消えました。** 素朴に書いて LLVM に任せる方針が、
引数でもそのまま効いています。

### ✅ 引数への代入

```python
def inc(a: int) -> int:
    a = a + 1        # ★ SSA レジスタには代入できないが、alloca なら書ける
    return a
```

`inc(10)` → `11`。**規約 R8 でコピーしておいた理由がこれです。**

### ✅ 全経路 return と `unreachable`

```python
def sign(x: int) -> int:
    if x > 0:
        return 2
    else:
        return 1
```

```llvm
define i64 @sign(i64 %x.arg) {
entry:
  ...
  br i1 %t1, label %if.then.0, label %if.else.0
if.then.0:
  ret i64 2
if.else.0:
  ret i64 1
if.end.0:
  unreachable        ; ★ 誰も来ないブロックを終端する（規約 R6）
}
```

**両分岐が `return` するので `if.end.0` には誰も来ません。**
それでもラベルは出力されるので、終端命令が要ります。
`unreachable` は「ここには来ない」と LLVM に伝える命令で、
**sema が「全経路で return」を保証しているので嘘ではありません。**

### ✅ 戻り型 None（規約 R9）

```llvm
define void @show(i64 %x.arg) {
entry:
  %x = alloca i64
  store i64 %x.arg, ptr %x
  %t0 = load i64, ptr %x
  %t1 = call i32 (ptr, ...) @printf(ptr @.fmt.int, i64 %t0)
  ret void          ; ★ 明示的な return が無くても必要
}
```

### ✅ グローバル変数

```python
counter: int = 1

def bump() -> None:
    counter = counter + 1
```

```llvm
@g.counter = global i64 1

define void @bump() {
entry:
  %t0 = load i64, ptr @g.counter
  %t1 = add i64 %t0, 1
  store i64 %t1, ptr @g.counter
  ret void
}
```

**`alloca` していません。** グローバルは最初からメモリ上にあります。
`@g.` を付けているので `@main` や C のシンボルと衝突しません。

### ✅ main のラッパ（第1章から変更なし）

```llvm
define i32 @main() {
entry:
  %t0 = call i64 @mython_main()
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
```

**第1章で「方式 A」を選んでおいたので、この章での変更はゼロでした。**

### ✅ エラー：main がない

```
error: main 関数がありません
   |
   = ヒント: プログラムの入口として次を定義してください:
             def main() -> int:
                 return 0
```

**初心者が最初に出会うエラーなので、書き方そのものをヒントに載せます。**

### ✅ エラー：引数の個数

```
error: 関数 'add' は 2 個の引数を取りますが、3 個渡されました
  --> t.my:6:12
   |
 6 |     return add(1, 2, 3)
   |            ^^^ 呼び出しの引数の個数が違います
   |
note: この関数はここで定義されています
  --> t.my:3:1
   |
 3 | def add(a: int, b: int) -> int:
   | ^^^
```

**`FuncSig` に定義位置を持たせておいたので、両方を示せます**
（第5章の `VarEntry.decl_tok` と同じ発想）。

### ✅ エラー：引数の型

```
error: 関数 'twice' の第 1 引数: 型 'bool' を 'int' に渡せません
   |
note: 引数 'x' は 'int' 型です
```

**引数名まで出します。** 引数が多いとき「第 1 引数」だけでは探しにくいからです。

### ✅ エラー：全経路で return しない

```
error: 関数 'f' は値を返さずに終わる経路があります
  --> t.my:3:1
   |
 3 | def f(x: int) -> int:
   | ^^^ 戻り型は 'int' です
   |
   = ヒント: すべての経路で return してください（if に else が無いと、条件が偽のとき素通りします）
```

**ヒントで「なぜ素通りするのか」を説明します。**
この検査は保守的なので、「絶対に到達しないのに怒られた」人が必ず出ます。

### ✅ エラー：トップレベルの実行文

```
error: トップレベルに実行文は書けません
  --> t.my:2:1
   |
 2 | print(1)
   | ^^^^^ ここに書けるのは def とグローバル変数だけです
   |
   = ヒント: 処理は main の中に書いてください:
             def main() -> int:
                 ...
                 return 0
```

**第7章までのプログラムを書いた人が最初に踏むエラー**なので、
移行のしかたをヒントに書きます。

### ✅ エラー：式文が呼び出しでない

```
error: この式は文として書けません
   |
 4 |     1 + 2
   |     ^ 計算した値がどこにも使われていません
```

### ✅ エラー：グローバルの初期化式が定数でない

```
error: グローバル変数の初期化式は定数でなければなりません
   |
   = ヒント: 計算が必要なら main の中でローカル変数にしてください
```

---

## 8.15 まとめと次章の予告

### できたこと

```
✅ def / 仮引数 / 戻り型 / return
✅ 関数呼び出し（postfix として階層に 1 段挟んだ）
✅ シグネチャの事前登録（2 パス）— 前方参照と再帰が通る
✅ 相互再帰
✅ 引数の alloca コピー（規約 R8）— 引数に代入できる
✅ 全経路 return の検査（制御フロー解析の入口）
✅ 到達不能な合流点に unreachable（規約 R6）
✅ None 型と ret void（規約 R9）
✅ グローバル変数（@g.x、定数初期化のみ）
✅ main の検査（引数なし・戻り型 int）
✅ 暫定の足場を 3 つ撤去（暗黙 main / 末尾の式 / print 文）
✅ 既存テスト 118 件の移行
✅ テスト 148 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/lexer.c` | 記号 `->` と `,` を追加（**それだけ**） |
| `src/types.h/c` | `TY_NONE` / `ty_none` |
| `src/ast.h/c` | `ND_FUNC` / `ND_PARAM` / `ND_CALL` / `ND_RETURN`、`params` / `args` / `is_global`、ダンプ |
| `src/parser.c` | `postfix`（呼び出し）、`func_def` / `param` / `type_name_token`、`return`、新しい `program`、`print_stmt` の**削除** |
| `src/sema.c` | `FuncSig` 表、2 パス化、`check_call` / `check_return` / `always_returns` / `check_main`、グローバル、`ND_PRINT` の**削除** |
| `src/codegen.c` | `gen_func` / `gen_call` / `gen_global`、`ND_RETURN`、`var_ptr` の**削除** |
| `tests/cases/` | 118 件移行、2 件削除、32 件追加 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | `main` はラッパ方式（方式 A） | **変更ゼロ**で本物の関数に対応できた |
| ch1 | `globals` / `decls` バッファ | `@g.x` の置き場所 |
| ch2 | 記号表を長さごとの段に分けた | `->` を足しても `-` が壊れない（**最長一致 4 度目**） |
| ch2 | 文法の階層化 | 呼び出しを `primary` の上に 1 段挟むだけで済んだ |
| ch3 | `Diag.related` | 「この関数はここで定義されています」 |
| ch5 | `Sema` 構造体に「第8章で cur_func が加わる」と書いた | **予告どおり**加わった |
| ch5 | `VarEntry.decl_tok` | `FuncSig.tok` として同じ手を使った |
| ch5 | 型注釈を必須にした | 戻り型必須 → 再帰関数の型推論の循環を回避 |
| ch6 | `alloca` 方式 | 引数のコピーも `mem2reg` が消してくれる |
| ch7 | `emit_label` / `terminated` | `unreachable` の判定にそのまま使えた |
| ch7 | IR 名を sema が割り当てる | 関数ごとに振り直すだけで、関数をまたぐ衝突が起きない |

### ★ 足場を外すということ

第1章から 3 つの足場を使ってきました。

```
第1章  暗黙の main（トップレベルが実行される）
第1章  プログラムの値＝最後の式
第7章  print は文
```

**足場は「外す前提」で作り、外すコストを最初から見込んでおく。**
今回はテスト 118 件の書き換えが必要でしたが、
**機械的に変換できる形**（期待値はヘッダコメント、コードは本文）で
テストを書いてあったので、大半はスクリプトで済みました。

**⚠️ ただし機械変換だけでは足りませんでした。**
`bool` を返すテスト 18 件と、字句解析器のテスト 2 件は、
**何を確かめるテストなのかを考えて手で直す**必要がありました。

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| `print` が int 専用 | 第9章 |
| 到達不能コードの**警告**が出せない（`diag.c` は「出したら終了」しかない） | 第9章 |
| グローバルの初期化式が定数のみ | v1 の仕様（変更しない） |
| 関数を値として渡せない（第一級関数） | v1 未対応 |
| ネストした関数定義 | v1 未対応 |
| 再帰の深さ（スタックオーバーフローを検出しない） | 未対応（既知の制限） |
| `1 // (2 - 2)` が実行時 SIGFPE | 第9章 |
| `**` 未実装 | 第9章 |

### ✍️ commit する

```bash
git add -A
git commit -m "第8章: 関数定義と呼び出し"
```

---

## 次章：第9章 文字列と C ランタイム連携

**達成目標** — `print("hello")` / `"a" + "b"` / `len(s)` が動く。
そして **FizzBuzz が本物になります**。

```python
def main() -> int:
    i: int = 1
    while i <= 15:
        if i % 15 == 0:
            print("FizzBuzz")      # ← 第7章では print(-15) だった
        elif i % 3 == 0:
            print("Fizz")
        ...
```

**やること**

| ファイル | 作業 |
|---|---|
| `lexer.c` | 文字列リテラルとエスケープ（`\n` `\t` `\\` `\"`） |
| `types.c` | `TY_STR`（値は `ptr`） |
| `codegen.c` | グローバル定数の出力（`@.str.N`）、`print` のオーバーロード |
| `runtime/runtime.c` | **新設**。`my_str_concat` / `my_str_len` / `my_print_str` |
| `sema.c` | `str` の `+`（連結）と `==`（内容比較）、`len()` |
| `Makefile` | ランタイムをビルドしてリンクする |

**★ 初めて「コンパイラが出す IR」以外のコードをリンクします。**
`malloc` を使うので、[design/memory-model.md](../design/memory-model.md) の
出番でもあります。

**⚠️ 予想される落とし穴**

- 文字列リテラルの長さは**エスケープ解決後**のバイト数（`[6 x i8]` の 6 を間違えない）
- `\00` の分を忘れない（第7章の `@.fmt.int` で 1 度踏みかけた）
- `str` の `==` は**ポインタ比較ではなく内容比較**（言語仕様 4.3）
- ランタイム関数は `declare` が必要。`decls` バッファに出す
- リンクの順序（`clang t.ll runtime.o -o t`）

### 🤔 第9章に入る前の練習問題

1. **`always_returns()` の `ND_IF` の判定から `n->els &&` を消して**
   `make test` を走らせ、どのテストが落ちるか確認する（**必ず元に戻す**）
2. **`gen_func` の `unreachable` を消して**、`func_all_paths_return.my` で
   LLVM が何と言うか確かめる
3. **シグネチャの事前登録（パス 1）を消して**、`func_forward_ref.my` の
   エラーメッセージを読む
4. 引数の `alloca` コピー（規約 R8）を消して `%n.arg` を直接使うようにしてみる。
   **「代入しているテスト（`func_arg_assign.my`）だけが落ちる」と予想しがちですが、
   実際には引数を持つ関数がほぼ全滅します。** なぜか考えてみてください
   （ヒント：`load` は何から読もうとしているか）
5. `fib(30)` の実行時間を `-O0` と `-O2` で比べてみる
