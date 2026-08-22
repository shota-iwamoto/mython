# 第18章 Mython で型検査器を書く

> **この章のゴール**
> `selfhost/sema.my` が、C 版と**まったく同じ診断**を出す。
>
> ```bash
> $ ./build/stage1-check tests/cases/err_arg_type.my
> error: 関数 'twice' の第 1 引数: 型 'bool' を 'int' に渡せません
>   --> tests/cases/err_arg_type.my:7:18
>    |
>  7 |     return twice(True)
>    |                  ^^^^ これは 'bool' 型です
>    |
> note: 引数 'x' は 'int' 型です
>   --> tests/cases/err_arg_type.my:3:1
>    |
>  3 | def twice(x: int) -> int:
>    | ^^^
>    |
>    = ヒント: Mython には暗黙の型変換がありません（言語仕様 3.5）
> $ diff <(./build/mythonc --check f.my 2>&1) <(./build/stage1-check f.my 2>&1)
> （差なし）
> ```

**この本でいちばん大きい移植です。**

| | C 版 | Mython 版 |
|---|---|---|
| 診断の描画 | `diag.c` 275 行 | `diag.my` 295 行 |
| 型 | `types.c` 261 行 | `ast.my` に同居 |
| モジュール読み込み | `module.c` 264 行 | `module.my` 264 行 |
| **意味解析** | **`sema.c` 2148 行** | **`sema.my` 1949 行** |

**★ そして、ここで「エラーメッセージまで一致」が完成します。**
第16章はトークン列、第17章は AST、この章は**診断そのもの**です。

---

## 目次

