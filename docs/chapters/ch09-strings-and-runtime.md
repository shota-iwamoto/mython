# 第9章 文字列と C ランタイム連携

> **この章のゴール**
> 文字列が使えるようになり、**FizzBuzz が本物になる**。
>
> ```bash
> $ cat t.my
> def main() -> int:
>     name: str = "Mython"
>     print("hello, " + name)
>     print(len(name))
>     return 0
> $ ./build/mythonc t.my -o t && ./t
> hello, Mython
> 6
> ```

**この章は「2 つ目のコード」を持ち込む章です。**

第1章から、私たちが出力するのは LLVM IR だけでした。
この章で **`runtime/runtime.c`（C で書いたランタイム）を新設**し、
生成した IR とリンクします。

```
                 ┌─ mythonc が生成した .ll ─┐
                 │                          │
ソース ─▶ mythonc ─┤                          ├─▶ clang ─▶ 実行ファイル
                 │                          │
                 └─ runtime.o（C で手書き）─┘
```

**★ 設計方針：生成する IR を単純に保つために、複雑さを C 側に押し出す。**

> **R10. 制御フローを含む処理はランタイム関数呼び出しにする。**
> — [../design/ir-conventions.md](../design/ir-conventions.md)

---

## 目次

- [9.1 なぜランタイムが要るのか](#91-なぜランタイムが要るのか)
- [9.2 字句解析器：文字列リテラル](#92-字句解析器文字列リテラル)
- [9.3 IR に文字列を埋め込む](#93-ir-に文字列を埋め込む)
- [9.4 str 型は参照型](#94-str-型は参照型)
- [9.5 ランタイムを書く](#95-ランタイムを書く)
- [9.6 Makefile とリンク](#96-makefile-とリンク)
- [9.7 組み込み関数の表](#97-組み込み関数の表)
- [9.8 文字列の演算](#98-文字列の演算)
- [9.9 R10 の実践：0 除算と `**`](#99-r10-の実践0-除算と-)
- [9.10 FizzBuzz が本物になる](#910-fizzbuzz-が本物になる)
- [9.11 動作確認](#911-動作確認)
- [9.12 まとめと次章の予告](#912-まとめと次章の予告)

---

## 9.1 なぜランタイムが要るのか

### 📖 IR だけでは書けないもの

第7章で `print` を実装したとき、C の `printf` を借りました。
`declare i32 @printf(ptr, ...)` と書くだけで、リンカが libc の実装を繋いでくれました。

**では `"a" + "b"` はどうでしょうか。**

1. 2 つの文字列の長さを測る
2. 合計 + 1 バイトのメモリを確保する
3. 順にコピーする
4. NUL を置く

**これを LLVM IR で手書きするのは、できますが、割に合いません。**
ループが要り、`malloc` の失敗処理が要り、生成器のコードが一気に膨らみます。

```c
// runtime/runtime.c
char *my_str_concat(const char *a, const char *b) {
    long long la = (long long)strlen(a), lb = (long long)strlen(b);
    char *p = my_alloc(la + lb + 1);
    memcpy(p, a, (size_t)la);
    memcpy(p + la, b, (size_t)lb);
    p[la + lb] = '\0';
    return p;
}
```

**C で書けば 7 行です。** コード生成器の仕事は `call ptr @my_str_concat(...)` を
1 行出すだけになります。

### 📖 どこまでを IR にし、どこからをランタイムにするか

| | IR で出す | ランタイムに任せる |
|---|---|---|
| 判断基準 | 命令 1〜数個で書ける | **ループ・分岐・メモリ確保**が要る |
| 例 | `a + b`（int）、比較、`if` | 文字列連結、`len`、0 除算の検査 |

**★ この線引きが R10 です。** 迷ったらランタイム側に置きます。
生成器が複雑になるより、C の関数が 1 つ増えるほうがずっと安全です。

---

## 9.2 字句解析器：文字列リテラル

### ✍️ トークン種別を足す

```c
typedef enum {
    ...
    TK_STR,  // 文字列リテラル（第9章）
} TokenKind;
```

`Token` には**エスケープを解決した後**のバイト列を持たせます。

```c
struct Token {
    ...
    char *text;  // TK_IDENT / TK_KEYWORD / TK_STR（TK_STR は解決後の中身）
    int slen;    // TK_STR のバイト長（★ NUL を含み得るので strlen では測れない）
};
```

**⚠️ 長さを別に持つ**のが要点です。`"a\0b"` のような文字列を将来許すなら、
`strlen()` では測れません。**最初から長さを持たせておきます。**

### ✍️ エスケープの解決

```c
static const struct { char c; char to; } ESCAPES[] = {
    {'n', '\n'}, {'t', '\t'}, {'r', '\r'}, {'0', '\0'},
    {'\\', '\\'}, {'"', '"'}, {'\'', '\''},
    {0, 0},
};
```

**⚠️ 解決は字句解析器の仕事です。**
構文解析器から先は「もう解決済みのバイト列」だけを見ます。
`\n` が「バックスラッシュと n」なのか「改行」なのかを
後の段階で気にしなくて済むようにします。

### ⚠️ 未知のエスケープはエラーにする

```python
"C:\path"      # ← \p は未知
```

**黙って `\p` を `p` にする言語もありますが、Mython はエラーにします。**

```
error: 未知のエスケープシーケンス '\p' です
   |
   = ヒント: 使えるのは \n \t \r \0 \\ \" \' です。
             バックスラッシュそのものを書くには \\ とします
```

**理由**：黙って通すと、`"C:\new"` が `C:` + 改行 + `ew` になって
**気づきにくいバグ**になります。第3章から一貫している「曖昧なら止まる」方針です。

### ⚠️ 行をまたぐ文字列

```python
s: str = "hello
```

閉じ引用符がないまま行末に来たらエラーです。
**次の行まで読みに行ってはいけません**（エラー位置が遠くなり、原因が分からなくなる）。

```
error: 文字列が閉じられていません
   |
 2 |     s: str = "hello
   |              ^ この引用符に対応する " がありません
```

---

## 9.3 IR に文字列を埋め込む

### ✍️ グローバル定数として出す

```llvm
@.str.0 = private unnamed_addr constant [6 x i8] c"hello\00"
```

| 要素 | 意味 |
|---|---|
| `private` | このモジュールの外から見えない |
| `unnamed_addr` | アドレスに意味がない（同じ内容をまとめてよい） |
| `[6 x i8]` | **5 文字 + NUL = 6 バイト** |
| `c"..."` | バイト列リテラル |

**⚠️ 長さは「文字数 + 1」です。** 第7章の `@.fmt.int` で
`[6 x i8] c"%lld\0A\00"` を数えたのと同じです。**ここを間違えると静かに壊れます。**

### ✍️ エスケープは全部 `\XX` にする

```c
// IR の文字列に 1 バイト出力する。
//
// ⚠️ 安全策として、ASCII 印字可能文字**以外はすべて** \XX にします。
//    「どの文字をエスケープすべきか」を考えないで済むようにするためです。
static void emit_ir_byte(StrBuf *sb, unsigned char c) {
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
        sb_printf(sb, "%c", c);
    else
        sb_printf(sb, "\\%02X", c);
}
```

**★ 「印字可能なものだけそのまま、あとは全部 16 進」** にしておけば、
UTF-8 の日本語もそのまま通ります（各バイトが `\XX` になるだけ）。

### 📖 同じ文字列は共有する

```python
print("hi")
print("hi")     # ← 同じ内容
```

`@.str.0` を 2 つ作る必要はありません。
**すでに出した文字列を覚えておいて使い回します。**

```c
// 出力済みの文字列リテラル（内容 → ラベル）
typedef struct StrLit StrLit;
struct StrLit {
    char *bytes; int len; char *label;
    StrLit *next;
};
```

線形探索で十分です（第5章のシンボルテーブルと同じ判断）。

---

## 9.4 str 型は参照型

```c
typedef enum {
    TY_INT, TY_BOOL, TY_NONE,
    TY_STR,   // str → ptr（第9章）
} TypeKind;
```

| Mython | LLVM（値） | LLVM（メモリ） |
|---|---|---|
| `str` | `ptr` | `ptr` |

**★ 値型と参照型の違いが、ここで初めて出てきます**
（[../design/memory-model.md](../design/memory-model.md) 1 節）。

```python
a: str = "hello"
b: str = a        # ★ 中身はコピーされない。ポインタがコピーされる
```

**`str` は参照型ですが、不変（immutable）です。**
`s = s + "a"` は新しい文字列を作って `s` を差し替えます。
だから「共有していると書き換えが伝播する」問題は起きません。

### 📖 メモリはどこにあるか

```
┌─ .rodata（読み取り専用）─────┐
│  @.str.0 = "hello"          │  ← リテラル。プログラム終了まで存在
├─ ヒープ ────────────────────┤
│  my_str_concat が作った文字列 │  ← my_alloc で確保。★ 解放しない
└─────────────────────────────┘
```

**⚠️ v1 では文字列を解放しません**（メモリモデル 3 節）。

**🤔 なぜそれで許されるのか**
コンパイラは「起動して、コンパイルして、終了する」プログラムです。
プロセスが終われば OS がすべて回収します。
**GC も参照カウントも実装せずに、セルフホストまで到達できます。**

これは手抜きではなく**設計判断**で、メモリモデル 3 節に理由と限界を書いてあります。

---

## 9.5 ランタイムを書く

### ✍️ `runtime/runtime.c`（新規）

```c
// runtime/runtime.c — Mython のランタイムライブラリ
//
// ここに置くもの：ループ・分岐・メモリ確保を含む処理（規約 R10）。
// 生成する IR を単純に保つために、複雑さをこちら側に押し出します。

void *my_alloc(long long size) {
    void *p = calloc(1, (size_t)size);
    if (!p) my_panic("out of memory");
    return p;
}
```

**`calloc` でゼロ初期化する**のと**失敗したら即終了する**のが要点です
（メモリモデル 4 節）。失敗を即終了にすると、
**生成する IR に NULL チェックを入れなくて済みます。**

### ✍️ ランタイムエラーの出し方

言語仕様 8 節が形式を決めています。

```c
// 回復不能なエラー。stderr に出して終了コード 1 で死ぬ。
// 例外機構（try/except）は v1 では採用しません（言語仕様 8 節）。
_Noreturn void my_panic(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}
```

**⚠️ `_Noreturn` を付ける**と、C コンパイラが
「この後に到達しない」と分かるので警告が減ります。

### ✍️ 提供する関数

メモリモデル 4 節で決めた API を実装します。

| 関数 | 用途 |
|---|---|
| `my_alloc` | メモリ確保（失敗で panic） |
| `my_panic` | ランタイムエラー |
| `my_print_int` / `my_print_str` / `my_print_bool` | `print` のオーバーロード |
| `my_str_concat` | `+` |
| `my_str_len` | `len()` |
| `my_str_cmp` | 比較（`strcmp` の符号） |
| `my_str_from_int` / `my_str_from_bool` | `str()` |
| `my_str_to_int` | `int()` |
| `my_ord` / `my_chr` | `ord()` / `chr()` |
| `my_floordiv` / `my_mod` | **0 除算の検査つき除算**（9.9 節） |
| `my_ipow` | **`**`（負の指数を検査）**（9.9 節） |
| `my_exit` | `exit()` |

**⚠️ 名前を全部 `my_` で始める**のは、libc のシンボルと衝突させないためです。
第8章でグローバル変数に `@g.` を付けたのと同じ理由です。

---

## 9.6 Makefile とリンク

### ✍️ ランタイムをビルドする

```makefile
RUNTIME_SRC := runtime/runtime.c
RUNTIME_OBJ := build/runtime.o

$(RUNTIME_OBJ): $(RUNTIME_SRC) | build
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@
```

**⚠️ ランタイムは `-O2` でビルドします。**
コンパイラ本体（`-O0 -g`）とは目的が違います。
ランタイムは**ユーザーのプログラムの一部として動く**ので、速いほうがよいからです。

### ✍️ コンパイラにランタイムの場所を教える

```makefile
CFLAGS += -DMYTHON_RUNTIME_O='"$(abspath $(RUNTIME_OBJ))"'
```

第1章で `target triple` を `-D` で渡したのと同じ手です。

```c
// main.c
sb_printf(&cmd, "clang %s '%s' '%s' -o '%s'", opt.opt_level, ll,
          MYTHON_RUNTIME_O, opt.output);
```

**⚠️ これは stage0（C 版）だけの割り切りです。**
本来はインストール先を探すべきですが、
**セルフホストまではビルドツリーの中だけで完結すればよい**ので、
絶対パスを埋め込みます。第20章で見直します。

---

## 9.7 組み込み関数の表

`print` が 3 つの型を受けるようになりました。**オーバーロードの仕組み**が要ります。

```c
// 組み込み関数の表。名前 + 引数型 で 1 つの候補を表す。
static const Builtin BUILTINS[] = {
    // 名前      引数型      戻り型     呼び出す実装
    {"print",  TY_INT,   TY_NONE, "my_print_int"},
    {"print",  TY_STR,   TY_NONE, "my_print_str"},
    {"print",  TY_BOOL,  TY_NONE, "my_print_bool"},
    {"len",    TY_STR,   TY_INT,  "my_str_len"},
    {"str",    TY_INT,   TY_STR,  "my_str_from_int"},
    {"str",    TY_BOOL,  TY_STR,  "my_str_from_bool"},
    {"int",    TY_STR,   TY_INT,  "my_str_to_int"},
    {"ord",    TY_STR,   TY_INT,  "my_ord"},
    {"chr",    TY_INT,   TY_STR,  "my_chr"},
    {"exit",   TY_INT,   TY_NONE, "my_exit"},
    {"panic",  TY_STR,   TY_NONE, "my_panic"},
};
```

**★ 表にしておくと、sema と codegen が同じ表を見られます。**
sema は「型が合う候補があるか」を、codegen は「どの C 関数を呼ぶか」を見ます。

**🤔 なぜ `print` だけオーバーロードを許すのか**（言語仕様 7 節）
ユーザー定義関数のオーバーロードは**許しません**（名前解決が複雑になるため）。
組み込みだけは、表を引くだけで解決できるので許します。
**「言語機能」ではなく「表のエントリ」なので、実装が増えません。**

### ✍️ 候補が無いときのエラー

```
error: print は 'None' 型を出力できません
   |
   = ヒント: print が受け取れるのは int, str, bool です
```

**「何なら受け取れるのか」を必ず書きます**（第5章の `type_name_list()` と同じ発想）。

---

## 9.8 文字列の演算

### ✍️ `+` は連結

```c
// sema：op_supports に str を足す
if (t->kind == TY_STR) return op == OP_ADD || is_compare(op);
```

```llvm
%t2 = call ptr @my_str_concat(ptr %t0, ptr %t1)
```

**⚠️ `-` や `*` は使えません。** Python の `"ab" * 3` は便利ですが、
言語仕様 4.2 の表で `str` に許しているのは `+` だけです。

### ✍️ 比較は「内容」で行う

```python
a: str = "abc"
b: str = "ab" + "c"
a == b        # → True（内容が同じ）
```

**⚠️ ポインタ比較ではありません**（言語仕様 4.3）。

```llvm
%t2 = call i64 @my_str_cmp(ptr %t0, ptr %t1)
%t3 = icmp eq i64 %t2, 0
```

**`my_str_cmp` が `strcmp` の符号を返す**ので、
`<` `<=` `>` `>=` も同じ形で書けます（0 と比較する述語を変えるだけ）。

**★ 第6章で `icmp_pred()` を「型で述語を選ぶ」形にしておいたので、
ここは自然に拡張できました。**

---

## 9.9 R10 の実践：0 除算と `**`

### 📖 2 つの宿題を回収する

| 宿題 | いつから | なぜ第9章なのか |
|---|---|---|
| `1 // (2 - 2)` が SIGFPE | 第2章 | **エラーメッセージを出すランタイムが要る** |
| `**` が未実装 | 第2章 | **負の指数を実行時エラーにする**必要がある |

第2章でこう書いて先送りしていました。

> `**` は負の指数を実行時エラーにする必要があるため第9章に延期

**その第9章が来ました。**

### ✍️ 除算をランタイム関数にする

```c
long long my_floordiv(long long a, long long b) {
    if (b == 0) my_panic("division by zero");
    return a / b;
}
```

```llvm
; Before（第2章〜第8章）
%t2 = sdiv i64 %t0, %t1

; After（第9章）
%t2 = call i64 @my_floordiv(i64 %t0, i64 %t1)
```

**🤔 関数呼び出しにすると遅くならないか**

なります。しかし：

1. **IR に分岐を出す必要がなくなる**（規約 R10）。
   生成器は `call` を 1 行出すだけです
2. 0 除算は**プログラムを殺すバグ**です。1 命令の速さより、
   **`runtime error: division by zero` と出て死ぬ**ほうが価値があります
3. 速さが問題になったら、そのとき測って直します（第5章と同じ姿勢）

**⚠️ コンパイル時に分かる 0 除算は、今までどおりコンパイル時に弾きます。**
`1 // 0` は第5章の検査でエラーです。ランタイム検査は
「実行してみないと分からない場合」の最後の砦です。

### ✍️ `**`

```c
long long my_ipow(long long base, long long exp) {
    if (exp < 0) my_panic("negative exponent");
    long long r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}
```

**ループがあるので、当然ランタイム側です**（R10）。

パーサは第2章に書いておいたコメントのとおりに直します。

```c
// 第9章ではこの関数がこうなります（右結合なので unary() を再帰で呼ぶ）:
//     Node *base = primary(p);
//     if (consume(p, "**"))
//         return new_binop_node(t, OP_POW, base, unary(p));
//     return base;
```

**★ 7 章前に書いた「こうなります」が、そのまま実装になりました。**

---

## 9.10 FizzBuzz が本物になる

第7章では、文字列が無いので**負の数**で代用していました。

```python
# 第7章                      # 第9章
print(-15)                   print("FizzBuzz")
print(-3)                    print("Fizz")
print(-5)                    print("Buzz")
print(i)                     print(str(i))
```

**制御構造は 1 文字も変わりません。** 出力だけが本物になります。

```python
def main() -> int:
    i: int = 1
    while i <= 15:
        if i % 15 == 0:
            print("FizzBuzz")
        elif i % 3 == 0:
            print("Fizz")
        elif i % 5 == 0:
            print("Buzz")
        else:
            print(str(i))
        i += 1
    return 0
```

**★ 第7章で「第9章で書き換えるだけで完成します」と書いた約束を果たしました。**
先の章の機能を前借りせず、章ごとに動くものを残してきた結果です。

---

## 9.11 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```bash
make test
```

```
全 176 件パス
```

30 件追加し、2 件を削除しました（`err_pow_unsupported.my` と
`err_print_bool.my` は、**この章で「できるようになった」ので消えました**）。
ビルド警告 0 件、ASan/UBSan も全ケースでクリーンです。

### ✅ 目標

```bash
$ ./build/mythonc t.my -o t && ./t
hello, Mython
6
```

### ✅ 生成される IR

```bash
$ ./build/mythonc -S tests/cases/str_concat.my
```

```llvm
@.str.0 = private unnamed_addr constant [7 x i8] c"Mython\00"
@.str.1 = private unnamed_addr constant [8 x i8] c"hello, \00"
declare void @my_print_str(ptr)
declare ptr @my_str_concat(ptr, ptr)
```

**`"Mython"` は 6 文字なので `[7 x i8]`**（NUL の分）。
`"hello, "` は末尾の空白を含めて 7 文字なので `[8 x i8]`。**数え間違えていません。**

### ✅ UTF-8

```python
s: str = "こんにちは"
print(s)
print(len(s))
```

```llvm
@.str.0 = private unnamed_addr constant [16 x i8] c"\E3\81\93\E3\82\93\E3\81\AB\E3\81\A1\E3\81\AF\00"
```

```
こんにちは
15
```

**「印字可能な ASCII 以外は全部 `\XX`」という方針のおかげで、
日本語について何も特別扱いしていません。** 15 バイト + NUL = 16 です。

**⚠️ `len()` は「文字数」ではなく「バイト数」を返します。**
「こんにちは」は 5 文字ですが 15 バイトです。
これは既知の制限で、文字単位の操作は v1 では提供しません。

### ✅ 同じリテラルは共有される

```python
print("hi" == "hi")
```

```llvm
@.str.0 = private unnamed_addr constant [3 x i8] c"hi\00"
  %t1 = call i64 @my_str_cmp(ptr @.str.0, ptr @.str.0)
```

**定義は 1 つ、使用箇所は 2 つ。** 内容比較なので結果は変わりません（`True`）。

### ✅ 比較は内容で行う

```python
a: str = "abc"
b: str = "ab" + "c"      # ← 実行時に作られる別のポインタ
print(a == b)            # → True
```

**ポインタは違いますが、`True` です。**
`my_str_cmp` を通しているからです（言語仕様 4.3）。

順序比較も動きます（`"abc" < "abd"` → `True`）。
**第6章で `icmp_pred()` を「型で述語を選ぶ」形にしておいたので、
`my_str_cmp` の結果を 0 と比べる述語を変えるだけで 6 種類そろいました。**

### ✅ エスケープ

```python
print("a\tb\nc")
print("\"q\" \\ \'x\'")
```

```
a	b
c
"q" \ 'x'
```

### ✅ 組み込み関数

```python
print("n = " + str(42))    # n = 42
print(str(True))           # True
print(int("123") + 1)      # 124
print(ord("A"))            # 65
print(chr(65))             # A
print(True)                # True
print(1 > 2)               # False
```

**`print(bool)` が `True` / `False` と出ます**（`1` / `0` ではありません）。
第7章で「第9章で対応します」と書いた宿題です。

### ✅ 第2章から先送りしていた 2 つの宿題

**`**`（第2章で「第9章に延期」と書いた）**

```python
print(2 ** 10)        # 1024
print(2 ** 3 ** 2)    # 512（右結合：2 ** (3 ** 2)）
```

**0 除算（今まで SIGFPE だった）**

```bash
$ ./t
runtime error: division by zero
$ echo $?
1
```

**負の指数**

```bash
$ ./t
runtime error: negative exponent
$ echo $?
1
```

**言語仕様 8 節が決めた形式（`runtime error: ...` / 終了コード 1）どおりです。**

第2章のテスト `err_pow_unsupported.my`（「`**` はまだ未対応です」）は、
**役目を終えたので削除**しました。かわりに `rt_negative_exponent.my` が
実行時エラーを確かめています。

### ✅ FizzBuzz が本物になった

```bash
$ ./build/mythonc tests/cases/fizzbuzz.my -o t && ./t
1
2
Fizz
4
Buzz
Fizz
7
8
Fizz
Buzz
11
Fizz
13
14
FizzBuzz
```

**第7章のバージョンと比べてみてください。**

```python
# 第7章                          # 第9章
if i % 15 == 0:                  if i % 15 == 0:
    print(-15)                       print("FizzBuzz")
elif i % 3 == 0:                 elif i % 3 == 0:
    print(-3)                        print("Fizz")
```

**制御構造は 1 文字も変わっていません。**
第7章で「第9章で書き換えるだけで完成します」と書いた約束を果たしました。

### ✅ エラー：未知のエスケープ

```
error: 未知のエスケープシーケンス '\p' です
  --> t.my:2:17
   |
 2 |     s: str = "C:\path"
   |                 ^^
   |
   = ヒント: 使えるのは \n \t \r \0 \\ \" \' です。バックスラッシュそのものを書くには \\ とします
```

### ✅ エラー：文字列の閉じ忘れ

```
error: 文字列が閉じられていません
   |
 2 |     s: str = "hello
   |              ^
   |
   = ヒント: 文字列は同じ行の中で閉じてください
```

**開き引用符の位置**を指しています。行をまたいで読みに行かないので、
エラーが遠くに飛びません。

### ✅ エラー：型の混在

```
error: 型 'str' と 'int' に演算子 '+' は適用できません
   |
   = ヒント: Mython には暗黙の型変換がありません（言語仕様 3.5）
```

**`"n = " + 42` は書けません。** `str(42)` を挟みます。
Python なら `TypeError` になる場面を、**コンパイル時に**捕まえています。

### ✅ エラー：組み込みが受け取れない型

```
error: len は 'int' 型を受け取れません
   |
   = ヒント: len が受け取れるのは str です
```

```
error: print は 'None' 型を受け取れません
   |
   = ヒント: print が受け取れるのは int, str, bool です
```

**「何なら受け取れるのか」を表から自動生成しています。**
第10章で `len(list)` を足せば、この一覧も自動的に増えます。

---

## 9.12 まとめと次章の予告

### できたこと

```
✅ runtime/runtime.c の新設 — 初めて「IR 以外のコード」をリンクした
✅ 文字列リテラルとエスケープ（未知のエスケープはエラー）
✅ @.str.N の出力、同じ内容の共有、UTF-8 をそのまま通す
✅ TY_STR（参照型）
✅ 組み込み関数の表 — print / len / str / int / ord / chr / exit / panic
✅ print のオーバーロード（int / str / bool）
✅ 文字列の + （連結）と 6 種類の比較（内容比較）
✅ 0 除算の実行時検査（規約 R10）— SIGFPE ではなくメッセージを出す
✅ ** 演算子（負の指数は実行時エラー）— 第2章からの宿題
✅ FizzBuzz が本物になった — 第7章からの約束
✅ テスト 176 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `runtime/runtime.c` | **新規**。17 個の関数 |
| `Makefile` | ランタイムを `-O2` でビルド、パスを `-D` で埋め込む |
| `src/main.c` | `clang` にランタイムをリンクさせる |
| `src/lexer.h/c` | `TK_STR`、`slen`、`read_string()`、エスケープ表 |
| `src/types.h/c` | `TY_STR`、`type_from_kind()` |
| `src/ast.h/c` | `ND_STR`、`OP_POW`、`sval` / `slen` / `builtin` |
| `src/parser.c` | 文字列リテラル、`**`（第2章のコメントどおり） |
| `src/sema.h/c` | `Builtin` 表、`check_builtin_call()`、`str` の演算 |
| `src/codegen.c` | `intern_str()` / `emit_ir_byte()` / `declare_rt()` / `gen_builtin_call()`、`str` の演算、検査つき算術 |
| `tests/cases/` | 30 件追加、2 件削除 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch1 | `globals` / `decls` バッファ | `@.str.N` と `declare` の置き場所 |
| ch1 | triple を `-D` で渡す | ランタイムのパスも同じ手で渡せた |
| ch2 | 「`**` は第9章に延期」とコメント | **7 章越し**にそのまま実装 |
| ch5 | `type_name_list()` を 1 か所に | 組み込みの「受け取れる型」一覧も同じ発想で自動生成 |
| ch6 | `icmp_pred()` を型で選ぶ形に | `my_str_cmp` の結果を 0 と比べるだけで 6 種類そろった |
| ch6 | `zext` / `trunc` を境界に閉じ込めた | C との境界（bool → i64）でも同じ形 |
| ch7 | `print` の暫定実装 | 表に 3 行足すだけで本物になった |
| ch8 | 組み込みの再定義を禁止 | `len` / `str` などにも自動的に適用された |
| 設計 | R10（複雑さはランタイムへ） | 0 除算・`**`・連結すべてこの方針で解決 |
| 設計 | メモリを解放しない | 文字列の寿命を考えずに済んだ |

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| `len()` が**バイト数**を返す（文字数ではない） | v1 の制限 |
| 文字列の部分取得（`s[i]` / スライス） | 第10章（添字構文） |
| `float` 型と `fcmp` | v1 未対応 |
| 到達不能コードの**警告**（`diag.c` に非致命の経路が無い） | 第10章以降 |
| ランタイムのパスを絶対パスで埋め込んでいる | 第20章 |
| `"ab" * 3`（文字列の繰り返し） | 採用しない（言語仕様 4.2） |

### ✍️ commit する

```bash
git add -A
git commit -m "第9章: 文字列と C ランタイム連携"
```

---

## 次章：第10章 list[T]（動的配列）

**達成目標**

```python
def main() -> int:
    xs: list[int] = []
    xs.append(1)
    xs.append(2)
    print(xs[0] + xs[1])    # 3
    print(len(xs))          # 2
    return 0
```

**やること**

| ファイル | 作業 |
|---|---|
| `types.c` | **`list[T]`（複合型）** — ついに `type_equal()` の再帰比較が要る |
| `parser.c` | 型注釈の `list[int]`、添字 `xs[i]`、メソッド呼び出し `xs.append(x)` |
| `sema.c` | 要素型の伝播、`len` のオーバーロード追加 |
| `runtime.c` | `MyList`、`my_list_new` / `my_list_push` / `my_list_get`（**範囲検査つき**） |
| `codegen.c` | `getelementptr`、`ptr` としての list |

**★ 第5章で「`type_equal()` を必ず通せ。`a == b` は第10章で壊れる」と
書いた予告が、ついに現実になります。**

**⚠️ 予想される落とし穴**

- `list[int]` と `list[int]` は**別のオブジェクト**（シングルトンではない）→ 再帰比較が必須
- 空リスト `[]` の要素型は**注釈から**決まる（推論しない）
- 範囲外アクセスはランタイムで検査する（規約 R10。メッセージは言語仕様 8 節）
- `xs[i] = v`（添字への代入）は第5章で用意した「式を先に読んで役割を決める」形が効く
- `postfix()` のループに `[` と `.` を足すだけで構文は済む（第8章の設計）

### 🤔 第10章に入る前の練習問題

1. **`emit_ir_byte()` の条件から `c != '"'` を外して** `"\"q\""` を
   コンパイルし、LLVM が何と言うか見る（**必ず元に戻す**）
2. `intern_str()` の共有（既出の探索）を消して、`"hi" == "hi"` の IR が
   どう変わるか見る
3. **`my_floordiv` の 0 検査を消して** `rt_div_zero.my` を実行し、
   終了コードがどうなるか確かめる（ヒント：SIGFPE は 128 + 8）
4. `str(1) + str(2)` と `str(1 + 2)` の違いを IR で確認する
5. `runtime.c` に `my_str_repeat`（`"ab" * 3`）を書いてみる。
   **言語仕様がそれを採用しない理由**も考えてみる
