# 第20章 ブートストラップと不動点検証

> **この章のゴール**
> **Mython コンパイラが、自分自身をコンパイルできる。**
>
> ```bash
> $ make bootstrap
> ── stage1 = C 版がビルド
> ── stage2 = stage1 がビルド（Mython 製コンパイラが自分自身を）
> ── stage3 = stage2 がビルド
>
> ★ stage2 と stage3 が出す IR が完全一致（13 本）
> ★ 実行ファイルもバイト単位で一致（UUID を除く）
>
> ★ 不動点に到達しました（stage2 == stage3）
>    Mython コンパイラは、自分自身をコンパイルできます。
> ```

**本書の終着点です。**

第1章で「整数を返すだけ」だったコンパイラが、
**自分自身を含む 6,500 行の Mython プログラムをコンパイルできる**ところまで来ました。

---

## 目次

- [20.1 ドライバを移す](#201-ドライバを移す)
- [20.2 ビルド時の値をどう渡すか](#202-ビルド時の値をどう渡すか)
- [20.3 不動点とは何か](#203-不動点とは何か)
- [20.4 「一致しない」を調べる](#204-一致しないを調べる)
- [20.5 動作確認](#205-動作確認)
- [20.6 全体の振り返り](#206-全体の振り返り)
- [20.7 セルフホストが見つけた課題を直す](#207-セルフホストが見つけた課題を直す)
- [20.8 ここから先へ](#208-ここから先へ)

---

## 20.1 ドライバを移す

残っていたのは `src/main.c` だけです。

```python
# selfhost/main.my
def main() -> int:
    opt: Options = parse_args(sys.argv())

    mods: module.Module | None = module.load_modules(opt.input_)
    entry: module.Module | None = module.entry_of(mods, opt.input_)
    sema.sema_program(mods, entry)
    ...
    rc: int = sys.run(strings.join(parts, ""))   # ★ clang を呼ぶ
```

**★ 第14章で `sys.argv` と `sys.run` を入れた理由が、ここで回収されます。**

> [self-hosting.md](../design/self-hosting.md) 3.4 節
> **⚠️ 見落としやすい項目**：`argv` の取得と `system()` 呼び出しです。
> これがないと、Mython 製コンパイラは「コマンドラインツール」になれません。

6 章前の予告どおり、**この 2 つが無ければドライバは書けませんでした。**

### ✍️ 足したのは `io.remove` だけ

中間ファイル（`.ll`）を片付けるために `remove()` が要りました。

```python
# lib/io.my
extern def my_remove(path: str) -> None

def remove(path: str) -> None:
    my_remove(path)
```

**★ 第14章から数えて 5 つ目の「ライブラリに足す」です。**
（`byte_at` / `join` / `eprint` / `print_raw` / `remove`）
**言語には最後まで 1 つも足しませんでした。**

---

## 20.2 ビルド時の値をどう渡すか

C 版は 3 つの値をビルド時に埋め込んでいます。

```makefile
CFLAGS += -DMYTHON_TARGET_TRIPLE='"$(HOST_TRIPLE)"'
CFLAGS += -DMYTHON_RUNTIME_O='"$(abspath $(RUNTIME_OBJ))"'
CFLAGS += -DMYTHON_LIB_DIR='"$(abspath lib)"'
```

**Mython にプリプロセッサはありません。** stage1 は実行時に知るしかない。

### ✍️ 判断：両方が環境変数を先に見る

```c
// src/module.c / src/main.c
static const char *lib_dir(void) {
    const char *env = getenv("MYTHON_LIB_DIR");
    if (env && env[0]) return env;
    return MYTHON_LIB_DIR;      // 埋め込んだ既定値
}
```

```python
# selfhost/module.my
def lib_dir() -> str:
    env: str = sys.getenv("MYTHON_LIB_DIR")
    if len(env) > 0:
        return env
    return DEFAULT_LIB_DIR
```

**★ 「C 版だけ直す」でも「stage1 だけ直す」でもなく、
両方が同じ規則で探すようにします。**
片方だけ直すと、**同じ入力に対する挙動が食い違います**——
それは「同じコンパイラ」ではありません。

**⚠️ v1 の割り切りです。** 本来はインストール先を決めて埋め込むべきで、
その話は「配布」の領域に入ります（20.8 節）。

---

## 20.3 不動点とは何か

### 📖 3 段ビルド

```
stage1 = C 版がビルドした Mython 製コンパイラ
stage2 = stage1 が自分自身のソースをビルドしたもの
stage3 = stage2 が自分自身のソースをビルドしたもの
```

**★ 比べるのは stage2 と stage3 です。stage1 ではありません。**

| ペア | 一致すべきか | 理由 |
|---|---|---|
| stage1 と stage2 | ❌ しなくてよい | 作った人が違う（C 版 vs Mython 版）。**中身の最適化や並びが違ってよい** |
| **stage2 と stage3** | ✅ **しなければならない** | どちらも「Mython 製コンパイラが作った Mython 製コンパイラ」。**変わる理由がない** |

**🤔 なぜ stage2 と stage3 なのか**

stage2 は「C 版がビルドしたコンパイラ」が作ったもの、
stage3 は「stage2 自身」が作ったものです。
**同じソースを同じ規則でコンパイルしているのに結果が変わるなら、
そこには「誰がコンパイルしたかによって変わる何か」が残っています。**

一致すれば、**コンパイラの出力が入力の不動点に達した**ことになります。

```
f(source) = binary
f(f(source)) = f(source)   ← ★ これが不動点
```

### ✍️ make bootstrap

```bash
$ make bootstrap
```

`tests/bootstrap.sh` が 3 段ビルドし、次の 2 つを比べます。

1. **stage2 と stage3 が出す `.ll`（13 本）** ← 本体
2. **実行ファイル**（UUID を除く。次節）

---

## 20.4 「一致しない」を調べる

**最初の比較は失敗しました。**

```
✗ stage2 != stage3
build/stage2 build/stage3 differ: char 1369, line 1
```

**⚠️ ここで「不動点に達していない」と結論してはいけません。**
**どこがどう違うのか**を見ます。

```bash
$ xxd -s 1300 -l 160 build/stage2 | diff - <(xxd -s 1300 -l 160 build/stage3)
< 00000554: 1800 0000 ca6c 9575 6159 4770 ad08 5581
> 00000554: 1800 0000 9e51 538e bb71 42f9 af00 7829
```

**16 バイトだけが違います。** サイズは同じ。位置は 0x554。

これは Mach-O の **`LC_UUID`**——**リンカが毎回埋める識別子**です。
コンパイラの出力とは関係ありません。

### ✅ 確かめ方：IR を比べる

```bash
$ diff <(./build/stage2 -S selfhost/main.my) <(./build/stage3 -S selfhost/main.my)
（差なし）
```

**コンパイラが「作った」ものは完全に一致していました。**
違ったのは、**リンカが後から足したもの**だけです。

```bash
$ clang -O0 stage2.*.ll runtime.o -Wl,-no_uuid -o cmp2
$ clang -O0 stage3.*.ll runtime.o -Wl,-no_uuid -o cmp3
$ cmp cmp2 cmp3 && echo 一致
一致
```

> **★ 「違いが出た」で止まらず、「どこが違うのか」を見る。**
> 第11章（continue の飛び先）、第15章（strlen が真犯人）、
> 第17章（`t0` の衝突）——**この本で 4 回目の同じ教訓です。**

---

## 20.5 動作確認

**以下はすべて実際に実行した結果です。**

### ★ 不動点に到達した

```
$ make bootstrap
── stage1 = C 版がビルド
── stage2 = stage1 がビルド（Mython 製コンパイラが自分自身を）
── stage3 = stage2 がビルド

★ stage2 と stage3 が出す IR が完全一致（13 本）
★ 実行ファイルもバイト単位で一致（UUID を除く）

★ 不動点に到達しました（stage2 == stage3）
   Mython コンパイラは、自分自身をコンパイルできます。
```

### ★ Mython 製コンパイラでテストが全部通る

```bash
$ make bootstrap-test
全 323 件パス
```

**323 件のテストを、C 版ではなく stage2（Mython 製コンパイラ）でコンパイルして通しました。**

**★ これは「C 版と同じ出力を出す」より強い確認です。**
出力が同じかどうかではなく、**プログラムとして正しく動くか**を見ています。

### ✅ 5 本の検証はすべて維持されている

```bash
$ make test
全 323 件パス

トークン列一致 360 件 / 字句エラーの内容一致 9 件
AST 一致 323 件 / 構文エラーの内容一致 37 件
型検査 一致 183 件 / 型エラーの内容一致 140 件
IR 一致 183 件 / stage1 の IR で実行して一致 164 件
```

### ✅ 速度：C 版とほぼ同じ

```
C 版が stage1 を作る    real 0.48
stage1 が自分を作る     real 0.50
```

**自分自身（6,500 行・13 モジュール）をコンパイルする時間は、C 版と 4% 差です。**

**🤔 なぜ第16章では 2 倍だったのに？**
コンパイル全体では **`clang` の呼び出しが時間の大半**を占めるからです。
コンパイラ本体の差は、実行時間全体の中では小さくなります。

**★ 「どこが遅いか」は、測る範囲で変わります。**

### 📖 最終的な規模

| | 行数 |
|---|---|
| C 版コンパイラ（`src/`） | 7,685 |
| **Mython 版コンパイラ（`selfhost/`）** | **6,593** |
| 標準ライブラリ（`lib/`。Mython） | 267 |
| ランタイム（`runtime/`。C） | 426 |
| テストケース | 347 ファイル |
| 生成される IR（自分自身） | 28,333 行 / 13 モジュール |

---

## 20.6 全体の振り返り

### 📖 20 章で作ったもの

```
第I部   ch1-4    パイプラインの骨格（字句 → 構文 → 生成）とインデント構文
第II部  ch5-11   型検査・制御構文・関数・文字列・list・for
第III部 ch12     class
第IV部  ch13-15  モジュール・標準ライブラリ・T | None
第V部   ch16-20  セルフホスト
```

### ★ 繰り返し出てきた 5 つの考え方

**① 脱糖（desugaring）— 新しい概念を増やさずに機能を足す**

| 章 | 何を | 何に |
|---|---|---|
| ch5 | `x += 1` | `x = x + 1` |
| ch7 | `elif` | `else` の中の `if` |
| ch11 | `for` | `while` + 隠し変数 |

**新しいノード種別を作らずに済んだ**ので、意味解析もコード生成も無変更でした。

**② 名前修飾（mangling）— `.` を含めれば衝突しない**

| 章 | 名前 |
|---|---|
| ch7 | `%x.1`（IR 名の一意化） |
| ch11 | `for.ix.0`（隠し変数） |
| ch12 | `@Token.show`（メソッド） |
| ch13 | `@lexer.Token.show`（モジュール） |
| **ch17** | **`%t.0`（一時値）— 移植で衝突を踏んで直した** |

**利用者が書ける識別子に `.` は入らない**——この 1 点だけで全部解決しています。

**③ 「先に全部登録してから、本体を見る」**

| 章 | 単位 |
|---|---|
| ch8 | 関数（前方参照） |
| ch12 | クラス（相互参照。3 パスに分割） |
| ch13 | モジュール（ファイル単位） |

**同じ問題には同じ手が効きます。**

**④ 静的検査と動的検査の二段構え**

| 何を | 静的（コンパイル時） | 動的（実行時） |
|---|---|---|
| 0 除算 | リテラル 0 は弾く | `my_floordiv` が panic |
| 添字 | 型は検査する | `my_list_get` が範囲検査 |
| 未初期化フィールド | init での代入を要求（ch15） | `my_check_not_none`（ch12） |

**⑤ 疑いは実測してから直す**

| 章 | 疑い | 実測の結果 |
|---|---|---|
| ch11 | continue が増分を飛ばす？ | ✅ 無限ループ（終了コード 124） |
| ch12 | 自前のレイアウト計算は合っている？ | ✅ LLVM の計算と一致 |
| **ch15** | **`s[i]` の確保が遅い？** | **❌ 真犯人は `strlen`（131 倍の差）** |
| ch20 | 不動点に達していない？ | ❌ 違いは UUID 16 バイトだけ |

### ⚠️ セルフホストが見つけたバグ（4 件）

| 章 | バグ | いつから | なぜ見つからなかったか |
|---|---|---|---|
| ch17 | `--dump-ast` が `import` で落ちる | ch13 | `--dump-ast` にテストが無かった |
| ch17 | `ND_NONE` が改行を出さない | ch15 | 同上 |
| ch17 | **`t0` という変数が一時値と衝突** | **ch1** | 誰も `t0` という名前を使わなかった |
| ch18 | **`entry` という変数がラベルと衝突** | **ch1** | 同上 |

**★ 「テストを 300 個書く」ことと「実プログラムを 1 つ書く」ことは、
別の種類の検査です。**

### ⚠️ 言語の制限が設計を決めた

| 制限（決めた章） | 代償（出た章） |
|---|---|
| 循環 import の禁止（ch13） | Type / Class を 1 つのモジュールに集めた（ch18） |
| 利用者定義のジェネリクスなし（ch14） | `dict` は `str → int` 限定 |
| 絞り込みは `and` とガード節だけ（ch15） | 移植中に 3 回書き直した（ch18） |
| enum が無い | `int` の定数で代用（ch16） |
| プリプロセッサが無い | ビルド時の値を環境変数で渡す（ch18 / ch20） |

**★ 「作らない」と決めた機能は、必ずどこかで代償を払います。**
大事なのは**代償を払う場所を自分で選ぶこと**です。

### 📖 v1 で意図的に作らなかったもの

継承 / 例外 / クロージャ / ジェネリクス / 演算子オーバーロード /
タプル / イテレータプロトコル / GC / f-string / `from X import Y`

**すべて「無くてもコンパイラが書けた」ことが、この本で証明されました。**

---

## 20.7 セルフホストが見つけた課題を直す

20.6 節の表に並べた「セルフホストが見つけたバグ」は、**踏んだその場で直したもの**でした。
一方、**踏んだけれど直さずに記録した**ものが 3 つ残っています。

| どこで踏んだか | 症状 | 記録した場所 |
|---|---|---|
| ch16（字句解析器の移植） | `while True:` を「抜けない」と判定しない | ch16 の既知の課題 |
| ch17（構文解析器の移植） | `panic` が「戻らない」と扱われない | ch17 の既知の課題 |
| ch18（型検査器の移植） | 絞り込みが `or` / 条件式の途中 / `panic` の後で効かない | ch18 18.5 節 |

**★ 3 つとも「移植したから見つかった」ものです。**
テストケースは 1 行 2 行の断片なので、`while True:` も `or` の絞り込みも出てきません。
**6,500 行の実プログラムを書いて初めて、不便さとして表に出ました。**

不動点に達した今なら、**直した結果を不動点で検証できます。** ここで回収します。

### 📖 まず「いくつの問題なのか」を数え直す

症状は 3 つですが、**原因は 2 つ**です。

```
① always_returns が「戻らない構文」を知らない
      ├── while True: が抜けないと分からない        ← ch16 の症状
      ├── panic() が戻らないと分からない            ← ch17 の症状
      └── panic の後で絞り込みが効かない            ← ch18 の症状の一部
② 絞り込みが短絡評価の「途中」を見ていない
      ├── and の rhs で絞り込めない                 ← ch18 の症状の一部
      └── or  の rhs で絞り込めない                 ← 同上
```

**🤔 なぜ ① が絞り込みにも効くのか**

第15章でガード節の絞り込みを入れたとき、**判定を `always_returns` の再利用で作った**からです。

```c
// src/sema.c（第15章のまま）
if (st->kind == ND_IF && !st->els && always_returns(st->body))
    narrow_apply(s, st->lhs, false, &guard);
```

「その `if` の中で必ず抜けるなら、その後ろでは条件の反対側が成り立つ」——
**`always_returns` が賢くなると、絞り込みも同じだけ賢くなります。**

**★ 再利用しておくと、直す場所も 1 か所で済みます。**
第15章で「10 行で入った」のと同じ構造が、ここで 2 度目の配当を出しました。

### ✍️ ① `always_returns` に「戻らない」を 2 つ教える

```c
// src/sema.c
case ND_WHILE:
    // ★ while True: は break が無ければ抜けない。
    //   条件が「True というリテラルそのもの」のときだけ見ます。
    //   変数や式は追いません（保守的でよい）。
    return n->lhs && n->lhs->kind == ND_BOOL && n->lhs->ival != 0 &&
           !has_break(n->body);

case ND_CALL:
    // ★ panic() / exit() を呼んだら、その先へは進まない。
    return never_returns_call(n);
```

**⚠️ `has_break` は入れ子のループに降りません。**

```c
static bool has_break(Node *n) {
    switch (n->kind) {
        case ND_BREAK: return true;
        case ND_WHILE: return false;  // ★ 内側のループの break は、こちらには効かない
        case ND_IF:    return has_break(n->body) || has_break(n->els);
        ...
```

```python
while True:
    while i < 10:
        break          # ← これは内側のループのもの。外側は抜けない
    ...
```

**降りてしまうと「break がある」と誤判定し、正しいプログラムを弾きます。**
`if` の中には降ります（`break` は条件付きで書くのが普通だからです）。

**⚠️ `panic` / `exit` の判定は、ランタイムと対になっています。**

```c
// runtime/runtime.c
_Noreturn void my_panic(const char *msg) { ... }
_Noreturn void my_exit(long long code) { exit((int)code); }
```

```c
// src/sema.c
static bool never_returns_call(Node *n) {
    if (n->kind != ND_CALL || !n->builtin) return false;
    return strcmp(n->builtin->impl, "my_panic") == 0 ||
           strcmp(n->builtin->impl, "my_exit") == 0;
}
```

**片方だけ変えると「sema は通すのに、実行時には戻ってくる」ことになります。**

**🤔 codegen は直さなくてよいのか**

要りませんでした。`always_returns` が緩くなると
「`return` で終わらない関数本体」が codegen に届きますが、そこはもう塞がっています。

```c
// src/codegen.c（第8章のまま）
if (!e->terminated) {
    if (n->type->kind == TY_NONE) {
        sb_printf(&e->fn, "  ret void\n");
    } else {
        sb_printf(&e->fn, "  unreachable\n");   // ★ ここが受け止める
    }
}
```

**★ 「到達不能なら `unreachable` を置く」を先に書いてあったので、
sema を緩めても IR は壊れませんでした。**

### ✍️ ② 短絡評価の「途中」で絞り込む

`and` と `or` は短絡評価します。**rhs は「lhs がある側に転んだとき」しか評価されません。**

```python
a is not None and a.v == 0    # and の rhs は lhs が真のときだけ見る
a is None     or  a.v == 0    # or  の rhs は lhs が偽のときだけ見る
```

**その側の絞り込みを、rhs を検査する間だけ効かせます。**

```c
// src/sema.c
static Type *check_logical(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);

    NarrowSet sc = {0};
    narrow_apply(s, n->lhs, n->op == OP_AND, &sc);   // ★ and なら真の側、or なら偽の側
    Type *r = check_expr(s, n->rhs);
    narrow_restore(&sc);                             // ⚠️ 抜けたら必ず戻す
    ...
```

**★ 「入る前に変えて、抜けたら戻す」——第7章のスコープ、第15章の絞り込みと同じ形です。**
`positive` に `n->op == OP_AND` を渡すだけで、`and` と `or` の両方が入りました。

### ✍️ ③ `or` のガード節（ド・モルガン）

第15章で「`or` は絞れない」と書きました。**半分だけ間違いでした。**

```
not(a or b) = (not a) and (not b)
```

**「成り立たない側」なら、`or` でも両方絞れます。**

```python
def add(b: Box | None, c: Box | None) -> int:
    if b is None or c is None:
        return 0
    return b.v + c.v        # ★ ここでは b も c も None ではない
```

```c
// src/sema.c
else if (cond->kind == ND_LOGICAL && cond->op == OP_OR && !positive) {
    narrow_apply(s, cond->lhs, false, ns);
    narrow_apply(s, cond->rhs, false, ns);
}
```

**⚠️ 「成り立つ側」の `or` は相変わらず絞れません。**

```python
if b is None or c is None:
    print(b.v)          # ✗ b が None かもしれない（c が None なだけかも）
```

**どちらか一方しか保証されない**——第15章の理由はそのまま生きています。
`err_narrow_or_positive.my` として**テストに残しました。**

### ⚠️ 両方を、同じ規則で直す

**ここが第20章でいちばん大事な作業です。**

20.2 節で「C 版だけ直す」でも「stage1 だけ直す」でもない、と書きました。
**同じことが、バグ修正にもそのまま当てはまります。**

| 直す場所 | ファイル |
|---|---|
| C 版 | `src/sema.c` |
| **Mython 版** | **`selfhost/sema.my`** |

片方だけ直すと、`make selfhost-test` の**型検査の比較で落ちます**——
「同じ入力に対して、C 版は通すのに Mython 版は弾く」からです。
そして仮に比較を通っても、**不動点が壊れます。**

**★ 手順は「C 版 → テスト → Mython 版 → 比較 → 不動点」の順に固定します。**

```bash
$ make test            # ① C 版で 323 件 + 5 本の比較
$ make bootstrap       # ② stage2 == stage3 か
$ make bootstrap-test  # ③ Mython 製コンパイラで 323 件
```

**③ まで通って初めて「直った」と言えます。**
①だけでは「C 版が直った」だけ、②だけでは「両方が同じように壊れている」可能性が残ります。

### 📖 テストを 11 件足す

20.6 節の表で、4 件のバグのうち **2 件の原因が「テストが無かった」** でした。
**同じことを繰り返さないよう、直した内容をそのままテストにします。**

| テスト | 何を守るか |
|---|---|
| `while_true_no_return.my` | `while True:` の後ろに `return` が要らない |
| `while_true_nested_break.my` | 内側のループの `break` は外側に効かない |
| `func_panic_no_return.my` | `panic()` の後ろに `return` が要らない |
| `func_exit_no_return.my` | `exit()` も同じ |
| `nullable_narrow_and_rhs.my` | `and` の rhs で絞り込める |
| `nullable_narrow_or_rhs.my` | `or` の rhs で絞り込める |
| `nullable_narrow_or_guard.my` | `or` のガード節（ド・モルガン） |
| `nullable_narrow_after_panic.my` | `panic` の後で絞り込みが効く |
| **`err_while_true_break_no_return.my`** | **`break` があれば、やはり `return` が要る** |
| **`err_while_cond_not_literal_true.my`** | **`True` を変数に入れたら追わない（保守的）** |
| **`err_narrow_or_positive.my`** | **`or` の「成り立つ側」は絞れないまま** |

**★ 後ろの 3 件は「直さなかったこと」を守るテストです。**
緩めた検査は、**どこまで緩めたかを書いておかないと、次に誰かがさらに緩めます。**

### ✅ 動作確認

**以下はすべて実際に実行した結果です。**

```bash
$ make test
全 323 件パス

トークン列一致 360 件 / 字句エラーの内容一致 9 件
AST 一致 323 件 / 構文エラーの内容一致 37 件
型検査 一致 183 件 / 型エラーの内容一致 140 件
IR 一致 183 件 / stage1 の IR で実行して一致 164 件
```

```bash
$ make bootstrap
★ stage2 と stage3 が出す IR が完全一致（13 本）
★ 実行ファイルもバイト単位で一致（UUID を除く）

★ 不動点に到達しました（stage2 == stage3）
```

```bash
$ make bootstrap-test
全 323 件パス
```

**★ 型検査を 2 か所（C 版と Mython 版）で緩めたのに、不動点は動きませんでした。**
これが「両方を同じ規則で直した」ことの証明です。

### 🤔 直したのに、ソースは直さないのか

`selfhost/lexer.my` には、ch16 で書いた**回避のための行**がまだ残っています。

```python
def scan_indent(self) -> int:
    while True:
        ...
        return width
    return -1     # ← ★ もう要らない
```

**消せます。** ただし**消しませんでした。**

**★ 「コンパイラを直すこと」と「コンパイラのソースを書き直すこと」は別の作業です。**
同じコミットで混ぜると、不動点が壊れたときに
**「検査を緩めたせい」なのか「ソースを書き換えたせい」なのか分からなくなります。**

第16章の「一度に 1 つ」（16.1 節）が、最後まで同じ形で効いています。

---

## 20.8 ここから先へ

### ✅ 20.7 節で片づいた課題

| 課題 | どうしたか |
|---|---|
| `while True:` と `panic` が「戻らない」と扱われない | `always_returns` に 2 つ教えた |
| 絞り込みが `or` / 条件式の途中 / `panic` の後で効かない | 短絡評価の途中とド・モルガンを入れた |

### 📖 いま残っている課題

| 課題 | どこに書いてあるか | なぜ残すか |
|---|---|---|
| `strings.substr` が O(n²)、`dict` が線形探索 | ch14 / ch15 の既知の課題 | **速度の問題で、誤動作ではない。**自分自身のコンパイルは 0.5 秒で終わっている（20.5 節）。**測って困ってから直す** |
| 絞り込みが `or` の「成り立つ側」で効かない | 本章 20.7 節 | **一方しか保証されないので、原理的に絞れない。**`err_narrow_or_positive.my` で固定した |
| 絞り込みがフィールドで効かない | ch15 15.5 節 | `node.next` を絞ると「その間 `node.next` が変わらないこと」を保証しなければならない。**メソッド呼び出し 1 つで壊れる** |
| 標準ライブラリの場所が環境変数依存 | ch18 18.6 / 本章 20.2 | **「配布」の設計が要る。**インストール先を決める話であって、コンパイラの話ではない |
| メモリを一切解放しない | memory-model.md 8 節 | **GC を作る話。**本書の範囲を超える |

**★ 残した理由が「まだ考えていない」ではなく、
「こういう理由で、今は要らない」になっているかを確かめてください。**

知らずに残っている問題と、決めて残した課題は別物です。
**20.7 節で直した 3 つは、後者から「直せると分かった」ものが出てきた例でした。**

### 🤔 次に作るなら

| やりたいこと | 最初に読む章 |
|---|---|
| 最適化を入れる | ch19（IR の形）／`-O2` を試して IR を見る |
| GC を入れる | memory-model.md（今は解放しない前提） |
| エラーを 1 つ目で止めない（複数報告） | ch3（診断は `diag_fail` で即終了している） |
| 別のバックエンド（WASM など） | ch19（`codegen.c` を差し替える） |
| 型推論を強くする | ch5（型注釈必須の判断）／ch15（絞り込み） |

### ✍️ commit する

```bash
git add -A
git commit -m "第20章: ブートストラップと不動点検証（セルフホストが見つけた課題の修正まで）"
```

---

## ★ おわりに

第1章で、`42` を返すだけのプログラムから始めました。

```bash
$ ./build/mythonc t.my -o t && ./t; echo $?
42
```

第20章の今、同じコンパイラが**自分自身を作れます。**

```bash
$ make bootstrap
★ 不動点に到達しました（stage2 == stage3）
```

**この間に足した「言語機能」は 20 個ほどです。**
そのたびに、**作らない判断**も同じ数だけしてきました。
[dev-log.md](../dev-log.md) には**201 個の判断**が、理由つきで残っています。

**★ コンパイラを作る技術とは、「何を作らないか」を決める技術でもあります。**

お疲れさまでした。