- [18.1 何を「同じ」にするのか](#181-何を同じにするのか)
- [18.2 diag.my — 罫線とキャレットを 1 バイトも違えず](#182-diagmy--罫線とキャレットを-1-バイトも違えず)
- [18.3 循環 import が設計を決めた](#183-循環-import-が設計を決めた)
- [18.4 sema.my — 2148 行を移す](#184-semamy--2148-行を移す)
- [18.5 移植で見つかった 3 つ目のバグ](#185-移植で見つかった-3-つ目のバグ)
- [18.6 標準ライブラリの場所を揃える](#186-標準ライブラリの場所を揃える)
- [18.7 動作確認](#187-動作確認)
- [18.8 まとめと次章の予告](#188-まとめと次章の予告)

---

## 18.1 何を「同じ」にするのか

### ✍️ C 版に `--check` を足す

比較の基準を作ります。**型検査までで止まり、問題が無ければ何も出さない**モードです。

```c
if (strcmp(a, "--check") == 0) { o.stage = STAGE_CHECK; continue; }
...
sema_program(mods, entry);
if (opt.stage == STAGE_CHECK) return 0;
```

```bash
$ ./build/mythonc --check tests/cases/int_42.my   # 何も出ない、終了コード 0
$ ./build/mythonc --check tests/cases/err_arg_type.my
error: ...                                        # 終了コード 1
```

**★ 比較のための機能を、比較される側にも作ります。**
第16章の `--dump-tokens`、第17章の `--dump-ast` と同じ考えです。

### 📖 比べる 3 つのこと

| | 何を比べるか |
|---|---|
| ① 成否 | エラーが出るか / 出ないか（終了コード） |
| ② メッセージ | stderr の**全文をバイト単位で** |
| ③ 正常系 | 300 個以上のファイルが**素通り**するか |

**⚠️ ③ が抜けやすい。**「エラーを出せる」だけでは不十分で、
**出してはいけないところで出さない**ことも同じくらい重要です。
正しいプログラムを 1 つでも誤って弾いたら、その時点で別の言語になります。

---

## 18.2 diag.my — 罫線とキャレットを 1 バイトも違えず

### ⚠️ ここが「一致」のいちばん難しいところ

```
error: 閉じ括弧 ')' がありません
  --> t.my:4:31
   |
 4 |     return (1 + 2   # 日本語のコメント
   |                               ^ ここに ')' が必要です
```

キャレットの位置を合わせるには、**3 種類の数え方**を使い分けます。

| 数え方 | 何に使うか |
|---|---|
| バイト数 | `Token.col`（ソースを切り出す） |
| **文字数** | `--> file:line:col` の桁番号 |
| **表示幅** | キャレット手前の空白（全角は 2） |

C 版（`diag.c`）と同じ判定表を移します。

```python
def utf8_width(s: str, i: int) -> int:
    c: int = strings.byte_at(s, i)
    if c < 128:
        return 1
    ...
    if c >= 224 and c <= 239 and cont_at(s, i + 1) and cont_at(s, i + 2):
        cp: int = (c - 224) * 4096 + ... 
        if is_wide_cp(cp):
            return 2
        return 1
```

**★ 「日本語のコメントが入った行でキャレットがずれない」——
この 1 点のために、第3章から表を持ち歩いてきました。**

### ✍️ Token は「ソースへの参照」を持ち直す

第17章では、表示用の桁を字句解析器が計算して持っていました（`dcol`）。
**この章で C 版と同じ形に戻します。**

```python
class Token:
    col: int          # 行頭からのバイト数（C 版と同じ）
    src: str          # ★ このトークンが属するソース全体
    line_start: int   # 行頭のバイト位置
    len: int          # トークンのバイト長
```

C 版の `const char *line_start`（ソースを指すポインタ）を、
**「ソースそのもの + 添字」**で表しています。
`str` は参照なので、**コピーは起きません**。

### ⚠️ 診断は stdout ではなく stderr

`print` は stdout です。診断は stderr に出さなければ、
`--dump-ast` の出力と混ざってしまいます。

```python
# lib/io.my
extern def my_eprint(s: str) -> None

def eprint(s: str) -> None:
    my_eprint(s)
```

**★ ここでも「言語には足さず、ライブラリに足す」で済みました**（第14章の境界線）。

---

## 18.3 循環 import が設計を決めた

### ⚠️ C 版の 3 ファイルが、Mython では 2 つに割れる

C 版の構造：

```
types.h  … Type
ast.h    … Node / Class / Field
sema.c   … FuncSig / VarEntry / Scope / ModuleSyms
```

これらは**互いを指し合います**。

```
Type → Class → ModuleSyms → Scope → VarEntry → Type
  ↑                                              |
  └──────────────────────────────────────────────┘
```

C では**前方宣言**（`struct Class;`）で解けます。
**Mython には前方宣言がありません。**モジュールは一方通行の依存しか持てず、
**循環 import は第13章で禁止した**からです。

### ✍️ 判断：型は ast.my に同居させ、所有者は「名前」で持つ

```
ast.my    … Node / Type / Class / Field  ← 相互参照するものを 1 つの島に
types.my  … FuncSig / VarEntry / Scope / ModuleSyms
```

それでも `Class → ModuleSyms` が残ります。ここは**ポインタをやめました**。

```python
class Class:
    owner_name: str    # ★ ModuleSyms への参照ではなく「モジュール名」
```

```python
owner: types.ModuleSyms = self.syms_by_name(c.owner_name)
```

**★ 「参照の代わりに名前で引く」は、循環を切る古典的な手です。**
引くコストは増えますが（モジュールは数個なので実質ゼロ）、
**依存の向きが一方通行になります。**

> **⚠️ これは「言語の制限が設計を決めた」例です。**
> 循環 import を禁止した判断（第13章 150）の**代償**が、ここで初めて出ました。
> 代償を払う価値があったかは第20章で振り返ります。

---

## 18.4 sema.my — 2148 行を移す

### 📖 移す順番

| 順 | C 版 | Mython 版 | 行数の目安 |
|---|---|---|---|
| 1 | `Sema` 構造体・表を引く関数 | `Sema` クラス | 300 |
| 2 | `resolve_type` / `no_implicit_hint` | 同名 | 100 |
| 3 | 式の検査（`check_binop` …） | 同名 | 300 |
| 4 | 文の検査・絞り込み | 同名 | 300 |
| 5 | 組み込み・リスト・添字 | 同名 | 200 |
| 6 | フィールド・メソッド・生成・呼び出し | 同名 | 400 |
| 7 | 宣言の登録（3 パス） | 同名 | 350 |

**★ 診断の文言は「1 文字も変えない」。**
`diag_fmt("...")` の書式文字列を、そのまま文字列連結に書き換えるだけです。

```c
d.message = diag_fmt("メソッド '%s' は %d 個の引数を取りますが、%d 個渡されました",
                     mname, f->nparams - 1, nargs);
```

```python
"メソッド '" + mname + "' は " + str(f.nparams - 1) +
" 個の引数を取りますが、" + str(nargs) + " 個渡されました"
```

### ⚠️ `T | None` を開く場所が増える

C 版は「NULL のはずがない」ポインタをそのまま使えます。Mython は開かねばなりません。

```python
def unwrap_type(t: ast.Type | None) -> ast.Type:
    if t is None:
        panic("sema: 型がありません")
        return ast.Type(ast.TY_INT)
    else:
        return t
```

**★ 第17章で作った `tok_of()` と同じ形を、型・クラス・シグネチャにも用意します。**
「型の上では None かもしれないが、実際には必ずある」ものを**1 か所で開く**。

### ⚠️ 絞り込みが効かない書き方を 3 つ踏んだ

```python
if ca is None or cb is None:     # ❌ or では絞れない
    return False
return ca == cb                  # ← ca が Class | None のまま

if gc is not None and wc is not None and gc.name == wc.name:
                                 # ❌ 同じ条件式の中では、左で絞った変数を右で使えない
```

**★ 第15章で決めた仕様どおりの動作です**（15.5 節の表）。
書き直せば済みますが、**「使ってみて初めて不便が分かる」**の 4 回目でした。

```python
if ca is None:
    return False
if cb is None:
    return False
return ca == cb      # ✅ ガード節なら絞れる
```

---

## 18.5 移植で見つかった 3 つ目のバグ

第17章では `t0` という変数名が一時値 `%tN` と衝突しました。
この章では——**`entry` という変数名**です。

```llvm
define ptr @sema.Sema.check(ptr %self.arg) {
entry:                        ← ★ ラベル
  %entry = alloca ptr         ← 利用者の変数
```

```
build/stage1-check.sema.ll:8965:3: error: multiple definition of local value named 'entry'
```

**LLVM では、ラベルとローカル値が同じ名前空間にいます。**
`sema.my` に `entry: module.Module | None = ...` と書いた瞬間に踏みました。

### ✍️ 直し方：`entry` を予約する

コンパイラが作る名前は `.` を含める約束です（`%t.0` / `if.then.0` / `for.ix.0`）。
**`entry` だけは LLVM の慣習を優先して `.` を入れていません。**
なので、**その 1 語だけを予約します。**

```c
static bool name_used(Sema *s, const char *name) {
    if (strcmp(name, "entry") == 0) return true;   // ★ 予約
    ...
}
```

利用者が `entry` という変数を書くと、IR 上は `%entry.1` になります。

> **★ 3 章連続で「実プログラムでないと踏まないバグ」が出ました。**
> ch17: `t0`（第1章から）／ch18: `entry`（第1章から）。
> **どちらも 5000 行の実プログラムを書いた初日に出ています。**

---

## 18.6 標準ライブラリの場所を揃える

C 版はビルド時に埋め込みます。

```makefile
CFLAGS += -DMYTHON_LIB_DIR='"$(abspath lib)"'
```

**Mython にプリプロセッサはありません。** stage1 は実行時に知るしかない。

### ✍️ 判断：環境変数で揃える

```c
// src/module.c
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
    return DEFAULT_LIB_DIR      # "lib"
```

**★ 「両方が同じ規則で探す」ようにするのがポイントです。**
C 版だけを直しても、stage1 だけを直しても、診断のパスが食い違います。

`getenv` はランタイムに足しました（`sys.getenv`）。
**これも言語ではなくライブラリです。**

---

## 18.7 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ 診断まで完全一致した

```bash
$ make test
全 312 件パス

トークン列一致 346 件 / 字句エラーの内容一致 9 件
AST 一致 309 件 / 構文エラーの内容一致 37 件
型検査 一致 173 件 / 型エラーの内容一致 136 件
```

**型エラー 136 件のメッセージが、1 バイトも違わず一致しました。**
罫線・キャレット・note ブロック・ヒントの字下げ——すべてです。

そして **173 件の正しいプログラムを、どちらも「エラー無し」で通しました。**
**「出す」だけでなく「出さない」も一致しています。**

### ★ 段階ごとの検証が 3 本になった

```
① トークン列   （第16章）
② AST（S 式）  （第17章）
③ 型検査の診断 （第18章）
```

**★ どの段階で食い違ったかが、そのまま分かる形を保っています。**
第19章で IR、第20章で不動点が加わって 5 本になります。

### ✅ 日本語を含む行でもキャレットが合う

```
error: 閉じ括弧 ')' がありません
  --> tests/cases/err_unclosed_paren_utf8.my:4:31
   |
 4 |     return (1 + 2   # 日本語のコメント
   |                               ^ ここに ')' が必要です
note: 対応する '(' はここです
```

C 版・stage1 ともに**同じ位置**です。
バイト数・文字数・表示幅の 3 つを使い分けた結果が、ここに出ます。

### ✅ 速度

```
C 版 (--check)    real 0.03
stage1-check      real 0.05
```

`selfhost/check.my`（＝ stage1 自身の全モジュール、約 5000 行）を型検査した時間です。
**C 版の約 1.7 倍**——第16章 2.0 倍、第17章 1.6 倍と同じ水準を保っています。

### 📖 規模

| | C 版 | Mython 版 |
|---|---|---|
| `diag` | 275 行 | 295 行 |
| `types` / `ast` | 261 + 532 行 | 586 + 80 行 |
| `module` | 264 行 | 264 行 |
| `sema` | 2148 行 | 1949 行 |
| **selfhost 合計** | — | **5038 行** |

**★ Mython 版のほうが短い**のは、診断の組み立てが
`Diag` 構造体の代入ではなく**関数呼び出し 1 回**で済むからです。

```c
Diag d = {0};
d.message = ...; d.primary.tok = ...; d.primary.label = ...;
d.related.tok = ...; d.related.label = ...; d.hint = ...;
diag_fail(&d);
```

```python
diag.error_full(tok, message, label, rel_tok, rel_label, hint)
```

**⚠️ ただし引数 6 個の関数は読みにくい**という代償があります。
C 版の「名前付き代入」のほうが、**どれが何かは分かりやすい**。

### ⚠️ この章で見つけた C 版のバグ（3 章連続）

```llvm
entry:                   ← ラベル
  %entry = alloca ptr    ← 利用者の変数 entry
```

```
error: multiple definition of local value named 'entry'
```

**第1章から存在していたバグです。** `sema.my` に
`entry: module.Module | None` と書いた瞬間に出ました。

| 章 | 見つけたバグ | いつから |
|---|---|---|
| ch17 | `t0` という変数が一時値と衝突 | 第1章 |
| ch17 | `--dump-ast` が `import` で落ちる | 第13章 |
| ch17 | `ND_NONE` が改行を出さない | 第15章 |
| **ch18** | **`entry` という変数がラベルと衝突** | **第1章** |

**★ 「実プログラムを 1 つ書く」ことが、
テストケースを 300 個足すより強いことがあります。**

### ⚠️ 絞り込みが効かない書き方（第15章の仕様どおり）

移植中に 3 回踏みました。

| 書き方 | 結果 |
|---|---|
| `if a is None or b is None: return` | ❌ `or` は絞れない |
| `if a is not None and a.x > 0:` | ❌ 同じ条件式の右では使えない |
| `if a is None: panic(...)` の後ろ | ❌ `panic` は「戻らない」と扱われない |

**すべて第15章 15.5 節の表に書いてあるとおりです。**
書き直せば済みますが、**「仕様どおりで、かつ不便」**という状態が
はっきり見えました。第20章の後に判断します。

---

## 18.8 まとめと次章の予告

### できたこと

```
✅ selfhost/diag.my  — 診断の描画（295 行）。UTF-8 の桁計算まで一致
✅ selfhost/types.my — シンボル表（80 行）
✅ selfhost/module.my — モジュール読み込みと循環検出（264 行）
✅ selfhost/sema.my  — 意味解析（1949 行）。C 版 2148 行の移植
✅ selfhost/check.my — 入口
✅ C 版に --check を追加（比較の基準）
✅ 型エラー 136 件のメッセージが完全一致・正常系 173 件が素通り
✅ C 版のバグを 1 件発見して修正（entry 衝突）
✅ lib/io.my に eprint、lib/sys.my に getenv
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `selfhost/sema.my` | **新規 1949 行**（この本で最大の 1 ファイル） |
| `selfhost/diag.my` | **新規 295 行** |
| `selfhost/module.my` | **新規 264 行** |
| `selfhost/types.my` | **新規 80 行** |
| `selfhost/ast.my` | `Type` / `Class` / `Field` を同居（+210 行） |
| `selfhost/token.my` | `src` / `line_start` / `len`（C 版と同じ形に戻す） |
| `selfhost/lexer.my` / `parser.my` | 診断を `diag.my` に載せ替え |
| `src/main.c` | `--check` |
| `src/sema.c` | **`entry` を予約**（バグ修正） |
| `src/module.c` | `MYTHON_LIB_DIR` を環境変数からも読む |
| `runtime/runtime.c` | `my_eprint` / `my_getenv` |
| `lib/io.my` / `lib/sys.my` | `eprint` / `getenv` |
| `tests/selfhost.sh` | 型検査の比較を追加（メッセージ全文） |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch3 | 診断を `Diag` の 3 点セットに統一 | **移植先も 1 つの関数で済んだ** |
| ch3 | UTF-8 の表示幅の表 | そのまま移すだけでキャレットが合った |
| ch9 | `str` は NUL 終端 + 参照 | `Token.src` がコピーを起こさない |
| ch12 | `type_equal` は定義の同一性 | クラスの `==` が参照比較なので**そのまま書けた** |
| ch13 | モジュールごとのシンボル表 | 構造をそのまま移せた |
| **ch13** | **循環 import の禁止** | **代償が出た**（18.3 節）。型を 1 つの島に集めた |
| ch14 | `extern` でライブラリに足す | `eprint` / `getenv` を**言語に触らず**追加 |
| ch15 | `T \| None` と絞り込み | 2000 行の移植で**セグフォ 0 回** |
| ch15 | ガード節の絞り込み | `unwrap_*()` が自然に書けた |
| ch16 | 「機械的に移す」規律 | 2148 行でも迷わなかった |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| **C 版に `--check` を足す** | 比較の基準を作る。第16章の `--dump-tokens` と同じ考え |
| **メッセージ全文を比べる** | 「位置が同じ」より厳しく、かつ自動で検証できる |
| **正常系も比べる**（素通りすること） | 「出す」だけでなく「出さない」も一致が要る |
| **`Type` / `Class` を `ast.my` に同居** | 循環 import を禁止したので、相互参照するものは同じ島に置くしかない |
| **`Class.owner` を「名前」で持つ** | 参照の代わりに名前で引けば、依存が一方通行になる |
| **`entry` を予約する**（C 版のバグ修正） | ラベルとローカル値が同じ名前空間。`.` を含まない生成名はこれだけ |
| **標準ライブラリの場所は環境変数で揃える** | Mython にプリプロセッサが無い。**両方が同じ規則で探す**ことが大事 |
| **`unwrap_*()` を 1 か所に用意** | 「型の上では None かもしれないが実際は必ずある」を開く場所を集める |

### ⚠️ 予想が外れたこと

**「sema は 2000 行あるから、移植に何日もかかる」と身構えていた。**

実際には**診断の文言をそのまま写す作業**がほとんどで、
迷った箇所は 3 つ（18.4 節の絞り込み）だけでした。

**理由は 2 つあります。**

1. **C 版が「診断を組み立てて 1 か所で投げる」形に統一されていた**（第3章）
2. **型が違えばコンパイルが通らない**ので、写し間違いがその場で出た

**★ 移植の難しさは行数ではなく、「元のコードの形」で決まります。**

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 絞り込みが `or` / 条件式の途中 / `panic` の後で効かない | 第20章の後に判断（第15章の仕様どおりではある） |
| `diag.error_full` の引数が 6 個 | 同上（読みやすさとの取引） |
| `Class.owner` を名前で引く（毎回線形探索） | 同上（モジュールは数個なので実害なし） |
| 標準ライブラリの場所が環境変数依存 | 第20章（インストールの話） |

### ✍️ commit する

```bash
git add -A
git commit -m "第18章: Mython で型検査器を書く"
```

---

## 次章：第19章 Mython でコード生成器を書く

**達成目標**

```bash
$ ./build/stage1-codegen tests/cases/int_42.my > mine.ll
$ ./build/mythonc -S tests/cases/int_42.my > theirs.ll
$ diff mine.ll theirs.ll        # ★ IR が完全一致すること
```

**★ ここまで来れば、stage1 は「コンパイラ」になります。**
IR を出せれば、あとは `clang` に渡すだけです。

**やること**

| ファイル | 作業 |
|---|---|
| `selfhost/codegen.my` | `src/codegen.c`（1100 行超）の移植 |
| `tests/selfhost.sh` | IR の比較を追加（4 本目） |

**⚠️ 予想される落とし穴**

- 一時値・ラベルの**連番**まで一致させる（第17章の隠し変数と同じ厳しさ）
- 文字列リテラルの重複排除（`@.str.N` の割り当て順）
- `declare` を出す順序（「使ったものだけ」なので出現順に依存する）
- バッファ 4 本（header / globals / decls / body）の連結順

### 🤔 第19章に入る前の練習問題

1. `selfhost/diag.my` の `utf8_width` で全角の判定を常に 1 にして、
   `err_unclosed_paren_utf8.my` の出力がどうずれるか見る（**必ず元に戻す**）
2. `src/sema.c` の `entry` 予約を外して、`entry` という変数を持つ
   プログラムをコンパイルし、エラーを読む
3. `sema.my` の `unwrap_type()` を消すとどれだけ書き換えが要るか数える
4. `--check` を使って、自分の書いた Mython プログラムを
   C 版と stage1 の両方に通し、出力が同じことを確かめる
5. **わざと型エラーを 1 つ作り**、C 版と stage1 の出力を `diff` する。
   一致しなかったらどこを直すべきか説明する
