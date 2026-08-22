# 第12章 class（構造体とメソッド）

> **この章のゴール**
> `class` にフィールドとメソッドを定義して使える。
> **そして「名前修飾（マングリング）」と「構造体のレイアウト」を学ぶ。**
>
> ```bash
> $ cat t.my
> class Token:
>     kind: int
>     text: str
>
>     def init(self, kind: int, text: str) -> None:
>         self.kind = kind
>         self.text = text
>
>     def show(self) -> None:
>         print(str(self.kind) + ":" + self.text)
>
> def main() -> int:
>     t: Token = Token(1, "hello")
>     t.show()
>     return 0
> $ ./build/mythonc t.my -o t && ./t
> 1:hello
> ```

**この章で初めて「利用者が新しい型を作れる」ようになります。**

第10章までの型は、コンパイラが最初から知っているものばかりでした
（`int` / `bool` / `str` / `list[T]`）。
`class` を入れると、**型の一覧がソースコードによって増えます。**
そのため、これまで「表を引くだけ」だった部分がすべて
「まずクラス表を作り、それから引く」に変わります。

**この章の中心にあるのは、たった 2 つの道具です。**

| 道具 | 何を解決するか |
|---|---|
| `getelementptr` | 「オブジェクトの何バイト目がどのフィールドか」 |
| 名前修飾（マングリング） | 「別のクラスの同名メソッドをどう区別するか」 |

第7章で作った「IR 名の一意化」が、ここで**本物の名前修飾**になります。

---

## 目次

