# 第13章 モジュールと import

> **この章のゴール**
> `import` で複数ファイルに分割できる。
> **そして「名前空間」と「分割コンパイル」を学ぶ。**
>
> ```bash
> $ cat lexer.my
> class Token:
>     kind: int
>     text: str
>
>     def init(self, kind: int, text: str) -> None:
>         self.kind = kind
>         self.text = text
>
> def make(kind: int, text: str) -> Token:
>     return Token(kind, text)
>
> $ cat main.my
> import lexer
>
> def main() -> int:
>     t: lexer.Token = lexer.make(1, "hello")
>     print(t.text)
>     return 0
>
> $ ./build/mythonc main.my -o main && ./main
> hello
> ```

**この章で初めて「1 つのプログラムが 1 つのファイルではなくなる」ようになります。**

第12章までの `mythonc` は、**1 ファイルを読んで 1 つの `.ll` を出す**だけでした。
`import` を入れると、次の 3 つが同時に増えます。

| 増えるもの | 何を決める必要があるか |
|---|---|
| **ファイルが複数** | どこから探すか／どの順に読むか／循環したらどうするか |
| **名前空間が複数** | `Token` と `lexer.Token` をどう区別するか |
| **`.ll` が複数** | 定義（`define`）と宣言（`declare`）をどう出し分けるか |

**この章の中心にあるのは、たった 1 つの道具です。**

| 道具 | 何を解決するか |
|---|---|
| 名前修飾（マングリング） | 「別のモジュールの同名の関数をどう区別するか」 |

第12章で `Token.show` → `@Token.show` としたのと**まったく同じ手**を、
**もう 1 段深く**適用するだけです。

---

## 目次

