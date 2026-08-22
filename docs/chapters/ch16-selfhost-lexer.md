# 第16章 Mython で字句解析器を書く

> **この章のゴール**
> `selfhost/lexer.my`（Mython 製の字句解析器）が、
> **C 版とまったく同じトークン列を出す。**
>
> ```bash
> $ ./build/mythonc selfhost/dump_tokens.my -o build/stage1-lexer
> $ ./build/stage1-lexer tests/cases/class_basic.my > mine.txt
> $ ./build/mythonc --dump-tokens tests/cases/class_basic.my > theirs.txt
> $ diff mine.txt theirs.txt && echo "一致"
> 一致
> ```

**ここから第V部です。作るものが変わります。**

```
第1〜15章： C で Mython コンパイラを書く（src/*.c）
第16章〜 ： Mython で Mython コンパイラを書く（selfhost/*.my）
```

**★ この部の強みは「正解が手元にある」ことです。**
C 版が同じ入力に対する答えを持っているので、
**どこで間違えたかが diff で必ず分かります。**
ゼロから書くのではなく、**答え合わせをしながら移植する**作業です。

---

## 目次

- [16.1 移植の心得](#161-移植の心得)
- [16.2 何を「同じ」にするのか](#162-何を同じにするのか)
- [16.3 token.my — トークンの表現](#163-tokenmy--トークンの表現)
- [16.4 lexer.my — 移植する](#164-lexermy--移植する)
- [16.5 ポインタが無い言語への移し方](#165-ポインタが無い言語への移し方)
- [16.6 検証：全ケースで diff](#166-検証全ケースで-diff)
- [16.7 動作確認](#167-動作確認)
- [16.8 まとめと次章の予告](#168-まとめと次章の予告)

---

## 16.1 移植の心得

### ⚠️ 機械的に移す。工夫しない

[self-hosting.md](../design/self-hosting.md) 6 節に書いた約束です。

| やること | やらないこと |
|---|---|
| C 版の関数を 1 つずつ同じ名前で移す | 「もっと良い書き方」に変える |
| 判定の順序をそのまま保つ | 条件をまとめて短くする |
| 変数名も揃える | 名前を「改善」する |
| C 版のコメントも持っていく | 説明を省略する |

**🤔 なぜ改善してはいけないのか**

出力が食い違ったときに、原因が 2 種類に増えるからです。

```
移植ミス   … 元の意味を変えてしまった
改善のバグ … 改善したつもりの部分が間違っていた
```

**切り分けられない状態を自分で作らないこと。**
改善したくなったら、**まず一致させてから、C 版と一緒に直します。**

> **★ これは第11章「素朴な脱糖だとどうなるか」や
> 第15章「測ってから直す」と同じ規律です。**
> **一度に 1 つのことだけを変える。**

---

## 16.2 何を「同じ」にするのか

### 📖 比べるのは `--dump-tokens` の出力

```
  0  KEYWORD   3:1    class
  1  IDENT     3:7    Token
  2  PUNCT     3:12   :
  3  NEWLINE   3:13   
  4  INDENT    4:5    
```

C 版はこう出しています。

```c
printf("%3d  %-8s  %d:%-3d  ", i, token_kind_name(t->kind), t->line, t->col);
```

**★ 書式まで含めて完全一致を狙います。**
「意味が同じ」ではなく「バイト列が同じ」にすると、
**diff が 1 行でも出たら負け**という明快な基準になります。

### ⚠️ 桁揃えのために足りないもの

Mython には `printf` の桁揃え（`%3d` / `%-8s`）がありません。
**ライブラリに足します**（第14章の境界線のとおり、言語には触りません）。

```python
# lib/strings.my
def lpad(s: str, width: int) -> str:    # 右寄せ（左に空白）
def rpad(s: str, width: int) -> str:    # 左寄せ（右に空白）
```

```python
# selfhost/dump_tokens.my
strings.lpad(str(i), 3) + "  " + strings.rpad(kind_name, 8) + "  " + ...
```

### 📖 この章で比べないもの

| | 理由 |
|---|---|
| **エラーメッセージ** | 診断の描画（`diag.c`）はまだ移植していません。stage1 は `panic` で止まります |
| 実行速度 | 第20章の後に測ります |
| 内部のデータ構造 | 出力が同じなら、持ち方は違ってよい（16.5 節） |

**★ 一度に 1 つ。** この章で保証するのは
**「正しいソースに対するトークン列」だけ**です。

---

## 16.3 token.my — トークンの表現

### ✍️ 種類は int の定数にする

```python
# selfhost/token.my
TK_EOF: int = 0
TK_INT: int = 1
TK_PUNCT: int = 2
TK_IDENT: int = 3
TK_KEYWORD: int = 4
TK_STR: int = 5
TK_NEWLINE: int = 6
TK_INDENT: int = 7
TK_DEDENT: int = 8

def kind_name(kind: int) -> str:
    if kind == TK_EOF:
        return "EOF"
    ...
```

**⚠️ Mython には enum がありません。**
グローバル定数（言語仕様 6.2）で代用します。初期化式が
コンパイル時定数なので、そのまま `int` の定数として使えます。

**★ ここは「工夫しない」の例外です。** C の `enum` をそのまま移す方法が
無いので、**表現だけは変えざるをえません**。値と順序は C 版に揃えます。

### ✍️ Token クラス

```python
class Token:
    kind: int
    line: int
    col: int

    raw: str      # ソース上の見た目（PUNCT/IDENT/KEYWORD/STR）
    text: str     # 値（STR はエスケープ解決後、IDENT/KEYWORD は名前）
    ival: int     # TK_INT の値
    slen: int     # text のバイト長
```

**⚠️ C 版の `loc` / `len`（ソースを指すポインタ）は移せません。**
Mython にポインタはないので、**その部分を文字列としてコピー**して持ちます
（16.5 節）。

---

## 16.4 lexer.my — 移植する

### ✍️ Lexer の状態はクラスにする

C 版：

```c
typedef struct {
    const char *file, *src, *p, *line_start;
    int line;
    int indent_stack[MAX_INDENT_DEPTH];
    int indent_len;
    int paren_depth;
    TokenVec out;
} Lexer;
```

Mython 版：

```python
class Lexer:
    file: str
    src: str
    pos: int          # ★ ポインタ p の代わりに「添字」
    line_start: int   # ★ 同上
    line: int
    prev_line_start: int
    prev_line: int
    indent_stack: list[int]
    paren_depth: int
    out: list[Token]

    def init(self, file: str, src: str) -> None:
        self.file = file
        self.src = src
        self.pos = 0
        self.line_start = 0
        self.line = 1
        self.indent_stack = [0]     # ★ 底は常に 0
        self.out = []
```

**★ `p` を `pos`（添字）に置き換えるのが、この移植で唯一の大きな変換です。**
残りは 1 対 1 で移せます。

### ✍️ 1 文字読むところ

```c
if (*lx->p == '\n') { ... }          // C
```

```python
if self.at() == 10:                  # Mython（10 は '\n'）
```

```python
    # 現在位置のバイト。終端なら 0（C の '\0' と同じ扱い）
    def at(self) -> int:
        if self.pos >= len(self.src):
            return 0
        return strings.byte_at(self.src, self.pos)
```

**⚠️ 文字ではなく「バイト（int）」で比べます。**
第15章で実測したとおり、`s[i]` は 1 文字ごとに `str` を確保するからです
（[ch15](ch15-nullable.md) 15.7 節）。字句解析器は**全バイトを 1 回ずつ**
通るので、ここが効きます。

**★ 「なぜ `byte_at` を作ったのか」の答えが、この 3 行です。**

### 📖 移す順番

C 版の関数を、**依存の少ない順に**移します。

| 順 | C 版 | Mython 版 | 備考 |
|---|---|---|---|
| 1 | `is_digit` / `is_ident_start` … | 同名 | バイト（int）を受け取る形に |
| 2 | `is_keyword` | 同名 | キーワード表は `list[str]` |
| 3 | `tv_push` | `Lexer.push` | `list[Token].append` |
| 4 | `advance_newline` | 同名 | |
| 5 | `read_int` | 同名 | `strtoll` が無いので自前で組む |
| 6 | `read_ident` | 同名 | |
| 7 | `read_string` | 同名 | エスケープ表 |
| 8 | `read_punct` | 同名 | 記号表（**長い順**を保つ） |
| 9 | `scan_indent` / `emit_indent_tokens` | 同名 | この章の山場 |
| 10 | `tokenize` | 同名 | 本体のループ |

**⚠️ 記号表の順序を絶対に変えないこと。**
`"//"` より先に `"/"` を書くと最長一致が壊れます（第4章の注意そのまま）。

### ⚠️ `strtoll` が無い

C 版は基数つきの文字列変換を標準ライブラリに任せていました。
Mython の `int()` は 10 進だけなので、**自分で組みます。**

```python
def digit_value(b: int) -> int:
    if b >= 48 and b <= 57:      # '0'-'9'
        return b - 48
    if b >= 97 and b <= 102:     # 'a'-'f'
        return b - 97 + 10
    if b >= 65 and b <= 70:      # 'A'-'F'
        return b - 65 + 10
    return -1
```

**★ 桁を 1 つずつ足していくだけです。** オーバーフローの検出も、
`v > (LIMIT - d) / base` の形で自分で書きます。
**C 版が `errno` でやっていたことを、明示的に書くことになります。**

---

## 16.5 ポインタが無い言語への移し方

### 📖 3 つの置き換え

| C 版 | Mython 版 | 影響 |
|---|---|---|
| `const char *p`（読み取り位置） | `pos: int`（添字） | 差分 `p - start` は `pos - start` |
| `t->loc` / `t->len`（ソースへの参照） | `raw: str`（部分文字列のコピー） | **メモリを使うが、意味は同じ** |
| `char digits[64]`（スタック上の配列） | `list[int]` か `str` | 上限を気にしなくてよくなる |

### ⚠️ 部分文字列をコピーすることの意味

C 版のトークンは**ソースを指すだけ**で、文字列を複製しません。
Mython 版は `strings.substr()` で**コピー**します。

```
C 版      : トークン 1 個 = 48 バイト（位置と長さだけ）
Mython 版 : トークン 1 個 = 48 バイト + 中身の文字列
```

**★ これは「移植したから遅くなった」ではなく、
「ポインタを持たない言語を選んだ結果」です。**
第20章で速度を測るとき、この差が最初に出てくるはずです。**予想として記録します。**

### 📖 変えてよかったもの・変えてはいけないもの

| C 版 | Mython 版 | 判断 |
|---|---|---|
| `enum TokenKind` | `int` の定数 | ✅ 変えざるをえない（値は揃える） |
| `char *` + 長さ | `str` のコピー | ✅ 同上 |
| 固定長 `indent_stack[64]` | `list[int]`（無制限） | ⚠️ **上限のエラーが消える**。C 版に合わせて 64 で制限する |
| 判定の順序 | そのまま | ❌ 変えない |
| 記号表の並び | そのまま | ❌ 変えない |

**⚠️ 「Mython のほうが便利だから」で挙動を変えないこと。**
`indent_stack` を無制限にすると、C 版がエラーにする入力を
stage1 が受け入れてしまい、**2 つのコンパイラの意味が食い違います。**

---

## 16.6 検証：全ケースで diff

### ✍️ 比較スクリプト

```bash
# tests/selfhost.sh
for f in tests/cases/*.my tests/mods/*/*.my; do
    ./build/mythonc --dump-tokens "$f" > /tmp/c.txt 2>/dev/null || continue
    ./build/stage1-lexer "$f" > /tmp/m.txt || fail
    diff -q /tmp/c.txt /tmp/m.txt || fail
done
```

**★ テストケースをそのまま「字句解析器のテスト」に再利用します。**
300 個以上のファイルが、この章の検証データになります。
**わざわざ新しいテストを書く必要はありません。**

`make test` から呼ぶので、**以降の章で C 版を触ったら、
stage1 との差もすぐ分かります。**

### ⚠️ C 版が失敗するファイルは飛ばす

`tests/cases/err_*.my` は**わざと壊してある**ので、
C 版の `--dump-tokens` も失敗します（字句エラーの場合）。
比較対象は「C 版が成功したもの」だけにします。

---

## 16.7 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ トークン列が完全一致した

```bash
$ make test
全 312 件パス

トークン列一致 338 件 / 字句エラーの位置一致 9 件
```

**338 ファイルで、C 版と stage1 の `--dump-tokens` 出力がバイト単位で一致しました。**
比較対象には `tests/cases/` `tests/mods/` に加えて、
**`lib/*.my` と `selfhost/*.my` 自身**も含まれています。

> **★ 字句解析器が、自分自身のソースを C 版と同じように読めています。**
> セルフホストの最初の 1 歩が通りました。

### ✅ 字句エラーの位置も一致した

トークン列が比べられない 9 件（わざと壊してあるファイル）は、
**エラーの位置**を比べました。

```
C 版:   err_str_bad_escape.my:4:17
stage1: err_str_bad_escape.my:4:17
```

| ファイル | C 版 | stage1 |
|---|---|---|
| `err_bad_char.my` | 3:12 | 3:12 |
| `err_empty_hex.my` | 4:12 | 4:12 |
| `err_indent_misaligned.my` | 6:1 | 6:1 |
| `err_num_suffix.my` | 4:12 | 4:12 |
| `err_overflow.my` | 4:12 | 4:12 |
| `err_str_bad_escape.my` | 4:17 | 4:17 |
| `err_str_unclosed.my` | 4:14 | 4:14 |
| `err_tab.my` | 4:5 | 4:5 |
| `err_tab_indent.my` | 5:5 | 5:5 |

**⚠️ 一致しているのは位置だけです。** メッセージの体裁（`-->` の抜粋や
ヒント）はまだ移植していません（`src/diag.c` は第18章）。

### ★ 第15章の投資が効いた（実測）

166KB（4,500 行相当）の入力を stage1 に食わせます。

| | 実行時間 |
|---|---|
| **今**（`str` に長さを持たせた） | **0.05 秒** |
| 第14章までの表現（長さが O(n)） | **6.55 秒** |

**131 倍**です。しかも O(n²) なので、入力が 10 倍になると差は 100 倍に開きます。

```
1.66MB の入力なら：  今 0.5 秒 ／ 第14章までの表現なら 11 分（推定）
```

**★ 第15章で「疑いを実測してから直す」をやらなかったら、
この章は「なぜか終わらない」で止まっていました。**

### ✅ C 版との速度差

1.66MB（45,311 行）の入力：

```
C 版      real 0.23
stage1    real 0.48
```

**Mython 版は C 版の約 2 倍の時間**で済んでいます。

**★ これは十分に速い**と判断します。第20章のブートストラップでは
コンパイラ自身（数千行）を読むだけなので、体感差はほぼ出ません。

**⚠️ 差の主な原因は 16.5 節で予想したとおり**、トークンごとの
部分文字列コピーだと考えられます（C 版はソースを指すだけ）。
**確かめるのは第20章の後**です。

### ✅ 出力の例

```bash
$ ./build/stage1-lexer tests/cases/int_42.my
  0  KEYWORD   3:1    def
  1  IDENT     3:5    main
  2  PUNCT     3:9    (
  3  PUNCT     3:10   )
  4  PUNCT     3:12   ->
  5  IDENT     3:15   int
  6  PUNCT     3:18   :
  7  NEWLINE   3:19   
  8  INDENT    4:5    
  9  KEYWORD   4:5    return
 10  INT       4:12   42
 11  NEWLINE   4:14   
 12  DEDENT    4:14   
 13  EOF       4:14   
```

**桁揃えまで同じ**です（`strings.lpad` / `strings.rpad` を足しました）。

### ⚠️ 移植して初めて分かったこと

**① `while True:` を「抜けない」と判定してくれない。**

```python
def scan_indent(self) -> int:
    while True:
        ...
        return width
    return -1     # ← ★ ここには来ないが、書かないとコンパイルが通らない
```

`src/sema.c` の `always_returns` に
「`while True:` は `break` が無ければ抜けないが、v1 では判定しない」
とコメントしてあったとおりでした。**自分で書いた制限に、自分で当たりました。**

**⚠️ ここでは stage0 を直しません。** この章の仕事は移植です。
**一度に 1 つ**（16.1 節）。

**② `list` に `pop` が無い。**

インデントのスタックを `list[int]` で持とうとして気づきました。
C 版は「固定長配列 + 長さ」だったので、**同じ形にしました**。

```python
indent_stack: list[int]
indent_len: int          # ★ これより後ろの要素は「使われていない古い値」
```

**結果的に C 版と 1 対 1 のままになりました。**
「Mython のほうが便利だから」と `pop` を求めていたら、
**表現が食い違って移植が難しくなっていました。**

**③ グローバルにリストが書けない。**

言語仕様 6.2 で「グローバル変数の初期化式はコンパイル時定数」と
決めてあるので、`KEYWORDS: list[str] = [...]` は書けません。
`Lexer.init` の中で 1 回だけ組み立てる形にしました。

---

## 16.8 まとめと次章の予告

### できたこと

```
✅ selfhost/token.my — TokenKind（int 定数）と Token クラス
✅ selfhost/lexer.my — src/lexer.c の移植（444 行）
✅ selfhost/dump_tokens.my — C 版と同じ書式で出す入口
✅ lib/strings.my に lpad / rpad（桁揃え）
✅ tests/selfhost.sh — 338 ファイルでトークン列を diff、9 件でエラー位置を比較
✅ make test に組み込んだ（以降、C 版を触れば差がすぐ出る）
✅ C 版の約 2 倍の速度で動く（1.66MB を 0.48 秒）
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `selfhost/token.my` | **新規 80 行** |
| `selfhost/lexer.my` | **新規 464 行**（C 版 `lexer.c` は 647 行） |
| `selfhost/dump_tokens.my` | **新規 45 行** |
| `lib/strings.my` | `lpad` / `rpad` |
| `tests/selfhost.sh` | **新規**。C 版との比較 |
| `Makefile` | `make test` に組み込み、`make selfhost-test` |

**★ コンパイラ本体（`src/`）は 1 行も変えていません。**
この章は「Mython を使う側」に回った最初の章です。

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch4 | `--dump-tokens` を作った | **そのまま「正解」になった**（比較の基準） |
| ch4 | INDENT / DEDENT を仮想トークンにした | 移植しても構造が変わらなかった |
| ch10 | `list[T]` | トークン列・スタック・表のすべてに使えた |
| ch12 | class とメソッド | `Lexer` の状態をそのまま持てた |
| ch13 | `import` | `token` / `lexer` / `dump_tokens` に分けられた |
| ch14 | `io` / `sys` / `strings` | ファイルを読み、引数を取り、部分文字列を切り出せた |
| ch14 | 「Mython で書けるものは Mython で書く」 | `lpad` / `rpad` を**言語に触らず**足せた |
| **ch15** | **`str` に長さを持たせた** | **これが無ければ 131 倍遅かった（実測）** |
| ch15 | `strings.byte_at` | 全バイトを 1 回ずつ通る処理で効いた |
| ch15 | `T \| None` | まだ出番が少ない。第17章の AST で本番になる |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| **機械的に移す。改善しない** | 出力が違ったとき、原因を「移植ミス」1 種類に絞れる |
| **`enum` → `int` 定数**（値も順序も揃える） | Mython に enum が無い。表現は変えるが**意味は変えない** |
| **ポインタ → 添字（`pos`）** | 唯一の大きな変換。これさえ決めれば残りは 1 対 1 |
| **トークンは部分文字列をコピーして持つ** | ポインタが無いので必然。**遅くなる可能性を記録して先へ進む** |
| **インデントのスタックは「配列 + 長さ」** | `pop` が無いから……ではなく、**C 版と同じ形を保つため** |
| **テストケースをそのまま検証データにする** | 300 個以上のファイルが手元にある。**新しいテストを書かない** |
| **エラーの「位置」だけ比べる** | 診断の描画は第18章。**比べられるものだけ比べる** |
| **`make test` に組み込む** | C 版を直したら stage1 との差がその場で分かる |

### ⚠️ 予想が外れたこと

**一発で一致した。**

正直なところ、最初の diff は数十行出ると思っていました。実際には
`tests/cases/int_42.my` が**いきなり一致**し、338 ファイルでも
差はゼロでした。

理由は 3 つあると考えています。

1. **C 版のコードが「移しやすい形」だった**（第4章で関数を細かく分けた）
2. **`--dump-tokens` が最初からあった**（第4章）ので、基準が明確だった
3. **改善しなかった**（16.1 節）

**★ 「移植は難しい」のではなく、「移植しにくい書き方」があるだけです。**

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 診断の描画（`-->` の抜粋・ヒント・note） | 第18章（`diag.c` の移植） |
| `while True:` を「抜けない」と判定しない | 未定（stage0 の `always_returns`。第20章の後に検討） |
| トークンごとの部分文字列コピー | 第20章の後（速度を測ってから） |
| C 版の 2 倍の実行時間 | 同上 |

### ✍️ commit する

```bash
git add -A
git commit -m "第16章: Mython で字句解析器を書く"
```

---

## 次章：第17章 Mython で構文解析器を書く

**達成目標**

```bash
$ ./build/stage1-parser tests/cases/int_42.my > mine.txt
$ ./build/mythonc --dump-ast tests/cases/int_42.my > theirs.txt
$ diff mine.txt theirs.txt        # ★ S 式が完全一致すること
```

**やること**

| ファイル | 作業 |
|---|---|
| `selfhost/ast.my` | `Node` クラス（**全部入り構造体**方式。architecture.md 3.2） |
| `selfhost/parser.my` | 再帰下降構文解析器（`src/parser.c` の移植） |
| `tests/selfhost.sh` | AST の比較を足す |

**★ ここで `T | None` が本番になります。**
`Node.lhs` / `rhs` / `els` は「無いことがある」子ノードです。
第15章で入れた理由が、この章で回収されます。

**⚠️ 予想される落とし穴**

- `Node` のフィールドが多い（20 個以上）。**全部 `T | None` になる**
- 再帰下降なので**関数の相互再帰**が深い（`expr → … → primary → expr`）。
  Mython は前方参照できる（第8章）ので書けるはず
- 演算子の優先順位の階層を、C 版と**同じ関数名・同じ順序**で移す
- 脱糖（`elif` / 複合代入 / `for`）も構文解析器の中にある。**そこも移す**

### 🤔 第17章に入る前の練習問題

1. `selfhost/lexer.my` の `puncts` の並びを 1 か所入れ替えて（`"/"` を `"//"` より前に）、
   `make selfhost-test` が何と言うか見る（**必ず元に戻す**）
2. `scan_indent` の「空行を読み飛ばす」処理を消して、どのテストが落ちるか確かめる
3. `Lexer.at()` を `strings.byte_at` から `ord(self.src[self.pos])` に変えて、
   16.7 節のベンチマーク（166KB）がどう変わるか測る
4. C 版の `lexer.c` と `lexer.my` を並べて読み、**関数の順序が同じか**確かめる
5. **自分でトークンを 1 種類足してみる**（例：`@`）。
   C 版と Mython 版の両方を直さないと `make test` が通らないことを体験する