- [12.1 何を作るか（と、作らないか）](#121-何を作るかと作らないか)
- [12.2 型に「クラス」を足す](#122-型にクラスを足す)
- [12.3 構文：class 定義とフィールドアクセス](#123-構文class-定義とフィールドアクセス)
- [12.4 意味解析：クラス表と名前修飾](#124-意味解析クラス表と名前修飾)
- [12.5 コード生成：構造体と getelementptr](#125-コード生成構造体と-getelementptr)
- [12.6 初期化とヌル参照](#126-初期化とヌル参照)
- [12.7 動作確認](#127-動作確認)
- [12.8 まとめと次章の予告](#128-まとめと次章の予告)

---

## 12.1 何を作るか（と、作らないか）

### 📖 この章で書けるようになるもの

```python
class Token:
    kind: int              # フィールド（型注釈は必須）
    text: str

    def init(self, kind: int, text: str) -> None:   # コンストラクタ
        self.kind = kind
        self.text = text

    def is_eof(self) -> bool:                        # メソッド
        return self.kind == 0

def main() -> int:
    t: Token = Token(0, "")     # インスタンス生成（init が呼ばれる）
    print(t.kind)               # フィールドの読み出し
    t.kind = 3                  # フィールドへの代入
    print(t.is_eof())           # メソッド呼び出し
    ts: list[Token] = [t]       # list の要素にもできる
    return 0
```

言語仕様 5.10 のとおりです。ポイントは 4 つ。

| 決めごと | 理由 |
|---|---|
| **フィールドはメソッドより先**にまとめて書く | レイアウトを確定してからメソッドを型検査したいから（文法で強制する） |
| **継承なし** | v1 の方針。仮想関数表が要らなくなり、メソッド呼び出しが普通の関数呼び出しになる |
| **`self` は明示的に書く**（Python と同じ） | 「メソッドは第 1 引数が自分のふつうの関数」と説明できる |
| コンストラクタ名は **`init`**（`__init__` ではない） | ダンダー記法を導入しないため |

### 📖 この章で作らないもの

**`T | None`（nullable 型）は次章以降に送ります。**

言語仕様には `Token | None` が載っていますが、これを入れるには
**フロー依存の型の絞り込み（narrowing）** が必要です。

```python
t: Token | None = find()
if t is not None:
    print(t.kind)     # ← この中でだけ t は Token 型に「絞られる」
```

「`if` の中だけ型が違う」というのは、
**第5章から積み上げてきた「変数の型は 1 つ」という前提を崩す**変更です。
class とまとめて入れると、どちらのバグか分からなくなります。

`T | None` は**セルフホストで AST を表現するときに必ず必要**になるので、
[design/self-hosting.md](../design/self-hosting.md) のチェックリストの担当章を
**第15章（セルフホスト準備）** に移しました。

> **★ 章を分ける判断のしかた**
> 「片方が壊れたときに、どちらの機能のせいか切り分けられるか」で決めます。
> 切り分けられないなら、章を分けます。

### ⚠️ 参照セマンティクス（Python と同じ）

```python
a: Token = Token(1, "x")
b: Token = a          # ★ コピーされない。同じオブジェクトを指す
b.kind = 99
print(a.kind)         # 99
```

インスタンスは**常にヒープ**に置かれ、変数が持つのは**そのアドレス**です
（[design/memory-model.md](../design/memory-model.md) 2 節）。
`str` や `list[T]` と同じ扱いなので、**新しい規則は 1 つも増えません。**

---

## 12.2 型に「クラス」を足す

### ✍️ TypeKind に TY_CLASS を足す

`src/types.h`：

```c
typedef enum {
    TY_INT, TY_BOOL, TY_NONE, TY_STR, TY_LIST,
    TY_CLASS,  // ユーザー定義クラス → ptr（第12章）
} TypeKind;

struct Type {
    TypeKind kind;
    Type *elem;          // list[T] の要素型（第10章）
    char *name;          // class 名（第12章）
    struct Class *cls;   // クラス定義への参照（第12章）
};
```

`struct Class` の中身（フィールドの一覧やサイズ）は `ast.h` で定義します。
型そのものは「どのクラスか」を指すポインタさえ持てば十分なので、
`types.h` には**前方宣言だけ**を置きます。

### ✍️ type_equal は「定義の同一性」で比べる

```c
bool type_equal(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    if (a->kind == TY_LIST) return type_equal(a->elem, b->elem);
    // ★ 第12章：クラスは「同じ定義か」で比べる。名前の一致ではない。
    if (a->kind == TY_CLASS) return a->cls == b->cls;
    return true;
}
```

**🤔 なぜ名前ではなく定義ポインタで比べるのか**
今は 1 ファイルしかないので、同じ名前のクラスは 1 つだけです。
しかし第13章で `import` を入れると、
`lexer.Token` と `parser.Token` が同時に存在しえます。
**名前で比べていると、この 2 つが同じ型になってしまいます。**
定義ポインタで比べておけば、第13章で何も直す必要がありません。

これは第10章の「`list[int]` と `list[str]` を区別するために再帰比較を入れた」
のと**同じ性質の穴**です。`type_equal` を触るのは 3 度目になります。

### 📖 クラスの型は 1 個だけ作る

```c
Type *type_class(char *name, struct Class *cls);
```

`list[T]` は書かれた場所ごとに新しい `Type` を作りましたが、
クラスの `Type` は**クラス定義ごとに 1 個だけ**作ります。
`Token` と書かれた型注釈は、すべて同じ `Type *` を指します。
プリミティブのシングルトンと同じ扱いです。

---

## 12.3 構文：class 定義とフィールドアクセス

### 📖 文法（grammar.md 3 節）

```ebnf
class_def  ::= "class" IDENT ":" NEWLINE INDENT class_body DEDENT
class_body ::= { field_decl } { func_def | NEWLINE }
field_decl ::= IDENT ":" type NEWLINE
```

**★ フィールドがメソッドより先、というのは文法そのものが決めています。**
「実装の都合」を文法に書いておくと、
利用者には**エラーメッセージではなく仕様として**伝わります。

### ✍️ class_def を読む

```c
// class_def ::= "class" IDENT ":" NEWLINE INDENT { field_decl } { func_def } DEDENT
static Node *class_def(Parser *p) {
    Token *kw = advance(p);   // "class"
    Token *name_tok = ... ;   // IDENT

    Node *n = new_node(ND_CLASS, kw);
    n->name = name_tok->text;

    expect_colon(p, "class の宣言");
    expect(p, TK_NEWLINE, ...);
    expect(p, TK_INDENT, ...);

    // ★ メンバは 1 本のリストにまとめる（ND_FIELDDECL と ND_FUNC が混ざる）。
    //   program() がトップレベルで def とグローバル変数を混ぜているのと同じ形。
    bool saw_method = false;
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        if (tok_is_kw(peek(p), "def")) { ... func_def(p, true); saw_method = true; }
        else if (IDENT の次が ":") { ... field_decl(p); }   // saw_method なら診断
        else 診断;
    }
    expect(p, TK_DEDENT, ...);
    return n;
}
```

`block()` を使わないのは、クラス本体が「文の列」ではなく
**「宣言の列」**だからです（トップレベルと同じ性質）。

### ✍️ self は「型注釈のない第 1 引数」

```python
def init(self, kind: int, text: str) -> None:
```

`self` にだけ型注釈がありません。**書かせても意味がない**
（そのクラスに決まっている）ので、構文の側で特別扱いします。

```c
// param ::= IDENT ":" type
// ★ ただしメソッドの第 1 引数だけは "self"（型注釈なし）を許す
static Node *param(Parser *p, bool allow_self) {
    Token *name_tok = ...;
    if (allow_self && strcmp(name_tok->text, "self") == 0 && !tok_is(peek(p), ":"))
        return ND_PARAM{name="self", type_ref=NULL};   // 型は sema が入れる
    ...
}
```

**⚠️ 型注釈が `NULL` の `ND_PARAM` は、この 1 か所からしか作られません。**
第11章の「型注釈のない `ND_VARDECL`」と同じ抜け道です。
利用者が書くふつうの引数は、今までどおり型注釈が必須です。

### ✍️ `.` が 2 つの意味に分かれる

第10章では `.` の後に必ず `(` を要求していました。

```c
if (!consume(p, "("))
    error_at_hint(mopen, "フィールドへのアクセスは第12章で対応します", ...);
```

この予告を回収します。

```c
if (consume(p, ".")) {
    Token *name_tok = ... ;   // メソッド名 or フィールド名
    if (tok_is(peek(p), "(")) {
        ... ND_METHOD ...      // xs.append(1) / t.show()
    } else {
        Node *f = new_node(ND_FIELD, name_tok);   // t.kind
        f->lhs = n;
        f->name = name_tok->text;
        n = f;
    }
    continue;
}
```

**★ `postfix()` のループに 1 個の分岐を足しただけです。**
`t.next.kind` のような連鎖も、ループなので自動的に通ります。

代入先としても使えるようにします（第10章で `xs[i] = v` を足したときと同じ 1 行）。

```c
if (lhs->kind != ND_VAR && lhs->kind != ND_INDEX && lhs->kind != ND_FIELD) { ...診断... }
```

---

## 12.4 意味解析：クラス表と名前修飾

### 📖 パスが 2 段から 3 段になる

第8章で「宣言をぜんぶ登録してから本体を見る」2 パス方式にしました。
クラスが入ると、**登録のほうが 3 段に分かれます。**

```
パス 1a  クラス名と Type だけ登録する      ← 相互参照のため
パス 1b  フィールドとメソッドを解決する      ← 型注釈に他のクラスが書ける
パス 1c  トップレベルの関数・グローバル変数   ← 引数の型にクラスが書ける
パス 2   本体（関数とメソッド）を検査する
```

**🤔 なぜ 1a と 1b を分けるのか**

```python
class Node:
    tok: Token      # ← Token はまだ登録されていない

class Token:
    kind: int
```

**クラスどうしは互いを参照できなければ使い物になりません。**
名前だけ先に登録しておけば、フィールドの型注釈を解決するときに
「まだ読んでいないクラス」も引けます。
関数の前方参照（第8章）と同じ問題を、同じ手で解いています。

### ✍️ クラス表とフィールド

`src/ast.h`：

```c
typedef struct Field Field;
struct Field {
    char *name;
    Type *type;
    int index;    // 構造体の何番目か（getelementptr に渡す）
    int offset;   // 先頭から何バイト目か（説明とデバッグ用）
    Token *tok;
    Field *next;
};

typedef struct Class Class;
struct Class {
    char *name;
    Token *tok;
    Field *fields;   // 宣言順
    int nfields;
    int size;        // インスタンスのバイト数（my_alloc に渡す）
    int align;
    Type *type;      // このクラスの Type（1 個だけ）
    Node *node;      // ND_CLASS（メソッドをたどるため）
    bool has_init;
    Class *next;
};
```

### ✍️ フィールドのオフセットとサイズ

[design/ir-conventions.md](../design/ir-conventions.md) 8.4 節に書いたとおりに実装します。

```c
// アラインメントの切り上げ：offset を align の倍数に丸める
static int align_up(int offset, int align) {
    return (offset + align - 1) / align * align;
}

static void layout_class(Class *c) {
    int offset = 0, max_align = 1, index = 0;
    for (Field *f = c->fields; f; f = f->next) {
        int a = type_align(f->type);       // bool は 1、それ以外は 8
        offset = align_up(offset, a);
        f->offset = offset;
        f->index = index++;
        offset += type_size(f->type);
        if (a > max_align) max_align = a;
    }
    c->align = max_align;
    c->size = align_up(offset, max_align);  // 全体も切り上げる
}
```

```python
class Mixed:
    a: bool     # offset 0,  size 1
    b: int      # offset 8,  size 8   ← 7 バイトのパディング
    c: bool     # offset 16, size 1
                # 全体 24（アラインメント 8 に切り上げ）
```

**🤔 なぜ LLVM に計算させないのか**
`ptrtoint (ptr getelementptr (%C.type, ptr null, i32 1) to i64)` という
定番のイディオムを使えば LLVM に計算させられます。
それでもコンパイラ側で計算するのは、
[memory-model.md](../design/memory-model.md) 5 節で決めたとおり
**「アラインメント規則を自分で理解するため」**です。
**12.7 節で、両方の計算が一致することを実測します。**

**⚠️ フィールドの読み書きに `offset` は使いません。**
`getelementptr` に渡すのは `index`（何番目のフィールドか）で、
バイト数への変換は LLVM がやります。
`offset` は「自分の計算が合っているか」を確認するための値です。

### ✍️ メソッドは「名前を修飾した、ただの関数」

```
Token.init   →  @Token.init(ptr %self.arg, i64 %kind.arg, ptr %text.arg)
Token.show   →  @Token.show(ptr %self.arg)
```

**★ ここが「名前修飾」の実践です。**
名前を `クラス名.メソッド名` にしてしまえば、
**メソッドは第8章で作った関数表にそのまま登録できます。**

```c
// メソッドを FuncSig として登録する（名前は "Token.init"）
static char *mangle(const char *cls, const char *m) {
    StrBuf sb; sb_init(&sb);
    sb_printf(&sb, "%s.%s", cls, m);
    return sb_str(&sb);
}
```

`.` を含む名前は**利用者が書ける識別子と絶対に衝突しません**。
第11章の隠し変数（`for.ix.0`）と同じ手口です。

これで、メソッド呼び出しの型検査は
**「関数呼び出しの検査に `self` を 1 個足すだけ」**になります。

### ✍️ コンストラクタは「名前がクラスだった呼び出し」

`Token(1, "x")` は構文上ただの関数呼び出し（`ND_CALL`）です。
名前解決の段階で分岐します。

```c
static Type *check_call(Sema *s, Node *n) {
    if (is_builtin_name(n->name)) return check_builtin_call(s, n);

    // ★ 名前がクラスなら、これはインスタンス生成
    Class *c = lookup_class(s, n->name);
    if (c) return check_new(s, n, c);
    ...
}
```

`check_new` は `C.init` のシグネチャを引いて、
**第 1 引数（`self`）を飛ばして**残りを検査します。
`init` が無いクラスは引数 0 個でのみ生成できます。

第9章の `n->builtin`（sema が選んだ候補を codegen に渡す）と同じ形で、
`n->cls` に「どのクラスを生成するか」を書き込みます。
**コード生成器は判断せず、記録を読むだけ**という役割分担は変わりません。

### 📖 参照の比較（言語仕様 4.3）

```python
if a == b:      # ★ 参照（アドレス）の比較。中身は見ない
```

`str` は**内容**で比較しましたが、クラスは**参照**で比較します。
比較できるのは `==` と `!=` だけで、`<` などは型エラーにします
（[type-system.md](../spec/type-system.md) 5.6）。

---

## 12.5 コード生成：構造体と getelementptr

### ✍️ 構造体型の定義

```llvm
%Token.type = type { i64, ptr }
```

フィールドの LLVM 型は、**メモリ上の型**（`llvm_mem_type`）を使います。
`bool` が `i8` になるのは第6章で決めた規約 R5 のとおりです。

型定義はモジュールの先頭（`header` バッファ）に出します。
**第1章でバッファを 4 本に分けておいた効果が、ここでも効きます。**

### ✍️ インスタンス生成

```llvm
%t0 = call ptr @my_alloc(i64 16)          ; ← ゼロ初期化される
%t1 = getelementptr %Token.type, ptr %t0, i32 0, i32 1
store ptr @.str.1, ptr %t1                ; str フィールドの既定値 ""（12.6 節）
call void @Token.init(ptr %t0, i64 1, ptr @.str.0)
```

`malloc` ではなく **`my_alloc`** を呼びます（第9章で作ったもの）。
`calloc` なので**必ずゼロ初期化**され、確保に失敗したらランタイムが親切に死にます。
生成する IR に NULL チェックが 1 つも入りません（規約 R10）。

### ✍️ フィールドアクセス

```llvm
%t2 = call ptr @my_check_not_none(ptr %t0)     ; 12.6 節
%t3 = getelementptr %Token.type, ptr %t2, i32 0, i32 0
%t4 = load i64, ptr %t3
```

**⚠️ `getelementptr` の 2 つのインデックス**

```llvm
getelementptr %Token.type, ptr %t0, i32 0, i32 1
              ^^^^^^^^^^^        ^^^^^^  ^^^^^^
              ベースの型          │       │
                                 │       └─ 構造体の何番目のフィールドか
                                 └─ その型の何個目の要素か（配列的な移動）
```

第 1 インデックスは「`Token` の配列とみなして何個目か」です。
**単一のオブジェクトを触るときは常に `0`。**
ここを `1` にすると、隣のオブジェクトがある場所（＝ゴミ）を読みます。

読み書きは第6章の `gen_load` / `gen_store` をそのまま使います。
**`bool` フィールドの `i8` ↔ `i1` 変換は、何も書かずに手に入ります。**

### ✍️ メソッド呼び出し

```llvm
call void @Token.show(ptr %t0)
```

`self` を第 1 引数として渡すだけです。
関数定義側（`gen_func`）は**まったく変更しません**。
`self` は「型がクラスのふつうの引数」なので、
第8章の「引数を alloca にコピーする」（規約 R8）がそのまま働きます。

**★ 名前修飾を sema でやっておくと、codegen は
「`n->ir_name` があればそれを使う」の 1 行で済みます。**

---

## 12.6 初期化とヌル参照

### ⚠️ ゼロ初期化では足りないものがある

`my_alloc`（`calloc`）はメモリを 0 で埋めます。

| フィールドの型 | ゼロの意味 | 安全か |
|---|---|---|
| `int` | `0` | ✅ |
| `bool` | `False` | ✅ |
| `str` | **NULL ポインタ** | ❌ `print` すると壊れる |
| `list[T]` | **NULL ポインタ** | ❌ `append` すると壊れる |
| クラス | **NULL ポインタ** | ❌ 参照すると壊れる |

`init` を書き忘れたときに壊れるのは、**言語として弱い**です。

### ✍️ 判断：`str` と `list` は「空の値」で初期化する

インスタンス生成のときに、コンパイラが既定値を書き込みます。

```llvm
; class Box: n:int / flag:bool / s:str / xs:list[int]
%t0 = call ptr @my_alloc(i64 32)
%t1 = getelementptr %Box.type, ptr %t0, i32 0, i32 2
store ptr @.str.0, ptr %t1                  ; s  → ""（空文字列リテラル）
%t2 = call ptr @my_list_new()
%t3 = getelementptr %Box.type, ptr %t0, i32 0, i32 3
store ptr %t2, ptr %t3                      ; xs → 空のリスト
```

`n`（`int`）と `flag`（`bool`）には**何も書きません**。
`calloc` のゼロがそのまま正しい既定値だからです。

これで **`str` と `list[T]` のフィールドは、`init` が無くても必ず有効な値**になります。
メモリモデルの「未初期化の値は存在しない」（6 節）を、フィールドまで広げた形です。

### ⚠️ クラス型のフィールドだけは NULL のままにするしかない

```python
class Node:
    child: Node        # ← 既定値を作ろうとすると無限再帰する
```

`Node` の既定値を作るには `Node` が要る……という循環になるので、
**クラス型のフィールドだけは NULL から始まります。**

そこで [memory-model.md](../design/memory-model.md) 7 節の決定どおり、
**フィールドアクセスのたびにランタイムで NULL チェック**します。

```c
// runtime/runtime.c
void *my_check_not_none(void *p) {
    if (!p) my_panic("field access on None (uninitialized reference field?)");
    return p;
}
```

**★ 検査を IR ではなくランタイムに置く**のは規約 R10 の実践です。
IR には `call` が 1 行増えるだけで、分岐は 1 つも出ません。

**⚠️ 本来の解決策は `T | None` と narrowing です**（12.1 節）。
このチェックは「クラッシュを親切なメッセージに変える」だけの応急処置で、
第15章で型の側から塞ぎます。

---

## 12.7 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```
全 260 件パス
```

38 件追加しました（正常系 12・実行時エラー 2・コンパイルエラー 24）。
ビルド警告 0 件、ASan/UBSan も 260 ケースすべてで検出 0 件です。

```bash
$ for f in tests/cases/*.my; do ./build/mythonc-asan "$f" -o /tmp/o; done
検出 0 件 / 260 ケース
```

### ✅ ゴールのプログラム

```bash
$ ./build/mythonc t.my -o t && ./t
1:hello
```

### ★ レイアウトの検算（この章の主役 その 1）

12.4 節で「LLVM に計算させず、自分で計算する」と決めました。
**その計算が本当に合っているかを、LLVM 自身に検算させます。**

```python
class Mixed:
    a: bool
    b: int
    c: bool
```

コンパイラが出した IR：

```llvm
%Mixed.type = type { i8, i64, i8 }
  %t0 = call ptr @my_alloc(i64 24)     ← ★ コンパイラの計算した size
```

LLVM 側の計算（`ptrtoint`＋`getelementptr` の定番イディオム）：

```llvm
%Mixed.type = type { i8, i64, i8 }
@.fmt = private constant [35 x i8] c"size=%lld  a=%lld  b=%lld  c=%lld\0A\00"
declare i32 @printf(ptr, ...)
define i32 @main() {
  call i32 (ptr, ...) @printf(ptr @.fmt,
    i64 ptrtoint (ptr getelementptr (%Mixed.type, ptr null, i32 1) to i64),
    i64 ptrtoint (ptr getelementptr (%Mixed.type, ptr null, i32 0, i32 0) to i64),
    i64 ptrtoint (ptr getelementptr (%Mixed.type, ptr null, i32 0, i32 1) to i64),
    i64 ptrtoint (ptr getelementptr (%Mixed.type, ptr null, i32 0, i32 2) to i64))
  ret i32 0
}
```

```bash
$ lli sizeof.ll
size=24  a=0  b=8  c=16
```

| | size | a | b | c |
|---|---|---|---|---|
| `layout_class()` の計算 | 24 | 0 | 8 | 16 |
| LLVM の計算 | 24 | 0 | 8 | 16 |

**一致しました。** `bool` の後に 7 バイトのパディングが入り、
末尾も 8 の倍数に切り上がっている——**12.4 節の図のとおり**です。

`Token`（`int` + `str`）も同じ手順で `size=16 / offset=8`。
`my_alloc(i64 16)` と一致します。

> **★ 「自分で計算する」の答え合わせは、必ず持っておく**
> `ptrtoint` のイディオムは、**答えを知るためだけ**に使えばよいのです。
> 生成する IR で使わなくても、検算の道具としては最高です。

### ★ 名前修飾（この章の主役 その 2）

同名のメソッドを持つ 2 つのクラス（`tests/cases/class_two_classes.my`）：

```python
class Dog:
    def speak(self) -> None:
        print("A")

class Cat:
    def speak(self) -> None:
        print("B")
```

```llvm
define void @Dog.speak(ptr %self.arg) {
define void @Cat.speak(ptr %self.arg) {
...
  call void @Dog.speak(ptr %t2)
  call void @Cat.speak(ptr %t3)
```

```
A
B
```

**`gen_func` には 1 行も足していません。**
sema が `ir_name` に `Dog.speak` と書いた時点で、
コード生成器にとっては**ただの関数**です。

### ✅ 生成される IR（生成・フィールド・メソッド）

```llvm
define void @Token.show(ptr %self.arg) {
entry:
  %self = alloca ptr                              ← 第8章 R8：引数を alloca に
  store ptr %self.arg, ptr %self
  %t0 = load ptr, ptr %self
  %t1 = call ptr @my_check_not_none(ptr %t0)      ← 12.6 節
  %t2 = getelementptr %Token.type, ptr %t1, i32 0, i32 0
  %t3 = load i64, ptr %t2                         ← self.kind
  ...
}

define i64 @mython_main() {
entry:
  %t0 = call ptr @my_alloc(i64 16)                ← calloc なのでゼロ埋め
  %t1 = getelementptr %Token.type, ptr %t0, i32 0, i32 1
  store ptr @.str.1, ptr %t1                      ← str フィールドの既定値 ""
  call void @Token.init(ptr %t0, i64 1, ptr @.str.2)
```

**分岐（`br`）が 1 つも増えていません。** NULL 検査はすべて `call` です（規約 R10）。

### ✅ 既定値：init が無くても str と list は使える

```python
class Box:
    n: int
    flag: bool
    s: str
    xs: list[int]

def main() -> int:
    b: Box = Box()
    print(b.n)
    print(b.flag)
    print(b.s)
    print(len(b.xs))
```

```
0
False

0
```

3 行目が空行なのは `s` が `""` だからです（NULL なら `print` が壊れます）。

```llvm
  %t0 = call ptr @my_alloc(i64 32)
  %t1 = getelementptr %Box.type, ptr %t0, i32 0, i32 2
  store ptr @.str.0, ptr %t1            ← s  → ""
  %t2 = call ptr @my_list_new()
  %t3 = getelementptr %Box.type, ptr %t0, i32 0, i32 3
  store ptr %t2, ptr %t3                ← xs → 空のリスト
```

**`int` と `bool` には何も書いていません。** `calloc` のゼロがそのまま正解だからです。

### ✅ 参照セマンティクス

```python
a: Cell = Cell()
b: Cell = a
b.v = 99
print(a.v)
print(b.v)
```

```
99
99
```

`list` と同じ、`str` と違う挙動です（`str` は不変なので観測できません）。

### ✅ 相互参照するクラス

```python
class Tree:
    root: Leaf      # ← Leaf はまだ読んでいない

class Leaf:
    v: int
```

```
42
```

**パス 1a（名前だけ先に登録）が効いています。**
1 パスで書いていたら「未知の型名 'Leaf' です」で落ちていました。

### ⚠️ self にも NULL チェックが入る（冗長に見えて、実は防波堤）

IR をよく見ると、`self.kind` にも `my_check_not_none` が入っています。
**`self` は NULL になり得ない**ように見えるので、一見むだです。

ところが：

```python
a: N = N()
a.next.show()      # ← a.next は NULL のまま
```

```
runtime error: field access on None (uninitialized reference field?)
```

**メソッド呼び出しのレシーバは、呼び出し側では検査していません。**
NULL の `self` でメソッドに入り、**メソッドの中の `self.v` で捕まっています**。

```llvm
  %t2 = call ptr @my_check_not_none(ptr %t1)   ← a の検査（a は非 NULL）
  %t3 = getelementptr %N.type, ptr %t2, i32 0, i32 0
  %t4 = load ptr, ptr %t3                      ← a.next（NULL）
  call void @N.hi(ptr %t4)                     ← 検査なしで渡している
```

**⚠️ フィールドに触らないメソッドなら、NULL の self のまま最後まで走ります。**
`class_two_classes.my` の `speak` のようなメソッドがそれです。
今は「壊れる前に必ず止まる」だけで、「呼んだ瞬間に止まる」ではありません。

**既知の課題として記録します**（12.8 節）。正しい解決は `T | None` です。

### ✅ エラー診断

**フィールドがメソッドより後ろ**（文法が決めている制約）：

```
error: フィールドはメソッドより前に書いてください
 8 |     x: int
   |     ^ このフィールド宣言がメソッドより後ろにあります
note: 最初のメソッドはここです
 5 |     def f(self) -> int:
   = ヒント: クラス本体は「フィールドを全部 → メソッドを全部」の順です（文法定義 3 節）
```

**制約を伝えるときは「どこと衝突したか」も見せる**——第3章からの約束です。

**メソッド名とフィールド名の衝突**：

```
error: 'x' はフィールドと同じ名前です
   = ヒント: t.f が「フィールド」か「メソッド」か決められなくなるため禁止です
```

**init が無いのに引数を渡した**：

```
error: クラス 'P' には init が無いので引数を渡せません
 6 |     p: P = P(1)
   |            ^ 1 個の引数が渡されています
   = ヒント: 引数を受け取るには init メソッドを定義してください:
             def init(self, ...) -> None:
```

**self の書き忘れ**：

```
error: メソッド 'f' の第 1 引数は self でなければなりません
 4 |     def f(n: int) -> int:
   |           ^ ここに self が必要です
   = ヒント: Mython は self を明示的に書きます（例: def show(self) -> None:）
```

**フィールドを `()` 付きで呼んだ**：

```
error: クラス 'C' にメソッド 'v' はありません
   = ヒント: 'v' はフィールドです。'()' を外してください
```

**★ 「間違いの形」から「言いたかったこと」を推測して、ヒントに書きます。**
クラスは `.` の後ろに 2 種類のものが来るので、この取り違えが必ず起きます。

**クラスに使えない演算子**：

```
error: 型 'C' に演算子 '<' は適用できません
```

`==` と `!=` だけが使えます（参照の比較。[type-system.md](../spec/type-system.md) 5.6）。

---

## 12.8 まとめと次章の予告

### できたこと

```
✅ class 定義（フィールド＋メソッド）と、インスタンス生成 C(...)
✅ フィールドの読み書き t.f / t.f = v、連鎖 a.next.v
✅ メソッド呼び出し t.m(...) — self を第 1 引数に渡すだけ
✅ 名前修飾 @Token.show — メソッドは「名前を修飾したただの関数」
✅ レイアウト計算（offset / size / align）を自前で行い、LLVM と一致を実測
✅ 参照セマンティクス、== / != は参照比較
✅ str / list[T] フィールドの既定値（init 無しでも壊れない）
✅ クラス型フィールドの NULL 検査（ランタイム側。IR に分岐は増えない）
✅ 相互参照するクラス（登録を 3 パスに分割）
✅ テスト 260 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/types.h` / `types.c` | `TY_CLASS`、`type_equal` は**定義の同一性**で比較、`type_size` / `type_align` |
| `src/ast.h` / `ast.c` | `Class` / `Field`、`ND_CLASS` / `ND_FIELD` / `ND_FIELDDECL` |
| `src/parser.c` | `class_def()`、`field_decl()`、`self` の特別扱い、`postfix()` の `.` の分岐 |
| `src/sema.c` | クラス表、3 パス化、`layout_class()`、名前修飾、`check_new()` / `check_field()` |
| `src/codegen.c` | `%C.type` の定義、`getelementptr`、既定値の書き込み、`ir_name` を使うだけ |
| `runtime/runtime.c` | `my_check_not_none()`（13 行） |
| `tests/cases/` | 38 件追加 |

**★ コード生成器の変更は 153 行で、うち大半が「構造体型の定義」と
「`getelementptr` を出す」の 2 つです。** 判断は 1 つも増えていません。

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | バッファを 4 本に分けた（header / global / fn / …） | `%C.type` を先頭に出すのが 1 行 |
| ch3 | `Diag`（primary / related / hint）の 3 点セット | 「定義はここです」が全診断で無償 |
| ch6 | 規約 R5（`bool` はメモリ上 `i8`、値は `i1`） | `bool` フィールドが**無変更**で動いた |
| ch6 | `gen_load` / `gen_store` を型で分けた | フィールドの読み書きがそのまま乗った |
| ch7 | IR 名の一意化 | **本物の名前修飾になった（予告の回収）** |
| ch8 | 2 パス（宣言を先に全部登録） | 3 パスへの拡張が自然にできた |
| ch8 | 規約 R8（引数を alloca にコピー） | `self` が**ふつうの引数**として動いた |
| ch9 | `my_alloc`（calloc＋失敗時 panic） | ゼロ初期化と NULL 検査不要が無償 |
| ch9 | `n->builtin`（sema が選び、codegen は読むだけ） | `n->cls` が同じ形で書けた |
| ch10 | `type_equal` の再帰比較 | `list[Token]` が**無変更**で動いた |
| ch10 | 「フィールドアクセスは第12章で」の予告診断 | 予告どおり回収 |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| `self` は **parser** で必須にする | sema が「第 1 引数は self」と仮定できる。エラー位置も正確になる |
| メソッドは**関数表に修飾名で登録**する | 関数呼び出しの検査をそのまま使える。分岐が増えない |
| レイアウトは**自前で計算**する | アラインメントを理解するため。答え合わせは LLVM にさせた（12.7） |
| フィールドは**メソッドより先**（文法で強制） | 実装の都合を「仕様」として伝えられる。エラーで伝えるより親切 |
| `str` / `list` フィールドに**既定値**を書く | 「未初期化の値は存在しない」をフィールドまで広げた |
| クラス型フィールドは **NULL のまま**＋ランタイム検査 | 既定値を作ると無限再帰する。型で塞ぐのは第15章 |

### ⚠️ 予想が外れたこと

第11章末の予告では「`self` を parser で足すか sema で足すかを決める」と書きました。
**実際には「足す」のではなく「書かせて検査する」になりました。**

Python と同じく `self` を明示的に書かせるなら、
コンパイラ側は**引数を 1 個増やす必要がありません**。
`self` はもともと第 1 引数として書かれていて、**型注釈だけが無い**からです。
sema が型を埋めれば、あとは第8章の関数とまったく同じです。

**★ 「暗黙に足す」より「明示的に書かせて検査する」ほうが、実装は小さくなります。**

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| `T \| None` と narrowing（NULL 検査を型で塞ぐ） | 第15章 |
| NULL レシーバでのメソッド呼び出しが、フィールドに触るまで検出されない | 同上（`T \| None` が入れば消える） |
| `self` への NULL 検査が冗長（毎フィールドアクセスで `call` が 1 個） | 同上 |
| 継承・仮想関数 | v1 では採用しない |
| クラスのフィールドを外から隠す（private） | v1 では採用しない |
| 同名クラスの衝突（`lexer.Token` と `parser.Token`） | 第13章（`type_equal` は既に対応済み） |

### ✍️ commit する

```bash
git add -A
git commit -m "第12章: class（構造体とメソッド）"
```

---

## 次章：第13章 モジュールと import

**達成目標**

```python
# lexer.my
class Token:
    kind: int
    text: str

# main.my
import lexer

def main() -> int:
    t: lexer.Token = lexer.Token()
    return 0
```

**やること**

| ファイル | 作業 |
|---|---|
| `main.c` | 複数ファイルの読み込みと依存順のコンパイル |
| `parser.c` | `import` 文、`mod.name` の解決（`.` の 3 つ目の意味） |
| `sema.c` | モジュールごとの名前空間、可視性 |
| `codegen.c` | モジュール名を含む名前修飾（`@lexer.Token.show`） |

**★ この章で作った名前修飾が、そのまま 1 段深くなります。**

**⚠️ 予想される落とし穴**

- `.` が「フィールド」「メソッド」「モジュール」の 3 つの意味を持つ
  → **名前解決の順序**を決める必要がある
- `type_equal` を**定義の同一性**にしておいたのがここで効くはず（12.2 節）
- 循環 import をどうするか（禁止するか、名前だけ先に登録するか）
  → クラスの相互参照（パス 1a）と同じ問題

### 🤔 第13章に入る前の練習問題

1. `layout_class()` の `align_up` を**外して**（`offset += type_size(...)` だけにして）
   `Mixed` の IR を出し、`lli` の検算と食い違うことを確かめる（**必ず元に戻す**）
2. `getelementptr` の**第 1 インデックスを `0` から `1` に変えて**
   `class_basic.my` を実行し、何が読めるか観察する
3. `my_check_not_none` の中身を `return p;` だけにして `rt_field_none.my` を実行し、
   どんな死に方をするか見る（メッセージのありがたみの確認）
4. `Token` に `bool` フィールドを 1 個足して、`size` が 16 → 24 になることを
   IR の `my_alloc` の引数で確かめる
5. **`class` で連結リストを書く**（`Node { v: int, next: Node }`）。
   末尾を NULL のままにすると、たどる側はどう書くしかないか——
   **`T | None` が要る理由**を自分の言葉で説明する
