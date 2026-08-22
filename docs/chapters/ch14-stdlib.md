# 第14章 標準ライブラリ

> **この章のゴール**
> ファイル入出力・文字列操作・`dict` 相当が使える。
> **そして「言語機能とライブラリの境界線」を学ぶ。**
>
> ```bash
> $ cat t.my
> import io
> import strings
>
> def main() -> int:
>     text: str = io.read_file("hello.txt")
>     parts: list[str] = strings.split(strings.strip(text), ",")
>     print(strings.join(parts, " / "))
>     return 0
> $ printf 'a,b,c\n' > hello.txt
> $ ./build/mythonc t.my -o t && ./t
> a / b / c
> ```

**この章で書くコードの大半は、C ではなく Mython です。**

第13章までは、機能を足すたびにコンパイラ（`src/*.c`）が育ちました。
この章で育つのは `lib/*.my`——**Mython 自身で書かれた標準ライブラリ**です。

| 層 | 書く言語 | この章で足すもの |
|---|---|---|
| 言語（コンパイラが知っている） | C | `extern def` **だけ** |
| ランタイム | C | ファイル入出力・`argv`・`system` |
| **標準ライブラリ** | **Mython** | `strings` / `io` / `sys` / `dict` |

**★ 第13章で `import` を作ったので、標準ライブラリを言語に埋め込まずに済みます。**
`strings.split` は構文でも組み込み関数でもなく、**ただの Mython の関数**です。

---

## 目次