- [13.1 何を作るか（と、作らないか）](#131-何を作るかと作らないか)
- [13.2 コンパイラにとってモジュールとは何か](#132-コンパイラにとってモジュールとは何か)
- [13.3 構文：import と修飾名](#133-構文import-と修飾名)
- [13.4 読み込み：モジュール表と依存の DAG](#134-読み込みモジュール表と依存の-dag)
- [13.5 意味解析：名前空間と名前解決の順序](#135-意味解析名前空間と名前解決の順序)
- [13.6 名前修飾がもう 1 段深くなる](#136-名前修飾がもう-1-段深くなる)
- [13.7 コード生成とリンク：define と declare](#137-コード生成とリンクdefine-と-declare)
- [13.8 動作確認](#138-動作確認)
- [13.9 まとめと次章の予告](#139-まとめと次章の予告)

---

## 13.1 何を作るか（と、作らないか）

### 📖 この章で書けるようになるもの

```python
# lexer.my
class Token:
    kind: int
    text: str

MAX_KIND: int = 9              # グローバル変数も公開される

def make(kind: int, text: str) -> Token:
    return Token(kind, text)
```

```python
# main.my
import lexer                    # ★ ファイル名（拡張子を除く）がモジュール名

def main() -> int:
    t: lexer.Token = lexer.make(1, "hi")   # 型注釈も呼び出しも「モジュール名.名前」
    print(t.text)
    print(lexer.MAX_KIND)
    return 0
```

```bash
$ ./build/mythonc main.my -o main
```

**⚠️ コマンドラインに渡すのは「入口のファイル」1 つだけです。**
`import` をたどって必要なファイルを見つけるのはコンパイラの仕事です
（`make` や `Makefile` に依存関係を書かせる C 方式は採りません）。

### 📖 決めごと

| 決めごと | 理由 |
|---|---|
| モジュール名 = **ファイル名から `.my` を除いたもの** | 対応を機械的にする。`import lexer` を見た瞬間に `lexer.my` を読めばよい |
| 探索場所は**入口ファイルのあるディレクトリ**だけ | v1 の割り切り。検索パスの設定は要らない（[13.4](#134-読み込みモジュール表と依存の-dag)） |
| トップレベルの名前は**すべて公開**する | `_` 始まりを非公開にする規約は作らない。**隠す機能はセルフホストに要らない** |
| **循環 import は禁止**（エラーにする） | [self-hosting.md](../design/self-hosting.md) 3.6 で「ファイル構成を DAG に保つ」と決めてある |
| `import` はトップレベルにのみ書ける | 言語仕様 6.3。関数の中に `import` を書けると初期化順序の話が始まる |

### 📖 この章で作らないもの

**`from lexer import Token` は作りません。**

言語仕様 5.12 には載っていますが、この形は
**「その名前がどのファイルから来たのか」がソースを見ても分からなくなります。**

```python
from lexer import Token
from parser import Token      # ★ どちらの Token？
```

`import lexer` だけに絞れば、`lexer.Token` と書いてある場所を読むだけで出どころが分かります。
**4000 行のコンパイラを自分で読むときに効きます**（第16章以降）。

同じ理由で、次のものも作りません。

| 作らないもの | 理由 |
|---|---|
| `import lexer as lx`（別名） | 短くしたいだけの機能。読みやすさは下がる |
| `from x import *` | 何が入ってくるか分からない |
| パッケージ（`a.b.c` のような階層） | ディレクトリ 1 段で足りる。**必要になってから** |
| 再エクスポート（`import` したものを公開する） | 依存関係が見えなくなる |

**★ 「作らない」判断のしかたは第12章と同じです。**
「セルフホストに必要か」「無いと書けないか」で決めます。
短く書けるだけの機能は、全部後回しにできます。

---

## 13.2 コンパイラにとってモジュールとは何か

### 📖 4 つの顔を持つ

```
lexer.my  ─┬─→ ① 1 つのファイル       … main.c が読む
           ├─→ ② 1 つの名前空間        … sema が名前を引く単位
           ├─→ 1 つの AST              … parser が作る木
           └─→ ③ 1 つの LLVM モジュール … codegen が出す .ll、clang がリンクする単位
```

**★ この 4 つが 1 対 1 に対応しているのが、v1 の一番大事な決めごとです。**
「1 ファイル = 1 モジュール = 1 名前空間 = 1 `.ll`」にしておけば、
どの層の話をしているときも、対応物がすぐ分かります。

C は `#include` があるためこの対応が崩れています
（1 つの `.c` が何百ものヘッダを取り込み、モジュール境界が消える）。
**Mython は Python と同じく「ファイル = モジュール」にします。**

### 📖 パイプラインがどう変わるか

第12章まで：

```
read_file → tokenize → parse → sema → codegen → clang
（1 本の直線）
```

第13章から：

```
      ┌─ read_file → tokenize → parse ─┐   ← モジュールごと
入口 →┤   （import をたどって繰り返す） ├→ sema（全モジュール）→ codegen（モジュールごと）→ clang
      └────────────────────────────────┘
```

**⚠️ 意味解析だけは「全モジュールまとめて」です。**
`main.my` が `lexer.Token` を使うには、`lexer` の中身を知っている必要があります。
モジュールを 1 つずつ完全に処理することはできません。

**★ この形は第8章・第12章で 2 度やった「先に全部登録してから、本体を見る」の
3 度目です。** 単位がファイルに変わっただけです。

```
パス 0   すべてのモジュールを読み、構文解析する（依存順に並べる）  ← 第13章で追加
パス 1a  クラス名だけ登録する                                    ← モジュールごと
パス 1b  フィールドとメソッド
パス 1c  関数とグローバル変数
パス 2   本体を検査する
```

---

## 13.3 構文：import と修飾名

### 📖 文法（grammar.md 2・3 節）

```ebnf
top_level  ::= func_def | class_def | import_stmt | global_var | NEWLINE

import_stmt ::= "import" IDENT NEWLINE

(* 型注釈にモジュール修飾が書けるようになる *)
type       ::= [ IDENT "." ] IDENT [ "[" type "]" ]
```

### ✍️ import 文を読む

```c
// import_stmt ::= "import" IDENT NEWLINE
static Node *import_stmt(Parser *p) {
    Token *kw = advance(p);          // "import"
    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "import の後にはモジュール名を書きます（例: import lexer）",
                      "モジュール名が必要です");
    advance(p);
    expect(p, TK_NEWLINE, "import 文の後には改行が必要です");

    Node *n = new_node(ND_IMPORT, kw);
    n->name = name_tok->text;
    return n;
}
```

**★ 新しいノード種別は `ND_IMPORT` の 1 つだけです。**

### 🤔 式の側は、構文解析器を 1 行も変えない

ここが第13章で一番おもしろいところです。

```python
lexer.make(1, "hi")     # ← すでに ND_METHOD として構文解析できる
lexer.MAX_KIND          # ← すでに ND_FIELD として構文解析できる
```

第12章で `postfix()` に入れた分岐（`.` の後が `(` かどうか）が、
**そのまま modulo 修飾名の構文になっています。**

| 書いたもの | parser が作る木 | sema がどう解釈するか |
|---|---|---|
| `t.kind` | `ND_FIELD(lhs=ND_VAR("t"))` | `t` は変数 → **フィールド** |
| `lexer.MAX_KIND` | `ND_FIELD(lhs=ND_VAR("lexer"))` | `lexer` はモジュール → **モジュールのグローバル変数** |
| `t.show()` | `ND_METHOD(lhs=ND_VAR("t"))` | `t` は変数 → **メソッド** |
| `lexer.make(1)` | `ND_METHOD(lhs=ND_VAR("lexer"))` | `lexer` はモジュール → **モジュールの関数** |

**★ 構文が同じで意味が違うものは、構文解析器で区別しません。**
第10章の「`[` はリテラルか添字か」（位置で決まる）、
第12章の「`.` はフィールドかメソッドか」（次が `(` か）に続いて、
**「`.` の左が何か」で決まる 3 つ目の分岐**です。これは名前解決の仕事なので sema に置きます。

### ✍️ 型注釈だけは文法を広げる

```python
t: lexer.Token = ...
xs: list[lexer.Token] = []
```

型注釈には `postfix()` を通らない専用の読み取り（`type_ref`）があるので、
ここだけ 1 段の修飾を許します。

```c
static Node *type_ref(Parser *p, const char *what) {
    Token *t = type_name_token(p, what);

    Node *n = new_node(ND_TYPEREF, t);
    n->name = t->text;

    // ★ 第13章：モジュール修飾 lexer.Token
    if (consume(p, ".")) {
        Token *m = type_name_token(p, "モジュール修飾の後には型名が必要です");
        n->mod = n->name;     // "lexer"
        n->name = m->text;    // "Token"
    }

    Token *open = peek(p);
    if (consume(p, "[")) {
        n->lhs = type_ref(p, "要素型を書いてください（例: list[int]）");
        expect_close(p, "]", open);
    }
    return n;
}
```

**⚠️ 修飾は 1 段だけです。** `a.b.Token` は文法エラーにします。
パッケージを作らないと決めた（13.1）以上、2 段目に意味がありません。

---

## 13.4 読み込み：モジュール表と依存の DAG

### ✍️ モジュール表

`src/module.h`（新しいファイル）：

```c
typedef struct Module Module;
struct Module {
    char *name;        // "lexer"（IR の名前修飾に使う）
    char *path;        // "/path/to/lexer.my"
    char *src;         // 読み込んだソース（診断が行を切り出すのに要る）
    Node *ast;         // 構文解析した結果
    Module **deps;     // import しているモジュール
    int ndeps;
    int state;         // 0=未訪問 1=訪問中 2=完了（循環検出用）
    Module *next;
};
```

**★ `state` の 3 値が循環検出そのものです。**
深さ優先で潜り、**「訪問中」のモジュールに再び入ったら循環**です。

### ✍️ 入口から import をたどる

```c
// 入口ファイルから始めて、import をすべて読み込む。
// 戻り値は「依存が先、依存する側が後」に並んだモジュールのリスト（トポロジカル順）。
static Module *load_module(Loader *ld, const char *name, Token *from) {
    Module *m = find_module(ld, name);
    if (m && m->state == 1) {                 // ★ 訪問中に再訪 → 循環
        Diag d = {0};
        d.message = diag_fmt("循環 import です: %s", cycle_path(ld, name));
        d.primary.tok = from;
        d.primary.label = "この import が循環を作っています";
        d.hint = "モジュールの依存関係は一方通行（DAG）にしてください。"
                 "共通部分を 3 つ目のモジュールに切り出すと解けます";
        diag_fail(&d);
    }
    if (m) return m;                          // 読み込み済み

    m = new_module(ld, name, path_for(ld, name), from);
    m->state = 1;                             // 訪問中
    m->ast = parse(tokenize(m->path, m->src));

    for (Node *d = m->ast->body; d; d = d->next)
        if (d->kind == ND_IMPORT) add_dep(m, load_module(ld, d->name, d->tok));

    m->state = 2;                             // 完了
    append(&ld->order, m);                    // ★ ここで積むと依存が先に並ぶ
    return m;
}
```

**★ 再帰から戻るときに積むと、それだけでトポロジカル順になります。**
別に整列アルゴリズムを書く必要はありません。

### 📖 探索場所は「入口ファイルのあるディレクトリ」

```bash
$ ls
main.my  lexer.my  parser.my
$ ./build/mythonc main.my -o main      # ★ lexer.my も parser.my も自動で読まれる
```

| 決めごと | 内容 |
|---|---|
| 探索場所 | **入口ファイルのあるディレクトリのみ** |
| 見つからないとき | `モジュール 'lexer' が見つかりません` ＋ 探したパスを表示する |
| 自分自身の import | エラー（`import` は自分を含められない） |
| 同じ import が 2 回 | エラー（重複した import） |

**🤔 なぜ検索パス（`-I` のようなもの）を作らないのか**
セルフホストのコンパイラは `selfhost/*.my` の 1 ディレクトリに収まります。
**必要になっていない機能を先に作ると、使われないまま保守だけが残ります。**
第14章で標準ライブラリを入れるときに、初めて「`lib/` も探す」が必要になります。

### ★ 診断は 1 行も変えなくてよい

モジュールが増えると、エラー表示は**どのファイルの何行目か**を出し分ける必要があります。
ふつうなら「ファイルごとのソースを引く表」を作るところです。

**その必要はありません。**

```c
struct Token {
    const char *file;        // ファイル名
    const char *line_start;  // このトークンがある行の先頭
    ...
};
```

第1章から、**全トークンが自分のファイル名とソース上の位置を持っています**。
診断はトークンを受け取って表示するので、
**どのモジュールのトークンが来ても、そのまま正しく表示されます。**

**★ 「位置情報は木ではなくトークンが持つ」という第1章の決めごとが、
12 章ぶんの機能追加を経ても一度も破れていません。**
ソースを解放しない（`tokenize` のコメント）約束も、ここで効いています。

---

## 13.5 意味解析：名前空間と名前解決の順序

### ✍️ モジュールごとのシンボル表

第12章までの `Sema` は、関数表とクラス表を 1 本ずつ持っていました。
これを**モジュールごとに持たせます**。

```c
typedef struct ModuleSyms ModuleSyms;
struct ModuleSyms {
    Module *mod;
    FuncSig *funcs;    // このモジュールのトップレベル関数とメソッド
    Class *classes;    // このモジュールのクラス
    VarEntry *globals; // このモジュールのグローバル変数
    ModuleSyms *next;
};
```

`Sema` は「今どのモジュールを検査中か」と「そのモジュールが import しているもの」を持ちます。

```c
typedef struct {
    ...
    ModuleSyms *cur_mod;    // 今検査中のモジュール
    ModuleSyms *all_mods;   // 全モジュール（修飾名の解決に使う）
} Sema;
```

### 📖 名前解決の順序（この章で一番大事な決めごと）

`foo` という**修飾されていない名前**を見たとき、次の順に探します。

```
① ローカル変数（内側のスコープから外へ）
② 自分のモジュールのグローバル変数
③ 自分のモジュールの関数・クラス
④ 組み込み（print / len / int / str …）
⑤ import したモジュール名          ← 第13章で追加
```

**⚠️ 他のモジュールの中身は、修飾しないと絶対に見えません。**
`import lexer` しても `Token` とは書けず、必ず `lexer.Token` です。

**🤔 なぜ「探しに行かない」のか**
探しに行く言語（C++ の ADL、Python の `import *`）は、
**名前の出どころがソースから読み取れなくなります。**
「修飾しないと見えない」なら、`.` が無い名前は必ず自分のファイルの中にあります。
**読む人にとっての規則が 1 行で言い切れる**ことを優先します。

### ✍️ モジュール名と同じ名前は宣言できない

```python
import lexer

def main() -> int:
    lexer: int = 1      # ★ エラー
```

```
error: 'lexer' は import したモジュールの名前です
   = ヒント: モジュール名と同じ名前の変数は宣言できません（lexer.Token が曖昧になります）
```

**★ 第7章のシャドーイング禁止と同じ判断です。**
`lexer.x` の `lexer` が変数かモジュールかを**読む人が迷う**なら、書けなくします。
コンパイラは「変数優先」と決めれば動きますが、それは**規則を覚える負担**を利用者に押しつけます。

### ✍️ `.` の左を見て 3 つに分岐する

```c
// ND_FIELD / ND_METHOD の共通の入口
static Type *check_dot(Sema *s, Node *n) {
    // ★ 左が「ただの名前」で、それがモジュールなら → 修飾名
    if (n->lhs->kind == ND_VAR && !lookup_var(s, n->lhs->name)) {
        ModuleSyms *m = lookup_import(s, n->lhs->name);
        if (m) return check_qualified(s, n, m);   // lexer.make(...) / lexer.MAX
    }
    // それ以外は第12章までと同じ（フィールド / メソッド）
    ...
}
```

**⚠️ 「変数が無いこと」を先に確かめます。**
第7章のシャドーイング禁止（13.5 の規則）があるので実際には衝突しませんが、
**規則の順序を実装の順序としてもそのまま書いておく**と、後で読んでも迷いません。

### 📖 型注釈の解決

```python
t: lexer.Token = ...
```

`ND_TYPEREF` に `mod` が入っていれば、そのモジュールのクラス表を引きます。

```c
static Type *resolve_type(Sema *s, Node *tr) {
    if (tr->mod) {
        ModuleSyms *m = lookup_import(s, tr->mod);   // import していなければエラー
        Class *c = find_class(m, tr->name);          // 無ければ「'lexer' にクラス 'Tokn' はありません」
        return c->type;
    }
    ...  // 第12章までと同じ
}
```

**★ `type_equal` は第12章で「定義の同一性」で比べるようにしてあります**（12.2 節）。
`lexer.Token` と `parser.Token` は別の `Class *` なので、
**この章では `types.c` を 1 行も触りません。**
第12章で「名前で比べていると第13章で困る」と書いた予告の回収です。

---

## 13.6 名前修飾がもう 1 段深くなる

### 📖 修飾のしかた

| 対象 | 第12章まで | 第13章から |
|---|---|---|
| 関数 | `@add` | `@lexer.add` |
| メソッド | `@Token.show` | `@lexer.Token.show` |
| クラスの型 | `%Token.type` | `%lexer.Token.type` |
| グローバル変数 | `@g.counter` | `@g.lexer.counter` |
| 入口 | `@mython_main` → `@main` | `@main.main` → `@main`（入口モジュールの `main`） |

**★ すべてのモジュールを一様に修飾します。**
「入口のモジュールだけ修飾しない」という特別扱いは作りません。

**🤔 なぜ一様にするのか**
特別扱いを 1 つ作ると、**その特別扱いを知らないコードが必ず出てきます。**
第12章で `mangle(cls, method)` を書いたときと同じで、
「修飾名を作る関数を 1 つ通す」ようにしておけば、どこから呼んでも正しくなります。

```c
// 第12章： "Token" + "show"        → "Token.show"
// 第13章： "lexer" + "Token.show"  → "lexer.Token.show"
static char *mangle(const char *prefix, const char *name);
```

**⚠️ `mython_main` という名前は無くなります。**
第1章から使ってきた「C の `main` と衝突しないための名前」は、
**モジュール修飾がその役目を引き取る**ので不要になります。
（`@main` は C のエントリポイントとして codegen が別に出します。）

### 📖 `.` は IR の識別子に使える

```llvm
define ptr @lexer.make(i64 %kind.arg, ptr %text.arg) { ... }
```

LLVM の識別子は `[-a-zA-Z$._][-a-zA-Z$._0-9]*` なので `.` を含められます。
**利用者が書ける Mython の識別子には `.` が入らない**ので、衝突は原理的に起きません。
第11章の隠し変数（`for.ix.0`）から数えて 3 度目の同じ手口です。

---

## 13.7 コード生成とリンク：define と declare

### 📖 モジュールごとに .ll を出す

```bash
$ ./build/mythonc main.my -o main --keep-ll
$ ls *.ll
main.ll  lexer.ll
$ clang -O0 main.ll lexer.ll build/runtime.o -o main
```

**★ 「1 モジュール = 1 `.ll`」を守ります。**
1 本にまとめてしまうこともできますが、それをやると
**「モジュールとは何か」が IR の層で消えます。**
分けておけば、第16章以降で Mython 版コンパイラを書くときに
`selfhost/lexer.ll` を単体で読んで検証できます。

### ⚠️ LLVM の型定義はモジュールローカル

`main.ll` が `lexer.Token` を使うなら、**`main.ll` にも同じ型定義が要ります。**

```llvm
; main.ll
%lexer.Token.type = type { i64, ptr }    ← lexer.ll と同じ定義を書く
```

型名は `.ll` を `.o` にコンパイルした時点で消えます（残るのはレイアウトだけ）。
**同じ形の定義を両方に書けば、リンクは通ります。**

**★ レイアウトの計算はコンパイラのプロセス内で 1 回だけ**行われ、
`Class *` を共有しているので**2 つの `.ll` が食い違うことはありません**。
これは「1 プロセスで全モジュールを読む」設計の効果です。

### ✍️ 定義（define）と宣言（declare）を出し分ける

| モジュール `main` から見て | 出すもの |
|---|---|
| 自分の関数 | `define ptr @main.f(...) { ... }` |
| import した関数 | `declare ptr @lexer.make(i64, ptr)` |
| 自分のグローバル変数 | `@g.main.n = global i64 0` |
| import したグローバル変数 | `@g.lexer.MAX_KIND = external global i64` |
| import したクラス | 型定義を複製（上記） |

```c
// codegen.c：使った外部シンボルだけ declare する（第9章の declare_rt と同じ仕組み）
static void declare_imported_func(Emitter *e, FuncSig *f) {
    if (already_declared(e, f->ir_name)) return;
    sb_printf(&e->decls, "declare %s %s(%s)\n",
              llvm_type(f->ret), f->ir_name, param_types(f));
}
```

**★ 「使ったものだけ宣言する」は第9章のランタイム宣言と同じ仕組みです。**
`declare` を全部先に出しても動きますが、
**IR を読むときに「このモジュールが本当に何に依存しているか」が見えなくなります。**

### ✍️ clang に並べて渡す

```c
// main.c
sb_printf(&cmd, "clang %s", opt.opt_level);
for (Module *m = mods; m; m = m->next) sb_printf(&cmd, " '%s'", m->ll_path);
sb_printf(&cmd, " '%s' -o '%s'", MYTHON_RUNTIME_O, opt.output);
```

**⚠️ リンク時の重複定義に注意します。**
同じ関数を 2 つの `.ll` が `define` すると、リンカが
`duplicate symbol` で落ちます。**モジュール修飾がその防止そのもの**です。

---

## 13.8 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```
全 280 件パス
```

20 件追加しました（正常系 6・エラー 14）。すべて複数ファイルのケースです。
ビルド警告 0 件、ASan/UBSan も 280 ケースすべてで検出 0 件。

**★ テストの置き方も変わりました。**

```
tests/mods/mod_basic/main.my    ← 入口。期待値のコメントはここに書く
tests/mods/mod_basic/lexer.my   ← import される側
```

`tests/cases/` に置くと、**モジュール側のファイルまで
単体のテストケースとして拾われてしまいます**。
import の探索場所が「入口ファイルのあるディレクトリ」だからです。
**言語の決めごとが、テストの置き方まで決めました。**

### ✅ ゴールのプログラム

```bash
$ ./build/mythonc main.my -o app && ./app
1:hello
9
direct
```

コマンドラインに渡したのは `main.my` だけです。`lexer.my` は
**`import` をたどってコンパイラが自分で見つけています。**

### ✅ 生成される IR（使う側）

```llvm
; app.main.ll
source_filename = "main.my"
%lexer.Token.type = type { i64, ptr }          ← ★ 型定義を複製している
@.str.0 = private unnamed_addr constant [6 x i8] c"hello\00"
@g.lexer.MAX_KIND = external global i64        ← ★ 定義は lexer.ll にある
declare ptr @lexer.make(i64, ptr)              ← ★ 使ったものだけ declare
declare void @lexer.Token.show(ptr)
declare void @lexer.Token.init(ptr, i64, ptr)

define i64 @main.main() {
entry:
  %t0 = call ptr @lexer.make(i64 1, ptr @.str.0)
  ...
  %t2 = load i64, ptr @g.lexer.MAX_KIND
  ...
}

define i32 @main() {                            ← C の入口は入口モジュールだけ
entry:
  %t0 = call i64 @main.main()
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
```

定義する側：

```llvm
; app.lexer.ll
%lexer.Token.type = type { i64, ptr }
@g.lexer.MAX_KIND = global i64 9                ← こちらが実体
define void @lexer.Token.init(ptr %self.arg, i64 %kind.arg, ptr %text.arg) {
define void @lexer.Token.show(ptr %self.arg) {
define ptr @lexer.make(i64 %kind.arg, ptr %text.arg) {
```

**★ `define` と `declare`、`global` と `external global` が
きれいに裏返しになっています。**

### ★ 名前修飾は本当に効いているか（この章の主役）

同名の関数とクラスを持つ 2 つのモジュール：

```python
# a.my                     # b.my
def f() -> int:            def f() -> int:
    return 1                   return 2
class Box:                 class Box:
    v: int                     v: int
                               w: int          ← ★ レイアウトも違う
```

```
1
2
3
```

生成された IR：

```llvm
; app.a.ll            ; app.b.ll
%a.Box.type = type { i64 }      %b.Box.type = type { i64, i64 }
define i64 @a.f() {             define i64 @b.f() {

; app.main.ll ← ★ 両方の型定義が複製され、両方を declare している
%a.Box.type = type { i64 }
%b.Box.type = type { i64, i64 }
declare i64 @a.f()
declare i64 @b.f()
```

リンク後のシンボル：

```bash
$ nm app | grep ' T _'
0000000100000560 T _common.base
0000000100000570 T _left.get
0000000100000580 T _right.get
0000000100000590 T _main.main
00000001000005c0 T _main
```

**★ すべてのシンボルにモジュール名が付いています。**

### ⚠️ 修飾しなかったらどうなるか（実測）

同じ名前の関数を 2 つの `.ll` が定義したらどうなるかを、手書きの IR で確かめました。

```llvm
; x.ll                    ; y.ll
define i64 @f() { ... }   define i64 @f() { ... }
```

```bash
$ clang x.ll y.ll -o dup
duplicate symbol '_f' in:
    /var/folders/.../x-921154.o
```

**名前修飾は「あると便利」ではなく「無いとリンクできない」ものでした。**
第12章のメソッド（同一モジュール内なので LLVM が先に文句を言う）より、
症状がはっきり出ます。

### ★ 型が同じかどうかは「名前」ではなく「定義」で決まる

```python
import a
import b

x: a.Box = b.Box()      # ← 中身が同じでも別の型
```

```
error: 型が一致しません
 5 |     x: a.Box = b.Box()
   |                  ^^^ 型 'Box' の式
note: 変数 'x' は 'Box' 型として宣言されています
   = ヒント: 'b.Box' と 'a.Box' は名前が同じだけの別のクラスです
             （同じ型かどうかは名前ではなく定義で決まります）
```

**★ 第12章の判断 133（`type_equal` はクラスを定義ポインタで比べる）が、
1 章あとに効きました。** あのとき名前で比べていたら、
`a.Box` と `b.Box` が同じ型になり、**レイアウトの違う構造体を
取り違えて実行時に壊れていました。**

⚠️ ただし、エラーメッセージのほうは**手を入れる必要がありました**。
`型 'Box' の式` と `'Box' 型として宣言されています` だけでは、
**利用者にはまったく意味が通じません。**
同名の別クラスのときだけ、修飾名で説明を足すようにしました。

**★ 「正しく弾ける」と「なぜ弾かれたか分かる」は別の仕事です。**

### ✅ 読み込みの順番（トポロジカル順）

`main → mid → base` と依存しているとき：

```bash
$ ./build/mythonc -S main.my
; ── module: base ──
define i64 @base.one() {
; ── module: mid ──
define i64 @mid.two() {
; ── module: main ──
define i64 @main.main() {
```

**依存が先に並んでいます。** 深さ優先で潜り、
**再帰から戻るときにリストへ積んだ**結果です（13.4 節）。整列はしていません。

### ✅ ダイヤモンド依存でも 1 回だけ読む

```
main → left  ┐
      → right├→ common
      → common┘
```

```bash
$ ls *.ll
app.common.ll  app.left.ll  app.main.ll  app.right.ll
```

`common` は 3 か所から import されていますが、**`.ll` は 1 本だけ**です。
「読み込み済みなら、その場で返す」（13.4 節の `find_module`）が効いています。

### ✅ 循環 import は読み込み中に止まる

```python
# main.my            # other.my
import other         import main
```

```
error: 循環 import です: main → other → main
 1 | import main
   | ^^^^^^ この import が循環を作っています
   = ヒント: モジュールの依存関係は一方通行（DAG）にしてください。
             共通部分を 3 つ目のモジュールに切り出すと解けます
```

**★ 3 値の `state`（未訪問 / 訪問中 / 完了）だけで検出できています。**
経路（`main → other → main`）を出せるのは、探索中のスタックを持っているからです。

### ★ 診断は 1 行も変えていない

`import` した側のファイルにエラーがある場合：

```python
# helper.my
def add(a: int, b: int) -> int:
    return a + "x"
```

```
error: 型 'int' と 'str' に演算子 '+' は適用できません
  --> helper.my:2:14
   |
 2 |     return a + "x"
   |              ^ この演算子の両辺の型が違います
```

**ファイル名も行も正しく出ています。`diag.c` は 1 行も変えていません。**
第1章から「全トークンが `file` と `line_start` を持つ」と決めてあったからです。

**★ 12 章ぶんの機能追加を経ても、この設計は一度も破れていません。**

### ✅ 特別扱いが 1 つ減った

第1章から続いていた `@mython_main`（C の `main` と衝突しないための名前）は、
**この章で無くなりました。**

```llvm
define i64 @main.main() { ... }     ← 入口モジュールの main
define i32 @main() {                ← C のラッパ
  %t0 = call i64 @main.main()
```

モジュール修飾が「衝突を避ける」役目を引き取ったからです。
**機能を足したのに、コード生成器の分岐が 1 つ減りました。**

---

## 13.9 まとめと次章の予告

### できたこと

```
✅ import lexer — 入口ファイルから import をたどって全部読む
✅ 依存の DAG（深さ優先＋再帰から戻るときに積む＝トポロジカル順）
✅ 循環 import の検出（3 値の state と探索スタック）
✅ モジュールごとのシンボル表（＝名前空間）。修飾しないと他モジュールは見えない
✅ lexer.make(...) / lexer.Token(...) / lexer.MAX_KIND / t: lexer.Token
✅ 2 段の名前修飾 @lexer.Token.show / %lexer.Token.type / @g.lexer.MAX_KIND
✅ モジュールごとの .ll と、define / declare の出し分け
✅ clang に .ll を並べて渡すリンク（duplicate symbol は修飾が防ぐ）
✅ 同名の別クラスを取り違えない（第12章の type_equal がそのまま効いた）
✅ テスト 280 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/module.h` / `module.c` | **新規**（254 行）。読み込み・依存解決・循環検出 |
| `src/parser.c` | `import_stmt()`、型注釈のモジュール修飾（54 行） |
| `src/sema.c` | `ModuleSyms`（モジュールごとの表）、修飾名の解決、名前修飾（412 行） |
| `src/codegen.c` | モジュール単位の生成、`declare` / `external global`、型定義の複製（215 行） |
| `src/main.c` | パイプラインが「モジュールの列」を回す形になった（100 行） |
| `src/ast.h` | `ND_IMPORT`、`Node.mod_name` / `is_extern`、`Class.ir_name` / `owner` |
| `tests/run_tests.sh` | `tests/mods/<名前>/main.my` をケースとして拾う |
| `tests/mods/` | **新規**。20 ケース |
| `docs/spec/` | 文法（`import_stmt`・修飾つき型）と言語仕様 5.12 を確定 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | 位置情報は AST ではなく**トークン**が持つ | 診断が**無変更**で複数ファイルに対応した |
| ch1 | `tokenize` の src を解放しない | 全モジュールのソースを同時に保持できた |
| ch1 | `@mython_main` という逃げ道 | **役目を終えて消えた**（修飾が引き取った） |
| ch3 | `Diag`（primary / related / hint） | 循環 import の経路表示までそのまま使えた |
| ch7 | シャドーイング禁止という考え方 | 「モジュール名と同じ名前は宣言できない」に流用 |
| ch8 | 2 パス（宣言を先に全部登録） | **3 度目**。今度は「ファイル単位で」 |
| ch9 | `declare_rt`（使ったものだけ宣言する） | そのまま `declare_extern` になった |
| ch10 | `postfix()` のループ構造 | `lexer.make(1)` が**構文解析器の変更ゼロ**で読めた |
| ch12 | `.` の「フィールド / メソッド」分岐 | 3 つ目の意味（モジュール）を**意味解析だけ**で足せた |
| ch12 | `type_equal` は**定義の同一性**で比べる | `a.Box` と `b.Box` が**何もせずに**別の型になった |
| ch12 | `mangle(prefix, name)` | 引数を変えるだけで 2 段の修飾になった |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| **1 ファイル = 1 モジュール = 1 名前空間 = 1 つの .ll** | どの層の話をしていても対応物が 1 つに決まる。C の `#include` はこの対応が壊れている |
| **`from X import Y` を作らない** | 名前の出どころがソースから読めなくなる。4000 行を自分で読むときに効く |
| **他モジュールの名前は必ず修飾する** | 「`.` の無い名前は必ず自分のファイルにある」と 1 行で言い切れる |
| **モジュール名と同じ名前は宣言できない** | `lexer.x` が変数かモジュールか、読む人が迷わないため |
| **全モジュールを一様に修飾する**（入口も） | 特別扱いを 1 つも作らない。結果として `@mython_main` が消えた |
| **モジュールごとに `.ll` を出す** | IR の層でもモジュール境界を保つ。第16章以降で 1 本ずつ検証できる |
| **式の側は parser を変えない** | `.` の意味は名前解決の話。構文解析器に意味の判断を持ち込まない |
| **循環 import はエラー**（v1） | 「名前だけ先に登録」で通すこともできるが、依存が DAG なら設計として読みやすい |
| **探索場所は入口ファイルのディレクトリだけ** | 使われていない機能（検索パス）を先に作らない。第14章で必要になる |

### ⚠️ 予想が外れたこと

**① 診断の作り直しが要ると思っていた。**

13.4 節の草稿には「モジュールが増えると、エラーの行を切り出すのに
ファイルごとのソース表が要る」と書いていました。**要りませんでした。**
`Token` が `file` と `line_start` を持っているので、
**`diag.c` は 1 行も変わっていません。**

**② 「`.` の 3 つの意味」で名前解決の順序に悩むと思っていた。**

第12章末の予告では「名前解決の順序を決める必要がある」と書きましたが、
実際には**「その名前の変数が無ければモジュール」の 1 行**で済みました。
「モジュール名と同じ名前は宣言できない」を先に決めたからです。

**★ 曖昧さは、解決規則を作るより「起こせなくする」ほうが小さく済みます。**

**③ エラーメッセージのほうが手間だった。**

同名の別クラスを弾く実装は**第12章のまま 0 行**でしたが、
「型 'Box' を 'Box' に代入できません」という**意味不明なメッセージ**を
直すのに、専用のヒント（`no_implicit_hint`）を書くことになりました。

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 探索場所が入口ファイルのディレクトリだけ | 第14章（標準ライブラリで `lib/` も探す必要が出る） |
| 変更のないモジュールも毎回コンパイルする（差分ビルドなし） | 未定（stage0 では速度が問題にならない） |
| `--dump-tokens` / `--dump-ast` は入口ファイルだけ | v1 の割り切り（1 ファイルを見る道具） |
| `from X import Y` / 別名 / パッケージ | 採用しない（13.1 節） |
| 循環 import を許す | 採用しない（DAG に保つ） |
| モジュールのグローバル変数を外から書き換えられる | v1 の割り切り（可視性は「全部公開」） |

### ✍️ commit する

```bash
git add -A
git commit -m "第13章: モジュールと import"
```

---

## 次章：第14章 標準ライブラリ

**達成目標**

```python
import strings

def main() -> int:
    print(strings.join(["a", "b"], ","))
    return 0
```

**やること**

| ファイル | 作業 |
|---|---|
| `lib/*.my` | **Mython 自身で書ける部分は Mython で書く** |
| `runtime/runtime.c` | ファイル入出力・`argv` など、C でしか書けないもの |
| `module.c` | 標準ライブラリの探索場所を足す（`lib/`） |
| `parser.c` / `sema.c` | `extern def`（C の関数を直接呼ぶ宣言） |

**★ この章で「言語機能とライブラリの境界線」を引きます。**
第13章で `import` を作ったので、**標準ライブラリを言語に埋め込まずに済みます。**

**⚠️ 予想される落とし穴**

- 探索場所が 2 つになる（入口のディレクトリと `lib/`）。**どちらを先に探すか**
- `extern def` は名前修飾しない（言語仕様 5.11）。
  モジュール修飾を一様にかけた今、**そこだけ例外**になる
- `dict` 相当をどう作るか（線形探索で始めてよい。[self-hosting.md](../design/self-hosting.md) 3.3）
- ライブラリのテストをどう書くか（`tests/mods/` の仕組みがそのまま使えるはず）

### 🤔 第14章に入る前の練習問題

1. `mangle` を**モジュール名を付けない形**に戻して（`sb_printf(&sb, "%s", name)`）、
   `tests/mods/mod_same_name` をコンパイルし、リンカが何と言うか確かめる
   （**必ず元に戻す**）
2. `module.c` の `load()` で **`order` に積む位置を再帰の前**に変えて、
   `mod_chain` の `-S` 出力の順番がどう変わるか見る。なぜ壊れるか説明する
3. `state` の 3 値のうち **1（訪問中）を使うのをやめる**と、
   循環 import がどうなるか予想してから試す
4. `codegen.c` の `class_type()` が型定義を**出さない**ようにして、
   `mod_basic` の `.ll` を `clang` に食わせ、どんなエラーになるか見る
5. **3 つのモジュールに分けた自作プログラム**を書く。
   `main.my` から 2 段先（`main → a → b`）の関数を**修飾なしで**呼ぼうとして、
   どんなエラーになるか確かめる
