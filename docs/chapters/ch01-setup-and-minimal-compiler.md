# 第1章 環境構築と最小コンパイラ

> **この章のゴール**
> `42` と書いたファイルをコンパイルして、**終了コード 42 で終わる実行ファイル**を作る。
>
> ```bash
> $ echo 42 > t.my
> $ ./build/mythonc t.my -o t
> $ ./t
> $ echo $?
> 42
> ```

貧弱に見えますか？ しかしこの章が終わると、
**字句解析 → 構文解析 → コード生成 → LLVM → 実行ファイル**という
パイプラインが端から端まで貫通します。

第2章以降は、この通った管を太くしていく作業です。
**最初に管を通すことが、コンパイラ自作で最も重要な一歩です。**

---

## 目次

- [1.1 環境を整える](#11-環境を整える)
- [1.2 LLVM IR を手で書いてみる](#12-llvm-ir-を手で書いてみる)
- [1.3 この章で作る言語（暫定仕様）](#13-この章で作る言語暫定仕様)
- [1.4 プロジェクトの骨組みを作る](#14-プロジェクトの骨組みを作る)
- [1.5 util — 基礎部品](#15-util--基礎部品)
- [1.6 ① 字句解析器](#16--字句解析器)
- [1.7 ② AST](#17--ast)
- [1.8 ② 構文解析器](#18--構文解析器)
- [1.9 ④ コード生成器](#19--コード生成器)
- [1.10 main — 全部つなぐ](#110-main--全部つなぐ)
- [1.11 Makefile](#111-makefile)
- [1.12 テストを整える](#112-テストを整える)
- [1.13 動作確認](#113-動作確認)
- [1.14 この章のまとめと次章の予告](#114-この章のまとめと次章の予告)

---

## 1.1 環境を整える

### ✍️ 手を動かす：必要なものを確認する

```bash
clang --version          # C コンパイラ（＋ .ll を実行ファイルにする道具）
make --version           # ビルドツール
xcode-select -p          # macOS の開発ツール一式
```

**macOS で `clang` が無い場合：**

```bash
xcode-select --install
```

### LLVM を入れる

`clang` だけでも `.ll` ファイルをコンパイルできますが、
**`opt` / `lli` / `llvm-as` という 3 つの道具**が学習と デバッグに強力なので入れます。

```bash
brew install llvm
```

### ✅ 確認

```bash
$(brew --prefix llvm)/bin/llvm-config --version
```

この環境での結果：

```
22.1.8
```

**⚠️ PATH には入れないことを推奨します。**
`/usr/local/opt/llvm/bin` を PATH の先頭に置くと `clang` も Homebrew 版になり、
macOS の SDK を見つけられずに C のビルドが失敗することがあります。

**この教材の方針**：

| 用途 | 使うもの |
|---|---|
| C のビルド、`.ll` → 実行ファイル | **Apple clang**（`/usr/bin/clang`） |
| `opt` / `lli` / `llvm-as` | **Homebrew LLVM**（フルパスで呼ぶ） |

Makefile が自動的に振り分けるので、普段は意識しなくて済みます。

### 🤔 なぜ `llvm-config` が最初は見つからなかったのか

macOS には Apple 版の clang/LLVM が付属していますが、
**開発用のツール群（`opt`, `llc`, `llvm-config`）は含まれていません。**
Apple は最終製品（コンパイラ）だけを配り、LLVM の中間ツールは配らないのです。
だから `brew install llvm` で別途入れる必要があります。

詳しいツールの使い方は [../reference/toolchain.md](../reference/toolchain.md) にまとめてあります。

---

## 1.2 LLVM IR を手で書いてみる

**コード生成器を書く前に、生成する対象を自分の手で書きます。**
これをやらずにコード生成器を書き始めると、必ず迷子になります。

### ✍️ 手を動かす

```bash
mkdir -p /tmp/irlab && cd /tmp/irlab
cat > hello.ll <<'EOF'
define i32 @main() {
entry:
  ret i32 42
}
EOF
clang hello.ll -o hello
./hello
echo "exit code = $?"
```

### ✅ 確認

```
exit code = 42
```

**おめでとうございます。今、あなたは LLVM IR を書いてプログラムを作りました。**

### 📖 1 行ずつ読む

```llvm
define i32 @main() {
```

| 部分 | 意味 |
|---|---|
| `define` | 関数を定義する（本体つき）。本体なしの宣言は `declare` |
| `i32` | 戻り値の型。`i` + ビット数で整数型 |
| `@main` | 関数名。`@` は**グローバルな名前**の印 |
| `()` | 引数なし |

```llvm
entry:
```

**基本ブロック**のラベルです。基本ブロックとは
「途中で分岐せず、上から下へ一直線に実行される命令の並び」のこと。
最初のブロックは慣習的に `entry` と名付けます。

```llvm
  ret i32 42
```

関数から戻る命令。**`ret 42` ではなく `ret i32 42`** と型を書きます。
LLVM IR ではすべての値に型が必要です。

### 🤔 なぜ型を毎回書かせるのか

冗長に見えますが、これは IR を**生成する側**にとって利点です。
型を間違えた IR は、実行時に不思議な挙動をするのではなく、
その場で明確なエラーになります。

```
error: '%t0' defined with type 'i64' but expected 'i32'
```

### ✍️ もう 1 つ：関数を呼ぶ

これが第1章で最終的に生成する形です。

```bash
cat > wrap.ll <<'EOF'
define i64 @mython_main() {
entry:
  ret i64 42
}

define i32 @main() {
entry:
  %t0 = call i64 @mython_main()
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
EOF
clang wrap.ll -o wrap && ./wrap; echo "exit code = $?"
```

```
exit code = 42
```

### 📖 なぜ 2 つの関数に分けるのか

Mython の `int` は **64bit**（`i64`）です。しかし C の `main` は **`i32`** を返す約束です。

そこで方式を決めます。

> **ユーザーの `main` は `@mython_main` として出力し、
> `@main` はそれを呼んで `i64` → `i32` に変換するラッパにする。**

```llvm
  %t1 = trunc i64 %t0 to i32
```

`trunc`（truncate）は**ビット幅を縮める**変換命令です。

**🤔 なぜラッパ方式なのか**：`main` だけ戻り型を `i32` にする方法もありますが、
そうすると「`main` のときだけ特別」という分岐がコード生成器に入ります。
ラッパにしておけば、**`main` を他の関数と完全に同じ規則で生成できます。**
将来ランタイムの初期化処理を挟みたくなったときも、ここに置けます。

### ⚠️ もっと IR を学びたい場合

この先の章では `alloca` / `br` / `icmp` / `getelementptr` などを使います。
**[../reference/llvm-ir-primer.md](../reference/llvm-ir-primer.md) に、
手を動かして学べる入門（所要 30 分）を用意しました。**
第5章（変数）に入る前に、一度通しておくことを強く推奨します。

---

## 1.3 この章で作る言語（暫定仕様）

**⚠️ この章の文法は、正式な Mython の仕様とは違います。**

```ebnf
program ::= expr EOF
expr    ::= INT
```

つまり、**ファイルに整数を 1 つ書くだけ**の言語です。

```python
# コメントと空行は無視される
42
```

### 🤔 なぜ仕様と違うものを作るのか

正式な Mython なら、最小のプログラムはこうです。

```python
def main() -> int:
    return 42
```

しかしこれを第1章で実装しようとすると、
**キーワード・識別子・記号・型注釈・インデント（INDENT/DEDENT）・関数定義・return 文**を
一度に作ることになります。1 つでもバグると、どこが悪いのか切り分けられません。

第1章の目的は「**管を通すこと**」であって「言語を作ること」ではありません。
だから、パイプラインを通すのに最低限必要な機能だけを作ります。

| 章 | 受け付けるもの |
|---|---|
| ch1（今） | `42` |
| ch2 | `1 + 2 * (3 - 1)` |
| ch4 | 複数行、インデント |
| ch8 | `def main() -> int: return 42` ← **ここで正式な形になる** |

**これがインクリメンタル開発です。** 各段階で必ず動くものを保ちます。

---

## 1.4 プロジェクトの骨組みを作る

### ✍️ 手を動かす

```bash
cd ~/Desktop/mython           # 自分の作業ディレクトリ
mkdir -p src tests/cases examples build
```

作るファイルは 11 個です。

```
mython/
├── Makefile                 ビルドとテスト
├── .gitignore
├── src/
│   ├── util.h   util.c      基礎部品（メモリ・文字列・エラー）
│   ├── lexer.h  lexer.c     ① 字句解析
│   ├── ast.h    ast.c       AST の定義
│   ├── parser.h parser.c    ② 構文解析
│   ├── codegen.h codegen.c  ④ コード生成
│   └── main.c               全部つなぐ
└── tests/
    ├── run_tests.sh         テストランナー
    └── cases/*.my           テストケース
```

### 📖 依存関係の向き（設計の背骨）

```
main.c
  ├──▶ lexer.h  ──▶ util.h
  ├──▶ parser.h ──▶ ast.h ──▶ lexer.h ──▶ util.h
  └──▶ codegen.h ─▶ ast.h
```

**下向きにしか依存しません。** `lexer.c` が `parser.h` を include したら設計ミスです。

**🤔 なぜ厳しく守るのか**：循環依存はビルドを壊し、理解を壊し、
そして第16章以降の**セルフホスト（1 ファイルずつ Mython に移植する作業）を壊します。**
最初から守るのが一番安いです。

### ✍️ .gitignore

```bash
cat > .gitignore <<'EOF'
build/
tests/tmp/
a.out
*.ll
*.o
*.dSYM/
EOF
```

---

## 1.5 util — 基礎部品

まず全モジュールが使う土台を作ります。ここには 4 種類のものを置きます。

1. メモリ確保（`xmalloc`）
2. 伸長する文字列バッファ（`StrBuf`）— **IR の組み立てに使う**
3. ファイル入出力（`read_file` / `write_file`）
4. エラー終了（`error` / `error_at_pos`）

### ✍️ `src/util.h` を作る

→ 実際のファイル：[`src/util.h`](../../src/util.h)

要点だけ抜き出します。

```c
// free() は一切呼びません。calloc なので必ず 0 / NULL で初期化されます。
void *xmalloc(size_t size);

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_printf(StrBuf *sb, const char *fmt, ...);   // printf と同じ書式で追記
char *sb_str(StrBuf *sb);

char *read_file(const char *path);
void write_file(const char *path, const char *text);

_Noreturn void error(const char *fmt, ...);
_Noreturn void error_at_pos(const char *file, const char *line_start,
                            int line, int col, int len, const char *fmt, ...);
_Noreturn void internal_error(const char *file, int line, const char *fmt, ...);

#define UNREACHABLE() \
    internal_error(__FILE__, __LINE__, "到達しないはずのコードに来ました")
```

### 🤔 `free()` を書かない理由

```c
void *xmalloc(size_t size) {
    void *p = calloc(1, size);
    if (!p) error("メモリ確保に失敗しました (%zu バイト)", size);
    return p;
}
```

コンパイラは**1 ファイルを処理して終了する短命なプロセス**です。
AST は最後まで使われ続けるので、解放するタイミングは「プロセス終了時」しかありません。
`free` を書くと、コードが増え、二重解放バグの余地が生まれ、**得るものが何もありません。**

`malloc` ではなく `calloc` を使うのは重要です。
**全フィールドが 0 / NULL で初期化される**ので、
`Node` のような大きな構造体で初期化忘れによるバグを構造的に防げます。

### 🤔 なぜ StrBuf が必要か（IR 出力の鍵）

`fprintf(fp, ...)` で直接ファイルに書けば済むように見えます。
しかし IR 生成では「**後から前に戻って書き足したい**」ことが起きます。

- 関数本体を生成し終わってから、entry ブロックの先頭に全部の `alloca` を置きたい
- 関数の途中で文字列リテラルを見つけたら、**ファイル先頭の**定数セクションに追記したい

そこで、**出力先を用途別のバッファに分けて持ち、最後に連結**します。
`StrBuf` はそのための道具です。第9章（文字列）でこの設計が効いてきます。

### 📖 `sb_printf` の実装のコツ

```c
void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;

    // ① まず「何バイト必要か」を測る。
    //    vsnprintf は size に 0 を渡すと、書き込まずに必要バイト数だけを返す。
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    // ② 足りなければ容量を 2 倍にしていく
    size_t required = sb->len + (size_t)need + 1;   // +1 は NUL の分
    if (required > sb->cap) {
        while (sb->cap < required) sb->cap *= 2;
        char *newdata = xmalloc(sb->cap);
        memcpy(newdata, sb->data, sb->len + 1);
        sb->data = newdata;
    }

    // ③ 実際に書き込む
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)need;
}
```

**⚠️ `va_list` は 1 回しか使えません。** 2 回使うので `va_start` / `va_end` を 2 度書きます。
これを忘れると、環境によって動いたり壊れたりする厄介なバグになります。

### 📖 エラー報告：最初から丁寧に作る

```c
_Noreturn void error_at_pos(const char *file, const char *line_start,
                            int line, int col, int len, const char *fmt, ...)
```

「ファイル名:行:桁」を出し、**該当行を抜粋して下線を引きます**。

**🤔 なぜ第1章からこれを作るのか**：エラーメッセージは後回しにしたくなります。
しかし**コンパイラを開発している間、最初のユーザーは自分自身**です。
「syntax error」だけ出すコンパイラでデバッグするのは苦行です。
最初に 40 行書いておくと、以降ずっと楽になります。

---

## 1.6 ① 字句解析器

**文字列 → トークン列**の変換です。

```
"42\n"  →  [INT(42)] [EOF]
```

### ✍️ `src/lexer.h`

→ 実際のファイル：[`src/lexer.h`](../../src/lexer.h)

```c
typedef enum {
    TK_EOF,   // 入力の終わり
    TK_INT,   // 整数リテラル
    // ── 第2章以降で追加していく ──
    // TK_PUNCT, TK_IDENT, TK_KEYWORD, TK_STRING,
    // TK_NEWLINE, TK_INDENT, TK_DEDENT,
} TokenKind;

struct Token {
    TokenKind kind;

    // ── 位置情報（全トークンが必ず持つ）──
    const char *file;
    const char *line_start;  // この行の先頭（ソース抜粋表示に使う）
    int line;                // 1 起算
    int col;                 // 1 起算

    // ── ソース上の実体 ──
    const char *loc;   // 開始位置（複製しない）
    int len;

    // ── 値 ──
    long long ival;    // TK_INT
};
```

### 🤔 なぜ位置情報を全トークンに持たせるのか

これがないと、後の章で

```
error: 型 'int' と 'str' に演算子 '+' は適用できません
  --> foo.my:5:14
```

が**書けなくなります**。位置情報は AST を経由してエラーメッセージまで運ばれます。
**後から追加するのは非常に面倒**なので、最初から全トークンに持たせます。

### 🤔 なぜトークンは配列なのか（リンクリストではなく）

多くの教材（chibicc など）はトークンをリンクリストで繋ぎます。
**Mython では配列にします。**

```c
typedef struct {
    Token *data;
    int len;
    int cap;
} TokenVec;
```

理由は 3 つ。

1. **任意の先読みが O(1)** … 第5章で「`x` の次が `:` か」を判定するのに `peek_at(p, 1)` が必要。
   リンクリストだと辿る必要があります。
2. **セルフホストで自然** … Mython 側で `list[Token]` としてそのまま書けます（第16章）。
3. **デバッグしやすい** … デバッガで配列全体を一覧できます。

### ✍️ `src/lexer.c` の中心部

→ 実際のファイル：[`src/lexer.c`](../../src/lexer.c)

字句解析器は「状態を持って前に進むループ」です。

```c
typedef struct {
    const char *file;
    const char *src;         // ソース全体（先頭）
    const char *p;           // 現在の読み取り位置 ★これを進めていく
    const char *line_start;  // 現在の行の先頭
    int line;                // 現在の行番号
    TokenVec out;            // 出力先
} Lexer;
```

メインループはこうなります。

```c
while (*lx.p) {
    if (*lx.p == '\n') {              // 改行：行番号を更新
        lx.p++; lx.line++; lx.line_start = lx.p;
        continue;
    }
    if (*lx.p == ' ') { lx.p++; continue; }        // 空白は捨てる
    if (*lx.p == '\t') { /* エラー */ }             // タブは禁止
    if (*lx.p == '#') {                            // コメント：行末まで飛ばす
        while (*lx.p && *lx.p != '\n') lx.p++;
        continue;
    }
    if (is_digit(*lx.p)) { read_int(&lx); continue; }

    /* ここに来たら解釈できない文字 → エラー */
}
tv_push(&lx, TK_EOF, lx.p, 0);      // ★ 最後に必ず EOF を置く
```

### 📖 4 つの設計ポイント

**① `line` と `line_start` を常に最新に保つ**

改行を読んだ瞬間に両方を更新します。これでトークン生成時に位置が確定します。

```c
static Token *tv_push(Lexer *lx, TokenKind kind, const char *loc, int len) {
    ...
    t->line = lx->line;
    t->col = (int)(loc - lx->line_start) + 1;   // 桁は「行頭からの差」で計算
    ...
}
```

**桁番号を別途カウントしなくてよい**のがこの方法の利点です。
ポインタの差から計算できます。

**② 最後に必ず `TK_EOF` を置く**

これがあると、パーサが「配列の終わりを越えたか」を毎回気にせずに済みます。
`peek()` は常に有効なトークンを返せます。**境界チェックのバグを構造的に防ぐ**技法です。

**③ コメントは改行を消費しない**

```c
if (*lx.p == '#') {
    while (*lx.p && *lx.p != '\n') lx.p++;   // ★ '\n' は残す
    continue;
}
```

改行を残すことで、次の周回で改行処理（`line++`）が走ります。
**行番号がずれるバグを防ぎます。**
第4章で `NEWLINE` トークンを出すようになると、これがさらに重要になります。

**④ タブを字句エラーにする**

```c
if (*lx.p == '\t') {
    error_at(&tmp, "タブ文字は使えません。半角スペースを使ってください");
}
```

言語仕様 2.4 の決定です。タブ幅の解釈（4 か 8 か）でインデントの意味が変わるコードは、
言語として欠陥です。**新しい言語なら禁止すべき**で、実装も単純になります。

### 📖 整数の読み取り

```c
static void read_int(Lexer *lx) {
    const char *start = lx->p;

    // 桁区切りの '_' を飛ばしながら数字を集める（1_000 == 1000）
    char digits[64];
    int n = 0;
    while (is_digit(*lx->p) || *lx->p == '_') {
        if (*lx->p == '_') { lx->p++; continue; }
        if (n < (int)sizeof(digits) - 1) digits[n++] = *lx->p;
        lx->p++;
    }
    digits[n] = '\0';

    // 123abc のような不正なリテラルを弾く
    if (isalpha((unsigned char)*lx->p) || *lx->p == '_') { /* エラー */ }

    // オーバーフローを errno で検出する
    errno = 0;
    char *end;
    long long v = strtoll(digits, &end, 10);
    if (errno == ERANGE) { /* エラー：int の範囲を超えている */ }

    Token *t = tv_push(lx, TK_INT, start, (int)(lx->p - start));
    t->ival = v;
}
```

**⚠️ 3 つの落とし穴**

1. **`isalpha` に `char` をそのまま渡さない**
   `<ctype.h>` の関数は引数が負の値だと未定義動作です。非 ASCII バイト（0x80 以上）は
   `char` が符号付きの環境で負になります。**必ず `(unsigned char)` にキャスト**します。

2. **`123abc` を黙って `123` と読まない**
   数字の直後に識別子文字が続いていたらエラーにします。
   これをしないと、後の章でタイプミスが謎のバグになります。

3. **オーバーフローを検出する**
   `strtoll` は失敗を戻り値で表現できない（`LLONG_MAX` は正当な値でもある）ので、
   **`errno = 0` にリセットしてから呼び、`ERANGE` を確認**します。

### ✍️ デバッグの道具：`--dump-tokens`

**これは「あると便利なおまけ」ではなく、開発速度を決める中核ツールです。**

```c
void dump_tokens(TokenVec toks) {
    for (int i = 0; i < toks.len; i++) {
        Token *t = &toks.data[i];
        printf("%3d  %-8s  %d:%-3d  ", i, token_kind_name(t->kind), t->line, t->col);
        switch (t->kind) {
            case TK_INT: printf("%lld", t->ival); break;
            case TK_EOF: break;
            default: printf("%.*s", t->len, t->loc); break;
        }
        printf("\n");
    }
}
```

**さらに重要な理由**：第16章で Mython 版の字句解析器を書いたとき、
**C 版と Mython 版のトークン列を `diff` して検証します。**
そのための正解出力を作る道具が、これです。

---

## 1.7 ② AST

### ✍️ `src/ast.h`

→ 実際のファイル：[`src/ast.h`](../../src/ast.h)

```c
typedef enum {
    ND_INT,  // 整数リテラル → ival
    // ── 第2章以降で追加していく ──
    // ND_BINOP, ND_UNARY, ND_VAR, ND_CALL, ND_IF, ND_WHILE, ND_FUNC, ...
} NodeKind;

struct Node {
    NodeKind kind;

    Token *tok;      // ★ このノードの代表トークン（エラー報告に必須）
    // struct Type *type;   // 第5章で sema が埋める

    long long ival;  // ND_INT

    // Node *lhs, *rhs;     // 第2章
    // Node *next;          // 第4章（文のリスト）
};
```

### 🤔 なぜ「全部入り」の 1 構造体なのか

本来はノード種別ごとに構造体を分けるべきです（共用体を使う）。
しかしこの教材では 1 つの構造体に全フィールドを持たせます。

| 理由 | 説明 |
|---|---|
| 読みやすい | 「1 個の構造体を見れば全部わかる」 |
| C で安全 | 共用体を安全に扱うコードは冗長になる |
| 実績がある | chibicc, 8cc, tcc など小型 C コンパイラが同じ方式 |
| 無駄が小さい | 1 ノード 150 バイト程度。10 万ノードでも 15MB |

**⚠️ 美しくはありません。** それは認めます。
しかし「美しい設計にこだわって完成しない」より「動くものを完成させる」ほうが価値があります。

### ★ `Token *tok` は絶対に省略しない

```c
Node *new_node(NodeKind kind, Token *tok) {
    Node *n = xmalloc(sizeof(Node));   // calloc なので他は 0 / NULL
    n->kind = kind;
    n->tok = tok;
    return n;
}
```

第1章ではまだ使いません。しかし第5章で型エラーを報告するとき、
**このフィールドがあるかどうかで実装の可否が決まります。**

```
error: 型が一致しません
  --> examples/bad.my:5:14      ← これは n->tok から出す
```

後から全ノードに追加するのは非常に面倒です。**今入れます。**

### ✍️ デバッグの道具：`--dump-ast`

AST を **S 式**で出力します。

```c
static void dump(Node *n, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    if (!n) { printf("(nil)\n"); return; }

    switch (n->kind) {
        case ND_INT: printf("(int %lld)\n", n->ival); break;
        default: UNREACHABLE();
    }
}
```

**🤔 なぜ S 式なのか**：**テキストとして比較できる**からです。

- パーサのバグ調査：「意図した木になっているか」が一目でわかる
- 第2章の優先順位テスト：`(binop + (int 1) (binop * (int 2) (int 3)))` を目で確認できる
- **第17章**：Mython 版パーサの出力と `diff` する正解になる

---

## 1.8 ② 構文解析器

**トークン列 → AST** の変換です。手法は**再帰下降構文解析**を使います。

> **文法規則 1 つ ＝ 関数 1 つ**

この対応を守ると、文法定義（[../spec/grammar.md](../spec/grammar.md)）を見ながら
機械的にコードが書けます。

### ✍️ `src/parser.c`

→ 実際のファイル：[`src/parser.c`](../../src/parser.c)

```c
typedef struct {
    TokenVec toks;
    int pos;        // 今どこを見ているか
} Parser;
```

### 📖 4 つの基本部品

**パーサ全体は、この 4 つの関数だけで書けます。**

```c
// ① 現在のトークンを覗く（消費しない）
static Token *peek(Parser *p) { return &p->toks.data[p->pos]; }

// ② 現在のトークンを消費して返す（1 つ進む）
static Token *advance(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TK_EOF) p->pos++;   // ★ EOF より先には進まない
    return t;
}

// ③ 期待する種類なら消費、違えばエラー
static Token *expect(Parser *p, TokenKind kind, const char *what) {
    Token *t = peek(p);
    if (t->kind != kind)
        error_at(t, "%s が必要です（実際は %s）", what, token_kind_name(t->kind));
    return advance(p);
}
```

**⚠️ `advance` が EOF で止まる設計が重要です。**
これで「配列の範囲外を読む」バグが構造的に起きなくなります。
パーサにバグがあっても、無限ループやクラッシュではなく
「EOF が来た」という扱いやすい状態に収束します。

### 📖 文法規則を関数にする

```c
// expr ::= INT
static Node *expr(Parser *p) {
    Token *t = expect(p, TK_INT, "整数");
    return new_int_node(t, t->ival);
}

// program ::= expr EOF
static Node *program(Parser *p) {
    Node *n = expr(p);

    // 式を読み終えたら EOF のはず
    Token *t = peek(p);
    if (t->kind != TK_EOF)
        error_at(t, "式の後に余分なトークンがあります");

    return n;
}
```

**この `expr()` が、第2章で優先順位の階層に置き換わります。**

```
expr ::= add_expr
add_expr ::= mul_expr { ("+"|"-") mul_expr }
mul_expr ::= unary { ("*"|"//"|"%") unary }
unary ::= ("-"|"+") unary | primary
primary ::= INT | "(" expr ")"
```

**関数を 4 つ足すだけで、演算子の優先順位が実装できます。**
その仕組みの解説は [../spec/grammar.md](../spec/grammar.md) 第5節にあります。

### 📖 `program` で EOF を確認する意味

```c
if (t->kind != TK_EOF)
    error_at(t, "式の後に余分なトークンがあります");
```

これを書かないと、`1 2` という入力が**エラーなく `1` として通ってしまいます**。
「読めるところまで読んで残りは無視」は、コンパイラとして最悪の振る舞いです。
**入力を最後まで消費したことを必ず確認します。**

---

## 1.9 ④ コード生成器

**AST → LLVM IR テキスト**の変換です。ここが第1章の山場です。

### ✍️ `src/codegen.c`

→ 実際のファイル：[`src/codegen.c`](../../src/codegen.c)

### 📖 出力バッファを 4 つに分ける

```c
typedef struct {
    StrBuf header;   // source_filename, target triple, 型定義
    StrBuf globals;  // グローバル変数・文字列定数
    StrBuf decls;    // declare（外部関数宣言）
    StrBuf body;     // 関数定義

    int tmp_counter; // 一時値 %tN の連番
} Emitter;
```

第1章では `globals` と `decls` は空のままです。
**それでも今から用意します。** 第9章で文字列リテラルを実装するとき、
関数本体の生成中に「ファイル先頭の定数セクションに追記」する必要が出るからです。

最後に規定の順序で連結します。

```c
sb_printf(&out, "%s", sb_str(&e.header));
sb_printf(&out, "%s", sb_str(&e.globals));
sb_printf(&out, "%s", sb_str(&e.decls));
sb_printf(&out, "%s", sb_str(&e.body));
```

### ★ `gen_expr` の約束（コード生成器の設計の核心）

```c
// 「式を評価する命令列を body に出力し、
//   結果の値が入っている場所の名前を返す」
static char *gen_expr(Emitter *e, Node *n);
```

**この 1 つの約束が、コード生成器の全体設計を決めます。**
第2章以降、`gen_expr` に `case` を足していくだけで機能が増えます。

```c
static char *gen_expr(Emitter *e, Node *n) {
    switch (n->kind) {
        case ND_INT: {
            // 整数リテラルは命令を出す必要すらない。
            // LLVM は即値をオペランドに直接書けるので（add i64 42, 1）、
            // 「42」という文字列をそのまま返す。
            char *buf = xmalloc(24);
            snprintf(buf, 24, "%lld", n->ival);
            return buf;
        }
        default:
            UNREACHABLE();
    }
}
```

### 🤔 「レジスタ名を返す」とはどういうことか

第2章で `1 + 2` を生成するとき、こうなります。

```c
case ND_BINOP: {
    char *l = gen_expr(e, n->lhs);      // → "1"
    char *r = gen_expr(e, n->rhs);      // → "2"
    char *t = new_tmp(e);               // → "%t0"
    sb_printf(&e->body, "  %s = add i64 %s, %s\n", t, l, r);
    return t;                            // → "%t0"
}
```

出力：

```llvm
  %t0 = add i64 1, 2
```

**「値の置き場所を文字列で表す」**という発想が、テキスト IR 生成の要点です。
即値（`"1"`）とレジスタ（`"%t0"`）を同じ `char *` で扱えるので、
場合分けが不要になります。

### ⚠️ 一時値の名前は必ず英字始まりにする

```c
static char *new_tmp(Emitter *e) {
    char *buf = xmalloc(24);
    snprintf(buf, 24, "%%t%d", e->tmp_counter++);
    return buf;
}
```

**`%0`, `%1` のような数値名を自分で使ってはいけません。**
LLVM は名前のない値に暗黙の連番を振るので、衝突してこうなります。

```
error: instruction expected to be numbered '%2'
```

原因が非常に分かりにくいエラーです。`%t0` のように英字で始めれば絶対に衝突しません。
（[../design/ir-conventions.md](../design/ir-conventions.md) 規約 R4）

### 📖 関数の生成

```c
static void gen_mython_main(Emitter *e, Node *ast) {
    e->tmp_counter = 0;   // ★ 関数ごとに連番をリセット

    sb_printf(&e->body, "define i64 @mython_main() {\n");
    sb_printf(&e->body, "entry:\n");

    char *val = gen_expr(e, ast);
    sb_printf(&e->body, "  ret i64 %s\n", val);

    sb_printf(&e->body, "}\n");
}

static void gen_c_main(Emitter *e) {
    e->tmp_counter = 0;

    sb_printf(&e->body, "\ndefine i32 @main() {\nentry:\n");

    char *t0 = new_tmp(e);
    sb_printf(&e->body, "  %s = call i64 @mython_main()\n", t0);

    char *t1 = new_tmp(e);
    sb_printf(&e->body, "  %s = trunc i64 %s to i32\n", t1, t0);

    sb_printf(&e->body, "  ret i32 %s\n", t1);
    sb_printf(&e->body, "}\n");
}
```

**⚠️ `tmp_counter` は関数ごとにリセットします。**
`%tN` は関数内でのみ有効な名前なので、関数をまたいで通し番号にする必要はありません。
リセットすると生成される IR が読みやすくなります。

### ★ target triple を必ず出力する

```c
if (MYTHON_TARGET_TRIPLE[0])
    sb_printf(&e.header, "target triple = \"%s\"\n", MYTHON_TARGET_TRIPLE);
```

これを書かないと、`clang` が毎回こう警告します。

```
warning: overriding the module target triple with x86_64-apple-macosx26.0.0
         [-Woverride-module]
```

triple の値は Makefile がビルド時に埋め込みます（次節）。

---

## 1.10 main — 全部つなぐ

### ✍️ `src/main.c`

→ 実際のファイル：[`src/main.c`](../../src/main.c)

パイプラインは 20 行で書けます。

```c
char *src = read_file(opt.input);                    // 読む

TokenVec toks = tokenize(opt.input, src);            // ① 字句解析
if (opt.stage == STAGE_DUMP_TOKENS) { dump_tokens(toks); return 0; }

Node *ast = parse(toks);                             // ② 構文解析
if (opt.stage == STAGE_DUMP_AST) { dump_ast(ast); return 0; }

// ③ 意味解析・型検査 — 第5章で sema(ast) がここに入る

char *ir = codegen(ast, opt.input);                  // ④ コード生成

if (opt.stage == STAGE_EMIT_IR) { /* .ll を出して終了 */ }

// ⑤ clang に丸投げ
char *ll = ll_path_for(opt.output);
write_file(ll, ir);
sb_printf(&cmd, "clang %s '%s' -o '%s'", opt.opt_level, ll, opt.output);
int rc = system(sb_str(&cmd));
```

**この構造が第20章まで変わりません。** 第5章で `sema(ast)` が 1 行増えるだけです。

### 📖 途中で止められることが重要

```
--dump-tokens  → ① まで
--dump-ast     → ② まで
-S             → ④ まで
（何もなし）    → ⑤ 実行ファイルまで
```

**各段階の出力を単独で確認できる**設計にしておくと、
バグの切り分けが「どの段階で壊れたか」の二分探索になります。

### 📖 clang が失敗したときの親切さ

```c
if (rc != 0) {
    fprintf(stderr,
            "error: clang の実行に失敗しました（生成した IR に問題があります）\n"
            "  生成された IR を残しました: %s\n"
            "  次のコマンドで詳しく調べられます:\n"
            "    clang %s -o /dev/null\n",
            ll, ll);
    return 1;
}
```

**ここに来たら、それは自分のコンパイラのバグです。**
`.ll` を消さずに残し、調査コマンドまで表示します。
第7章（制御構文）以降、この道が何度も役に立ちます。

**⚠️ `system()` を使うのは手抜きです。**
シェルのメタ文字を含むファイル名で問題が起きます（`'` で囲んで軽減はしています）。
`fork` + `execvp` に直すのは練習問題としておきます。

---

## 1.11 Makefile

### ✍️ `Makefile`

→ 実際のファイル：[`Makefile`](../../Makefile)

### ★ target triple の自動取得（実際に踏んだ落とし穴）

```makefile
HOST_TRIPLE := $(shell $(CC) -S -emit-llvm -x c /dev/null -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p')
CFLAGS  += -DMYTHON_TARGET_TRIPLE='"$(HOST_TRIPLE)"'
```

**⚠️ 直感的には `clang -print-target-triple` を使いたくなります。これは間違いです。**

実際に試した結果：

```bash
$ clang -print-target-triple
x86_64-apple-darwin25.5.0          # ← これを .ll に書くと…

$ clang t.ll -o t
warning: overriding the module target triple with x86_64-apple-macosx26.0.0
         [-Woverride-module]       # ← 警告が消えない！

$ clang -S -emit-llvm -x c /dev/null -o - | grep 'target triple'
target triple = "x86_64-apple-macosx26.0.0"    # ← こちらが正解
```

macOS の clang は、`-print-target-triple`（`darwin<カーネル版>`）と
IR に書く triple（`macosx<OS版>`）で**異なる表記**を使うのです。

**「clang 自身に空の C ファイルの IR を吐かせて、そこから抜き出す」**のが確実な方法です。
Linux でも同じ方法が使えます（`x86_64-pc-linux-gnu` などが取れます）。

### ★ ヘッダの依存関係を自動生成する

```makefile
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)
```

**⚠️ これがないと、`lexer.h` を直したのに `parser.o` が再ビルドされません。**
構造体のレイアウトがずれた状態でリンクされ、**原因不明のクラッシュ**に悩まされます。
コンパイラ開発では致命的なので、最初から入れます。

`-MMD` が `.d` ファイル（依存関係）を生成し、`-include` がそれを読み込みます。

### 📖 Homebrew LLVM の場所を探す

```makefile
LLVM_BIN := $(shell brew --prefix llvm 2>/dev/null)/bin
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(patsubst %/,%,$(dir $(shell which opt 2>/dev/null)))
endif
```

`brew` がない環境（Linux）でも PATH から探すようにフォールバックします。

### 📖 AddressSanitizer ターゲット

```makefile
asan:
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(SRCS) -o build/mythonc-asan
```

**★ コンパイラ開発では必ずポインタのバグが出ます。** ASan があると、

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow
    #0 0x... in tokenize src/lexer.c:87
```

と**原因の行を教えてくれます**。デバッグ時間が 10 分の 1 になります。

### ✅ 確認

```bash
make info
```

この環境での実際の出力：

```
CC           = clang
HOST_TRIPLE  = x86_64-apple-macosx26.0.0
LLVM_BIN     = /usr/local/opt/llvm/bin
SRCS         = src/ast.c src/codegen.c src/lexer.c src/main.c src/parser.c src/util.c
```

```bash
make
```

**⚠️ 警告が 1 つも出ないことを確認してください。**
`-Wall -Wextra` の警告を放置する習慣がつくと、本物のバグを見逃します。

---

## 1.12 テストを整える

### 📖 テストの形式

期待値を**テストケース自身の先頭コメント**に書きます。

```python
# EXIT: 42
42
```

```python
# ERROR: タブ文字は使えません
	42
```

| 記法 | 意味 |
|---|---|
| `# EXIT: n` | コンパイル・実行して終了コードが n |
| `# OUTPUT: text` | 標準出力が text（複数行は行ごとに書く。第7章から使う） |
| `# ERROR: msg` | **コンパイルが失敗**し、stderr に msg を含む |

**🤔 なぜこの形式か**：期待値を別ファイル（`.expected`）に置くと管理が 2 倍になります。
この方式なら**`.my` を 1 個置くだけでテストが 1 個増えます。**
心理的な障壁が下がり、テストを書く習慣がつきます。

### ✍️ `tests/run_tests.sh`

→ 実際のファイル：[`tests/run_tests.sh`](../../tests/run_tests.sh)

要点：

```bash
want_exit="$(sed -n 's/^# *EXIT: *//p'   "$case_file" | head -1)"
want_error="$(sed -n 's/^# *ERROR: *//p' "$case_file" | head -1)"

compile_err="$("$MYTHONC" "$case_file" -o "$exe" 2>&1 >/dev/null)"
compile_rc=$?
```

**⚠️ `2>&1 >/dev/null` の順序に注意。** これは
「stderr を（現在の）stdout に繋いでから、stdout を捨てる」なので、
**stderr だけが変数に入ります。** 順序を逆にすると意図と違う動作になります。

### ✍️ テストケースを作る

**正常系**（5 個）

```bash
tests/cases/int_42.my            # 42          → EXIT 42
tests/cases/int_zero.my          # 0           → EXIT 0
tests/cases/int_255.my           # 255         → EXIT 255
tests/cases/int_underscore.my    # 1_0_0       → EXIT 100
tests/cases/comment_and_blank.my # コメント+空行 → EXIT 7
```

**エラー系**（6 個）

```bash
tests/cases/err_empty.my       # 空ファイル
tests/cases/err_tab.my         # タブ
tests/cases/err_bad_char.my    # @
tests/cases/err_trailing.my    # 1 2
tests/cases/err_overflow.my    # 99999999999999999999999
tests/cases/err_num_suffix.my  # 123abc
```

### ★ エラー系テストを最初から書く

**正常系だけテストするのは半分しかテストしていません。**
コンパイラの価値の半分は「**間違いを正しく指摘すること**」です。

エラーメッセージをテストしておくと、
リファクタリングでエラー報告が壊れたことに気づけます。

### ⚠️ 終了コードの落とし穴

```bash
tests/cases/int_255.my   # → 255 : OK
```

**256 以上は使えません。** プロセスの終了コードは OS が下位 8bit しか伝えないので、
`256` は `0` になります。

```python
# EXIT: 0
256      # ← これは 0 になってしまう（テストの意図が曖昧になる）
```

だからテストは 255 以下にします。
第7章で `print` が使えるようになれば、`# OUTPUT:` で任意の値を検証できます。

---

## 1.13 動作確認

**ここからは実際にこの環境で実行した結果です。**

### ✅ ビルド

```bash
make
```

警告 0 件でビルドできます。

### ✅ テスト全実行

```bash
make test
```

```
  ok    comment_and_blank.my (exit=7)
  ok    err_bad_char.my (error)
  ok    err_empty.my (error)
  ok    err_num_suffix.my (error)
  ok    err_overflow.my (error)
  ok    err_tab.my (error)
  ok    err_trailing.my (error)
  ok    int_255.my (exit=255)
  ok    int_42.my (exit=42)
  ok    int_underscore.my (exit=100)
  ok    int_zero.my (exit=0)

────────────────────────────────
全 11 件パス
```

### ✅ ① 字句解析を見る

```bash
./build/mythonc --dump-tokens tests/cases/comment_and_blank.my
```

```
  0  INT       6:1    7
  1  EOF       8:1    
```

**コメントと空行が消え、`7` だけが残っています。**
`6:1` は「6 行 1 桁目」。位置情報が正しく追跡されています。

### ✅ ② 構文解析を見る

```bash
./build/mythonc --dump-ast tests/cases/int_42.my
```

```
(int 42)
```

### ✅ ④ 生成された IR を見る

```bash
./build/mythonc -S tests/cases/int_42.my
```

```llvm
; Generated by mythonc
source_filename = "tests/cases/int_42.my"
target triple = "x86_64-apple-macosx26.0.0"

define i64 @mython_main() {
entry:
  ret i64 42
}

define i32 @main() {
entry:
  %t0 = call i64 @mython_main()
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
```

**1.2 節で手書きした `wrap.ll` と同じ形です。**
自分の手で書いた IR を、コンパイラが生成できるようになりました。

### ✅ ⑤ 実行ファイルを作って動かす

```bash
./build/mythonc tests/cases/int_42.my -o /tmp/a42
/tmp/a42
echo "exit=$?"
```

```
exit=42
```

### ✅ おまけ：LLVM の道具で遊ぶ

**IR をそのまま実行する（コンパイル不要）**

```bash
./build/mythonc -S tests/cases/int_42.my -o /tmp/i.ll
$(brew --prefix llvm)/bin/lli /tmp/i.ll; echo "lli exit=$?"
```

```
lli exit=42
```

**最適化させてみる**

```bash
$(brew --prefix llvm)/bin/opt -O2 -S /tmp/i.ll | sed -n '/define/,/^}/p'
```

```llvm
define noundef i64 @mython_main() local_unnamed_addr #0 {
entry:
  ret i64 42
}
define noundef i32 @main() local_unnamed_addr #0 {
entry:
  ret i32 42
}
```

**`call` と `trunc` が消え、`ret i32 42` になりました。**
LLVM が `@mython_main` をインライン展開し、定数を畳み込んだのです。

**これが「素朴に生成して LLVM に任せる」戦略の実例です。**
私たちがラッパ関数を挟んだコストは、最適化で完全に消えました。

### ✅ エラーメッセージを確認する

```bash
./build/mythonc tests/cases/err_tab.my -o /tmp/x
```

```
error: タブ文字は使えません。半角スペースを使ってください
  --> tests/cases/err_tab.my:2:1
  |
2 | 	42
  | ^
```

```bash
./build/mythonc tests/cases/err_trailing.my -o /tmp/x
```

```
error: 式の後に余分なトークンがあります
  --> tests/cases/err_trailing.my:2:3
  |
2 | 1 2
  |   ^
```

```bash
./build/mythonc tests/cases/err_overflow.my -o /tmp/x
```

```
error: 整数リテラルが int の範囲 (64bit) を超えています
  --> tests/cases/err_overflow.my:2:1
  |
2 | 99999999999999999999999
  | ^^^^^^^^^^^^^^^^^^^^^^^
```

**下線が正しい範囲に引かれ、桁も合っています。**
この土台があるので、第3章では表示を洗練させるだけで済みます。

---

## 1.14 この章のまとめと次章の予告

### できたこと

```
✅ 環境構築（LLVM 22.1.8）
✅ LLVM IR を手で書いて動かした
✅ ① 字句解析器（整数・コメント・空白・タブ禁止・エラー検出）
✅ ② AST と構文解析器（再帰下降の骨格）
✅ ④ コード生成器（4 バッファ方式、target triple、main ラッパ）
✅ ⑤ clang 連携で実行ファイル生成
✅ デバッグ道具（--dump-tokens / --dump-ast / -S）
✅ テストランナーと 11 件のテスト（正常系 5 + エラー系 6）
✅ Makefile（triple 自動取得、ヘッダ依存自動生成、ASan）
```

### この章で入れた「将来への投資」

第1章では使わないのに作ったものがあります。**すべて意図的です。**

| 投資 | 回収する章 |
|---|---|
| `Token` の位置情報（file/line/col/line_start） | ch3（診断）、ch5（型エラー） |
| `Node.tok` | ch5 以降のすべてのエラー報告 |
| `StrBuf` と 4 バッファ方式 | ch9（文字列リテラルをグローバルに出す） |
| トークンを配列で保持 | ch5（2 トークン先読み）、ch16（Mython 移植） |
| `--dump-tokens` / `--dump-ast` | ch16 / ch17（**セルフホストの検証に使う正解出力**） |
| `error_at_pos` の抜粋表示 | ch3 以降ずっと |
| ヘッダ依存の自動生成 | 全章（謎のクラッシュを防ぐ） |
| `UNREACHABLE()` | 全章（ユーザーのミスと自分のバグの区別） |

**「今は要らないが後で必ず要るもの」を最初に入れておく**のが、
このプロジェクトを完走させるコツです。

### ⚠️ 逆に、意図的にやらなかったこと

| やらなかったこと | 理由 |
|---|---|
| 関数定義・変数・型検査 | ch5, ch8 で。今やると切り分けができない |
| `free()` | 短命プロセスなので不要（[../design/memory-model.md](../design/memory-model.md)） |
| 複数エラーの報告 | エラー回復は独立した難問。最初のエラーで exit する |
| LLVM C API | テキスト出力方式を採用（[../00-introduction.md](../00-introduction.md) 0.3 節） |
| `phi` 命令 | 全部 alloca 方式（ch5 で本格化） |

### ✍️ commit する

```bash
git add -A
git commit -m "第1章: 環境構築と最小コンパイラ（整数を返す）"
```

**各章末で必ず commit してください。**
第20章のブートストラップで何かが壊れたとき、「どの章まで戻れば動くか」がわかります。

---

## 次章：第2章 四則演算と演算子の優先順位

**達成目標**

```python
# EXIT: 7
1 + 2 * (3 - 1) // 2 + 3
```

**やること**

| ファイル | 作業 |
|---|---|
| `lexer.c` | `TK_PUNCT` を追加。`+ - * / // % ** ( )` を読む |
| `ast.h/c` | `ND_BINOP` / `ND_UNARY` を追加。`lhs` / `rhs` / `op` フィールドを使う |
| `parser.c` | **優先順位の階層**を作る（`add_expr` → `mul_expr` → `unary` → `power` → `primary`） |
| `codegen.c` | `gen_expr` に `ND_BINOP` の `case` を追加。`add`/`sub`/`mul`/`sdiv`/`srem` |

**学ぶ中心概念**

- 優先順位を**関数の呼び出し階層の深さ**で表現する技法
- 左結合（`while` ループ）と右結合（再帰）の書き分け
- `-x` に LLVM の `neg` 命令はない（`sub i64 0, x` を使う）
- `int` は符号付きなので `sdiv`（`udiv` ではない）

**予習**：[../spec/grammar.md](../spec/grammar.md) の第5節「式（優先順位の階層）」に、
`1 + 2 * 3` がどう解析されるかの完全なトレースがあります。

### 🤔 第2章に入る前の練習問題

第1章のコードを触って慣れておくと、第2章が楽になります。

1. **16 進リテラルを実装する**（`0xFF` → 255）
   `read_int` に分岐を足します。言語仕様 2.7 に定義があります。
2. **`--dump-tokens` に列幅を揃えた `len` の表示を足す**
   デバッグ出力を自分好みに改造してみる。
3. **`err_num_suffix.my` のエラーメッセージに「ヒント」を足す**
   `123abc` に対して「数値と識別子の間に空白が必要かもしれません」と出す。
4. **`make asan` でビルドして全テストを走らせる**
   メモリバグがないことを確認する。ASan の使い方に慣れておく。