- [14.1 境界線をどこに引くか](#141-境界線をどこに引くか)
- [14.2 extern def — C の関数を呼ぶ](#142-extern-def--c-の関数を呼ぶ)
- [14.3 ランタイムに足すもの](#143-ランタイムに足すもの)
- [14.4 lib/ を探す（探索場所が 2 つになる）](#144-lib-を探す探索場所が-2-つになる)
- [14.5 Mython で書く標準ライブラリ](#145-mython-で書く標準ライブラリ)
- [14.6 dict をジェネリクス無しで作る](#146-dict-をジェネリクス無しで作る)
- [14.7 動作確認](#147-動作確認)
- [14.8 まとめと次章の予告](#148-まとめと次章の予告)

---

## 14.1 境界線をどこに引くか

### 📖 3 つの層と、振り分けの規則

新しい機能を作るとき、置き場所は 3 つあります。

```
① 言語        … 構文・型・演算子・組み込み関数        （src/*.c）
② ランタイム  … OS を触る / 型に依存しない基礎処理    （runtime/runtime.c）
③ ライブラリ  … 上の 2 つの組み合わせで書けるもの     （lib/*.my）
```

**振り分けの規則は 1 つだけです。**

> **★ Mython で書けるものは Mython で書く。**
> 書けないものだけ ② に落とし、②でも書けないものだけ ① に足す。

| 機能 | 置き場所 | なぜ |
|---|---|---|
| `strings.split` | ③ ライブラリ | `len` と `s[i]` と `+` で書ける |
| `strings.substr` | ③ ライブラリ | 同上（**速くはない**。14.7 節で実測する） |
| `io.read_file` | ② → ③ | `fopen` が要る。C の関数を薄く包む |
| `sys.argv` | ② → ③ | `main(argc, argv)` を捕まえる必要がある |
| `dict` | ③ ライブラリ | class と `list[T]` で書ける（14.6 節） |
| `extern def` | ① 言語 | ② を呼ぶ**入口**が無いと何も始まらない |

**🤔 なぜ `split` を組み込み関数にしないのか**

組み込みにすれば速く、短く書けます。それでもライブラリに置くのは、
**コンパイラが知っている概念を増やさないため**です。

第9章で `print` / `len` / `str` を組み込みにしたときは、
`import` がまだ無かったので**ほかに置き場所がありませんでした**。
第13章で `import` を作った今、置き場所ができました。

**★ 言語に入れた機能は二度と減りません。ライブラリなら差し替えられます。**

### 📖 この章で作るもの

```python
# lib/strings.my — 文字列操作（すべて Mython で書く）
substr / find / contains / startswith / endswith / split / join / strip / repeat

# lib/io.my — ファイル入出力（C ランタイムを包む）
read_file / write_file / exists

# lib/sys.my — プロセス（C ランタイムを包む）
argv / run

# lib/dict.my — 文字列キーの表（class と list[T] で書く）
Dict.set / get / has / remove / len / keys
```

### 📖 この章で作らないもの

| 作らないもの | 理由 |
|---|---|
| ジェネリックな `dict[K, V]` | 利用者定義のジェネリクスは v1 で採用しない（[self-hosting.md](../design/self-hosting.md) 3.7）。14.6 節で代わりの形を示す |
| ハッシュ表 | まず線形探索で正しく動かす。**速度は第20章の後に測って直す** |
| 例外・`try` | エラーは `panic` で即終了（方針どおり） |
| パッケージ（`std.strings`） | モジュール 1 段で足りる（第13章 13.1） |
| `float` の数学関数 | セルフホストに要らない |

---

## 14.2 extern def — C の関数を呼ぶ

### 📖 文法（言語仕様 5.11）

```ebnf
extern_def ::= "extern" "def" IDENT "(" [ param_list ] ")" "->" type NEWLINE
```

```python
extern def my_read_file(path: str) -> str
extern def my_system(cmd: str) -> int
```

- **本体を持たない**（`:` もブロックも書かない）
- リンク時に C 側の実体に解決される
- **名前修飾をしない**

### ⚠️ 「名前修飾をしない」は、この章で唯一の例外

第13章の判断 147 で「**全モジュールを一様に修飾する**」と決めました。
`extern` はそれを破ります。

```llvm
declare ptr @my_read_file(ptr)     ← @io.my_read_file ではない
```

**🤔 なぜ例外を作ってよいのか**

修飾する目的は「Mython 側の名前どうしが衝突しないこと」でした。
`extern` が指すのは**すでに C 側で名前が決まっているシンボル**です。
こちらの都合で改名したら、リンクできません。

**★ 例外は「規則の目的に照らして、そこでは目的が成立しない」ときだけ作ります。**
「面倒だから」で作った例外は、必ずあとで壊れます。

### ✍️ 構文解析：本体を読まないだけ

```c
// extern_def ::= "extern" "def" IDENT "(" [param_list] ")" "->" type NEWLINE
static Node *extern_def(Parser *p) {
    Token *kw = advance(p);            // "extern"
    ...                                // "def" IDENT "(" params ")" "->" type
    expect_newline(p);

    Node *n = new_node(ND_FUNC, name_tok);
    n->name = name_tok->text;
    n->params = ...;
    n->type_ref = ...;
    n->body = NULL;                    // ★ 本体が無いことが extern の印
    return n;
}
```

**★ 新しいノード種別は作りません。** `ND_FUNC` の `body` が `NULL`
——これが「宣言だけで定義がない」という意味そのものです。

### ⚠️ extern で使える型は狭い

| 型 | extern で使えるか | LLVM 上の表現 |
|---|---|---|
| `int` | ✅ | `i64` |
| `str` | ✅ | `ptr`（NUL 終端の C 文字列） |
| `list[T]` | ✅ | `ptr`（ランタイムの `MyList *`） |
| クラス | ✅ | `ptr` |
| `None`（戻り値） | ✅ | `void` |
| **`bool`** | ❌ | `i1`。C の `_Bool` との ABI が環境依存 |

```
error: extern の引数と戻り値に bool は使えません
   = ヒント: int で受け取り、Mython 側で n == 1 と書いてください
```

**★ 境界は狭く保ちます。** 通す型が増えるほど「C と Mython で表現が違う」
という事故の面積が増えます。`bool` は 1 か所で `== 1` と書けば済みます。

---

## 14.3 ランタイムに足すもの

### ✍️ ファイル入出力

```c
// runtime/runtime.c
char *my_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { ... my_panic("cannot open file: ..."); }
    // 全部読んで NUL 終端した文字列を返す（my_alloc なので解放しない）
}

void my_write_file(const char *path, const char *text) { ... }

long long my_file_exists(const char *path) { ... }   // ★ bool ではなく int
```

**⚠️ 失敗したら `my_panic` で落とします。**
エラー値を返して利用者に検査させる形（`if fp is None`）は、
**`T | None` がまだ無い**ので書けません（第15章）。
「開けなければ即終了」は乱暴ですが、コンパイラという用途では十分です。

### ✍️ argv：C の main を 2 引数にする

```llvm
define i32 @main(i32 %argc, ptr %argv) {     ← ★ 引数を取るようになった
entry:
  %t0 = sext i32 %argc to i64
  call void @my_set_args(i64 %t0, ptr %argv)  ← ランタイムに預ける
  %t1 = call i64 @main.main()
  %t2 = trunc i64 %t1 to i32
  ret i32 %t2
}
```

```c
static long long g_argc;
static char **g_argv;

void my_set_args(long long argc, char **argv) { g_argc = argc; g_argv = argv; }

MyList *my_argv(void) {          // list[str] を組み立てて返す
    MyList *l = my_list_new();
    for (long long i = 0; i < g_argc; i++) my_list_push_ptr(l, g_argv[i]);
    return l;
}
```

**★ `@main` のラッパを持っていたおかげで、AST もコード生成の本体も
1 行も変わりません。** 第1章で「`main` は特別扱いせずラッパにする」と
決めた効果が、13 章あとに出ました。

### ✍️ 外部コマンドの実行

```c
long long my_system(const char *cmd) { return (long long)system(cmd); }
```

**⚠️ これはセルフホストの必須条件です**（[self-hosting.md](../design/self-hosting.md) 3.4）。
Mython 版コンパイラは、最後に `clang foo.ll -o foo` を自分で呼びます。
`argv` と `system` が無いと、**コンパイラがコマンドとして成立しません。**

---

## 14.4 lib/ を探す（探索場所が 2 つになる）

### 📖 決めごと

第13章では「入口ファイルのあるディレクトリだけ」を探していました。
標準ライブラリが増えるので、探索場所が 2 つになります。

```
① 入口ファイルのあるディレクトリ
② lib/（コンパイラに埋め込まれたパス）
```

### ⚠️ 「どちらを先に探すか」ではなく「両方にあったらエラー」

ふつうは優先順位を決めます。しかしどちらを選んでも損をします。

| 決め方 | 何が起きるか |
|---|---|
| 利用者のディレクトリを先に | 自分の `dict.my` が標準ライブラリを**黙って隠す** |
| `lib/` を先に | 自分で書いた `dict.my` が**黙って無視される**（もっと悪い） |

```
error: モジュール 'dict' が標準ライブラリと衝突しています
   |
 1 | import dict
   | ^^^^^^ どちらを指しているか決められません
   |
   = ヒント: 見つかった場所:
             ./dict.my
             /path/to/lib/dict.my
             どちらかの名前を変えてください
```

**★ 第13章の判断 146 と同じ手です。**
曖昧さは、解決規則を作るより**起こせなくする**ほうが小さく済みます。

### ✍️ `lib/` の場所はビルド時に埋め込む

```makefile
# Makefile
CFLAGS += -DMYTHON_LIB_DIR='"$(abspath lib)"'
```

**⚠️ stage0 だけの割り切りです**（第9章の `MYTHON_RUNTIME_O` と同じ）。
ビルドツリーの外にインストールする話は第20章で見直します。

---

## 14.5 Mython で書く標準ライブラリ

### ✍️ strings.my — 文字列操作

```python
# lib/strings.my — 文字列ヘルパ（すべて Mython で書ける）

def substr(s: str, start: int, count: int) -> str:
    out: str = ""
    i: int = start
    n: int = len(s)
    while i < n and i < start + count:
        out = out + s[i]        # ★ s[i] は 1 文字の str（型システム 5.8）
        i = i + 1
    return out

def find(s: str, sub: str) -> int:
    ...                          # 見つからなければ -1

def split(s: str, sep: str) -> list[str]:
    ...

def join(xs: list[str], sep: str) -> str:
    ...
```

**★ ここに 1 行も C はありません。**
第9章（`str` と `+`）・第10章（`list[T]`）・第11章（`for` / `range`）で
入れた機能の組み合わせだけで書けています。

**⚠️ `substr` は O(n²) です。** `out = out + s[i]` が毎回新しい文字列を
確保するからです。**それでも今は直しません。**
速度が問題になるのは第20章（ブートストラップ）の後で、
そのときに「どこが遅いか」を測ってから直します。

> **★ 自分の言語で標準ライブラリを書くと、言語の穴が見つかります。**
> 書きながら「これが無いと不便だ」と思ったものは、
> **第15章（セルフホスト準備）のチェックリスト行き**です。

### ✍️ io.my / sys.my — C ランタイムを包む

```python
# lib/io.my
extern def my_read_file(path: str) -> str
extern def my_write_file(path: str, text: str) -> None
extern def my_file_exists(path: str) -> int

def read_file(path: str) -> str:
    return my_read_file(path)

def write_file(path: str, text: str) -> None:
    my_write_file(path, text)

def exists(path: str) -> bool:
    return my_file_exists(path) == 1      # ★ bool は境界を越えられない（14.2）
```

**🤔 ただ呼び直すだけの関数に意味はあるのか**

あります。**`extern` を 1 か所に閉じ込めるため**です。
利用者は `io.exists` を使い、`my_file_exists` を知りません。
ランタイム側の名前や引数が変わっても、直すのは `lib/io.my` の 1 行だけです。

**★ これは「境界に薄い層を置く」という設計そのものです。**

---

## 14.6 dict をジェネリクス無しで作る

### ⚠️ 問題：`dict[str, X]` は書けない

セルフホストではシンボルテーブルが要ります（[self-hosting.md](../design/self-hosting.md) 3.3）。
しかし Mython には**利用者定義のジェネリクスがありません**。
`list[T]` はコンパイラが特別扱いしている組み込みだからです。

### ✍️ 判断：`str → int` の表だけ作る

```python
# lib/dict.my
class Dict:
    keys_: list[str]
    vals_: list[int]

    def has(self, k: str) -> bool: ...
    def get(self, k: str) -> int: ...      # 無ければ panic
    def get_or(self, k: str, default: int) -> int: ...
    def set(self, k: str, v: int) -> None: ...
    def len(self) -> int: ...
    def keys(self) -> list[str]: ...
```

**★ 値が `int` だけでも、実用上は困りません。**

```python
syms: dict.Dict = dict.Dict()
table: list[Symbol] = []

table.append(sym)
syms.set(sym.name, len(table) - 1)     # ★ 添字を入れる

i: int = syms.get_or("main", -1)
if i >= 0:
    found: Symbol = table[i]
```

**「オブジェクトの表」ではなく「オブジェクトの置き場所の表」にします。**
ハンドル（添字）で間接参照するのは、GC の無い言語では**むしろふつうの手**です。

**⚠️ 探索は線形です。** 1000 個入れたら 1000 回比べます。
[self-hosting.md](../design/self-hosting.md) 3.3 に
「間に合わなければ線形探索で先に進んでよい」と書いたとおり、
**まず正しく動かして、速度は第20章の後に測ります。**

---

## 14.7 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```
全 292 件パス
```

12 件追加しました（正常系 7・エラー 5）。
ビルド警告 0 件、ASan/UBSan も 292 ケースすべてで検出 0 件。

### ✅ ゴールのプログラム

```bash
$ printf 'a,b,c\n' > hello.txt
$ ./build/mythonc t.my -o t && ./t
a / b / c
```

`import strings` と書くだけで、コンパイラが `lib/strings.my` を見つけています。
**コマンドラインには何も足していません。**

### ★ コンパイラは `split` を知らない（この章の主役）

```bash
$ ./build/mythonc --keep-ll t.my -o t
$ ls *.ll
t.io.ll   t.strings.ll   t.t.ll     ← ★ 標準ライブラリも「ただのモジュール」
```

生成された IR：

```llvm
; t.strings.ll
define ptr @strings.substr(ptr %s.arg, i64 %start.arg, i64 %count.arg) {
define i1 @strings.matches_at(ptr %s.arg, ptr %sub.arg, i64 %at.arg) {
define i64 @strings.find(ptr %s.arg, ptr %sub.arg) {
```

**`@strings.split` は `@lexer.make`（第13章）とまったく同じ形です。**
コンパイラから見て、標準ライブラリと利用者のコードに違いはありません。

### ✅ extern は C の名前のまま

```llvm
; t.io.ll
declare ptr @my_read_file(ptr)          ← ★ @io.my_read_file ではない
declare void @my_write_file(ptr, ptr)
declare i64 @my_file_exists(ptr)

define ptr @io.read_file(ptr %path.arg) { ... }   ← 包む側は修飾される
define i1 @io.exists(ptr %path.arg) { ... }
```

**修飾する名前としない名前が、1 つのファイルに並んでいます。**
`@io.exists` が `i1`（Mython の `bool`）を返し、
`@my_file_exists` が `i64` を返しているのも見えます。
**`bool` を C の境界で止めた判断（14.2 節）がそのまま形になっています。**

### ✅ argv と system

```bash
$ ./t ARG1
1        ← len(sys.argv())（引数なしで実行した場合）
0        ← sys.run("true")
3        ← sys.run("exit 3")
```

**⚠️ `system()` の戻り値をそのまま返すと `exit 3` が `768` に見えます**（`3 << 8`）。
`WEXITSTATUS` で終了コードに直すのはランタイムの仕事にしました。
**境界の食い違いは、境界のところで吸収します。**

### ✅ 標準ライブラリだけで書いた語数カウント

`examples/wordcount.my`：

```bash
$ ./build/mythonc examples/wordcount.my -o wc && ./wc examples/sample.txt
the: 3
quick: 1
brown: 1
fox: 2
jumps: 1
over: 1
lazy: 1
dog: 1
```

使っているのは `io.read_file` / `strings.split` / `strings.strip` /
`dict.Dict` ——**全部 Mython で書かれたもの**です。

### ★ substr は本当に O(n²) か（実測）

`strings.substr` は `out = out + s[i]` を繰り返します。
文字列は不変なので、1 文字足すたびに新しい領域を確保して全部コピーします。

```bash
$ for n in 10000 20000 40000 80000; do ./bench $n; done
n=10000   real 0.07
n=20000   real 0.27      ← ×3.9
n=40000   real 1.12      ← ×4.1
n=80000   real 5.12      ← ×4.6
```

**入力を 2 倍にすると時間が約 4 倍**——きれいな O(n²) です。

**⚠️ それでも今は直しません。**
セルフホストのコンパイラが扱うソースは 1 ファイル数千行なので、
**この速度でも動きます**。直すのは第20章（ブートストラップ完了）の後、
**どこが遅いかを測ってから**です。

**★ 「遅いと知りながら進む」のと「遅いことに気づかないまま進む」のは違います。**
測って、記録して、担当章を決めておけば、それは技術的負債ではなく**予定**です。

### ★ 自分の言語で書くと、言語の穴が見つかる

`lib/*.my` を書きながら「これが無いと不便だ」と思ったものを、
[self-hosting.md](../design/self-hosting.md) 3.6 節に記録しました。

| 見つかった穴 | どう回避したか |
|---|---|
| **`T \| None` が無い** | `find` は「見つからない」を `-1` で返す。`dict.get` は panic |
| **`s[i]` が毎回ヒープ確保する** | 避けようがない。**substr が O(n²) になる主因** |
| 文字列スライス `s[a:b]` が無い | `strings.substr()` を書いた |
| `in` 演算子が無い | `strings.contains()` を書いた |
| ジェネリクスが無い | `dict.Dict` は `str → int` 限定（14.6 節） |

**★ これが第14章を第15章（セルフホスト準備）の前に置いた理由です。**
「何が足りないか」は、**使ってみないと分かりません。**

### ✅ 標準ライブラリと同じ名前を作ったら

```bash
$ ls
main.my  strings.my        ← 自分で strings.my を書いてしまった
```

```
error: モジュール 'strings' が標準ライブラリと衝突しています
 4 | import strings
   | ^^^^^^ どちらを指しているか決められません
   = ヒント: 見つかった場所:
             tests/mods/err_lib_collision/strings.my
             /path/to/lib/strings.my
             どちらかの名前を変えてください
```

**どちらかを黙って優先していたら、気づくのは何時間も後です。**
第13章の判断 146（曖昧さは起こせなくする）と同じ手を、探索場所にも使いました。

### ✅ extern の診断

```
error: extern の戻り値に bool は使えません
 1 | extern def my_file_exists(path: str) -> bool
   |                                         ^^^^ この型は C との境界を越えられません
   = ヒント: int で受け取り、Mython 側で 'n == 1' と書いてください
```

```
error: extern def に ':' は書けません
   = ヒント: extern 宣言は本体を持ちません（改行で終わります）
```

---

## 14.8 まとめと次章の予告

### できたこと

```
✅ extern def — C の関数を宣言して呼ぶ（名前修飾しない唯一の例外）
✅ ランタイム：ファイル入出力・argv・system（WEXITSTATUS で終了コードに直す）
✅ lib/ の探索（探索場所が 2 つ。両方にあったらエラー）
✅ lib/strings.my — substr / find / contains / startswith / endswith /
                    split / join / strip / repeat / replace（C は 1 行も無い）
✅ lib/io.my / lib/sys.my — extern を閉じ込める薄い層
✅ lib/dict.my — str → int の表（値に list の添字を入れて使う）
✅ examples/wordcount.my — 標準ライブラリだけで書けることの確認
✅ 言語の穴を 6 件見つけて記録した（第15章の入力）
✅ テスト 292 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `lib/strings.my` | **新規 120 行。Mython で書いた文字列ライブラリ** |
| `lib/dict.my` | **新規 55 行。class と list[T] だけで作った表** |
| `lib/io.my` / `lib/sys.my` | **新規 39 行。extern を閉じ込める層** |
| `runtime/runtime.c` | ファイル入出力・`my_set_args` / `my_argv` / `my_system`（82 行） |
| `src/parser.c` | `extern_def()`（64 行） |
| `src/sema.c` | extern の登録（修飾しない）と bool の禁止（29 行） |
| `src/codegen.c` | extern の `declare`、`@main(i32, ptr)`（23 行） |
| `src/module.c` | `lib/` の探索と衝突検出（59 行） |
| `Makefile` | `-DMYTHON_LIB_DIR` |
| `tests/` | 12 件追加 |

**★ コンパイラ本体（`src/`）の変更は 175 行です。**
この章の成果物のほとんどは `lib/*.my`——**Mython で書かれています。**

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | `main` はラッパ方式（方式 A） | `argc` / `argv` を受け取っても**利用者の main も AST も無変更** |
| ch9 | `MYTHON_RUNTIME_O` をビルド時に埋め込む | `MYTHON_LIB_DIR` を同じ手で足した |
| ch9 | `my_panic`（親切に落ちる） | ファイルが開けないときの落ち方がそのまま決まった |
| ch9 | `str` は NUL 終端の C 文字列 | `extern` で C の関数にそのまま渡せた |
| ch10 | `list[T]` はランタイムの `MyList` | `my_argv()` が `list[str]` を返せた |
| ch12 | class とメソッド | `dict.Dict` が書けた |
| ch13 | `import` とモジュールごとの `.ll` | **標準ライブラリを言語に埋め込まずに済んだ** |
| ch13 | 曖昧さは起こせなくする（判断 146） | `lib/` との名前衝突をエラーにする形に流用 |
| ch13 | モジュール修飾は `mangle()` を通す | `extern` だけ通さない、が 1 行で書けた |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| **Mython で書けるものは Mython で書く** | 言語に入れた機能は減らせない。ライブラリなら差し替えられる。**書いてみると言語の穴も見つかる** |
| **`extern` は名前修飾しない** | 指す先は C 側で名前が決まっているシンボル。修飾の目的（Mython 側の衝突回避）がそこでは成立しない |
| **`extern` に `bool` を通さない** | `i1` と C の `_Bool` の ABI が環境依存。境界は狭いほど事故が減る。`int` で受けて `== 1` と書けば済む |
| **`extern` を `lib/*.my` に閉じ込める** | 利用者は `io.exists` だけを知る。ランタイム側が変わっても直すのは 1 か所 |
| **探索場所の優先順位を決めず、衝突をエラーにする** | どちらを優先しても「黙って隠れる」ものが出る |
| **`dict` は `str → int` 限定** | ジェネリクスを入れない判断の帰結。値は `list[T]` の添字（ハンドル）で持てば実用上困らない |
| **線形探索で始める** | まず正しく動かす。速度は第20章の後に測ってから直す |
| **`system()` の戻り値をランタイムで直す** | `exit 3` が `768` に見えるのは驚き。境界の食い違いは境界で吸収する |

### ⚠️ 予想が外れたこと

**「ただ呼び直すだけの `io.read_file` は無駄では？」と思っていた。**

書いてみると、この薄い層が**型の変換の置き場所**になりました。
`my_file_exists` は `int` を返し、`io.exists` は `bool` を返します。
**`bool` を C の境界で止める判断は、この層があるから成立しています。**

**★ 「薄い層」は、境界の食い違いを吸収する場所として働きます。**

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| `strings.substr` が O(n²)（`s[i]` が毎回ヒープ確保する） | 第15章で「穴」として判断、第20章の後に最適化 |
| `dict.Dict` が線形探索 | 同上 |
| `T \| None` が無いので `find` が `-1`、`dict.get` が panic | 第15章 |
| `lib/` の場所がビルドツリー固定 | 第20章（インストールの話） |
| `float` 用のライブラリが無い | 作らない（セルフホストに不要） |

### ✍️ commit する

```bash
git add -A
git commit -m "第14章: 標準ライブラリ"
```

---

## 次章：第15章 セルフホスト準備（機能棚卸し）

**達成目標**

[design/self-hosting.md](../design/self-hosting.md) のチェックリストを全部埋める。
**そのために足りない機能を実装します。**

**やること**

| 作業 | 中身 |
|---|---|
| **`T \| None` と narrowing** | この章までに 3 回「無いから諦めた」と書いた機能。最優先 |
| 文字列アクセスの高速化の判断 | `s[i]` が毎回ヒープ確保する問題（3.6 節） |
| 棚卸し | 「Mython でコンパイラを書けるか」を機能ごとに確認する |

**★ 第15章は「新機能を思いつく章」ではありません。**
第14章までに**実際に困ったこと**を、記録から拾って潰す章です。

**⚠️ 予想される落とし穴**

- `T | None` は**フロー依存**（`if t is not None:` の中だけ型が変わる）。
  第5章からの「変数の型は 1 つ」という前提を崩す
- `None` を受け取れる `Type` をどう表すか（`TY_OPTION` を足すか、`Type` に印を足すか）
- 既存の全機能（代入・引数・戻り値・`list[T]` の要素）に影響が出る

### 🤔 第15章に入る前の練習問題

1. `lib/strings.my` の `substr` を**ランタイム側（C）に移して**（`my_substr`）、
   14.7 節のベンチマークがどう変わるか測る（**測ってから戻す**）
2. `lib/dict.my` の `index_of` に「見つかった位置を先頭に移す」
   （move-to-front）を足して、`wordcount` の出力が変わらないことを確かめる
3. `extern def my_system(cmd: str) -> bool` と書いて、どんな診断が出るか見る
4. `lib/` に自分のモジュール（`mylib.my`）を置いて `import mylib` できるか試す。
   **できてしまうのは良いことか**を考える
5. **`strings.my` に `lstrip` / `rstrip` を足す**。テストを
   `tests/cases/lib_strings*.my` に倣って書く
