# 第17章 Mython で構文解析器を書く

> **この章のゴール**
> `selfhost/parser.my` が、C 版と**まったく同じ AST**（S 式）を出す。
>
> ```bash
> $ ./build/stage1-ast tests/cases/int_42.my
> (block
>   (func main
>     (type int)
>     (block
>       (return
>         (int 42)
>       )
>     )
>   )
> )
> $ diff <(./build/stage1-ast t.my) <(./build/mythonc --dump-ast t.my) && echo 一致
> 一致
> ```

**第16章と同じやり方で、規模だけが 3 倍になります。**

| | C 版 | Mython 版 | 比 |
|---|---|---|---|
| 第16章（字句解析） | `lexer.c` 647 行 | `lexer.my` 464 行 | 0.7 |
| **第17章（構文解析）** | `parser.c` 1430 行 + `ast.c` 269 行 | `parser.my` + `ast.my` | — |

**★ そして、ここで `T | None` が本番になります。**
AST の子ノード（`lhs` / `rhs` / `els` / `body` …）は、
**ほとんどが「無いことがある」**からです。
第12章で章を分けてまで後回しにし、第15章で入れた機能の回収です。

---

## 目次

- [17.1 移植の前に：C 版のバグを 2 件直す](#171-移植の前にc-版のバグを-2-件直す)
- [17.2 ast.my — 全部入りノード](#172-astmy--全部入りノード)
- [17.3 T | None だらけの木を歩く](#173-t--none-だらけの木を歩く)
- [17.4 parser.my — 移植する](#174-parsermy--移植する)
- [17.5 脱糖も移す](#175-脱糖も移す)
- [17.6 検証：S 式で diff](#176-検証s-式で-diff)
- [17.7 動作確認](#177-動作確認)
- [17.8 まとめと次章の予告](#178-まとめと次章の予告)

---

## 17.1 移植の前に：C 版のバグを 2 件直す

移植の準備で `--dump-ast` を全ファイルに掛けたところ、**C 版が落ちました。**

```bash
$ ./build/mythonc --dump-ast lib/strings.my
mythonc internal error: src/ast.c:265: 到達しないはずのコードに来ました
```

### ⚠️ バグ①：`ND_IMPORT` の case が無い

第13章で `import` を足したとき、**`dump_ast` に case を足し忘れていました。**
`--dump-ast` はテストされていなかった（`--dump-tokens` だけがテスト対象だった）ので、
**1 章半のあいだ誰も気づきませんでした。**

```c
case ND_IMPORT:  // 第13章
    printf("(import %s)\n", n->name);
    break;
```

### ⚠️ バグ②：`ND_NONE` が改行を出さない

```c
case ND_NONE:
    printf("None");     // ← 改行が無く、break でもなく return
    return;
```

```
          (var b)
          None        )      ← 桁が崩れている
```

第15章で足したときに、**周りの形に合わせ忘れていました。**

```c
case ND_NONE:
    printf("(none)\n");
    break;
```

### 🤔 なぜ移植の準備でバグが出るのか

**移植は「全部の場合を 1 つずつ書き写す」作業だからです。**
`switch` の case を全部たどるので、**抜けがあれば必ず気づきます。**

> **★ セルフホストは「実装の網羅性テスト」でもあります。**
> 第16章では気づかなかったのに（字句解析は場合分けが少ない）、
> 第17章では移植を始める前に 2 件出ました。

**⚠️ そして、これは「テストが無かった」ことの証拠でもあります。**
この章の検証（17.6 節）で `--dump-ast` も全ファイルに掛かるようになります。

---

## 17.2 ast.my — 全部入りノード

### 📖 C 版と同じ「全部入り構造体」方式

[architecture.md](../design/architecture.md) 3.2 節の方針をそのまま移します。

```python
class Node:
    kind: int
    tok: token.Token | None    # 代表トークン（位置情報）

    ival: int
    sval: str
    slen: int
    op: int
    name: str

    lhs: Node | None
    rhs: Node | None
    body: Node | None
    els: Node | None
    incr: Node | None
    params: Node | None
    args: Node | None
    type_ref: Node | None
    next: Node | None
    ...
```

**⚠️ ほぼ全部が `T | None` です。**
C 版では「使わないフィールドは NULL」でした。Mython でそれを表す唯一の方法が
`T | None` です。**第15章が無ければ、この章は書けません。**

### ✍️ ノード種別も int 定数

第16章の `TK_*` と同じ手です（Mython に enum が無い）。

```python
ND_INT: int = 0
ND_BOOL: int = 1
...
ND_IMPORT: int = 28
ND_NONE: int = 29
```

**★ 値と順序は C 版の `NodeKind` に揃えます。**
順序が同じなら、C 版を見ながら 1 つずつ確認できます。

---

## 17.3 T | None だらけの木を歩く

### ⚠️ これが第17章のいちばんの手間

```c
// C 版：NULL チェックは「必要なときだけ」
dump(n->lhs, depth + 1);        // dump が NULL を受け取れる
```

```python
# Mython 版：型が「無いかもしれない」と言ってくるので、必ず開く
def dump(n: Node | None, depth: int) -> None:
    if n is None:
        indent(depth)
        print("(nil)")
        return
    # ★ ここから下では n は Node
```

**★ 入口で 1 回開けば、その関数の中ではずっと `Node` です。**
第15章の絞り込み（narrowing）が、そのまま「C 版の NULL チェック」の位置に来ます。

### 📖 子をたどるループ

```c
for (Node *el = n->body; el; el = el->next) dump(el, depth + 1);
```

```python
el: Node | None = n.body
while el is not None:
    dump(el, depth + 1)
    el = el.next          # ★ 代入で絞り込みが解除される（15.5 節）
```

**★ 第15章で「代入したら絞り込みを解除する」と決めた形が、
そのまま C の `for` ループの移植になります。**
`el = el.next` の後に `el.kind` と書けば、**エラーになります**——
それが正しい動作です（`next` は `None` かもしれない）。

### ⚠️ 「絞ってから渡す」が要る場所

```python
# ✗ フィールドは絞り込めない（15.5 節）
if n.lhs is not None:
    dump(n.lhs, depth + 1)      # ← n.lhs は Node | None のまま

# ✓ 引数の型を Node | None にしておけば、そのまま渡せる
dump(n.lhs, depth + 1)
```

**★ 木を歩く関数は、最初から `Node | None` を受け取る形にします。**
そうすれば呼び出し側で絞り込む必要がなくなり、**C 版と同じ書き方**になります。
「絞り込みは受け取った側で 1 回」——これが Mython での定石です。

---

## 17.4 parser.my — 移植する

### 📖 移す順番（依存の少ない順）

| 順 | C 版 | Mython 版 |
|---|---|---|
| 1 | `peek` / `advance` / `consume` / `expect` | `Parser` のメソッド |
| 2 | `primary` → `postfix` → `power` → `unary` | 同名 |
| 3 | `mul_expr` … `bitor_expr` | 同名（**順序も同じ**） |
| 4 | `comparison` / `not_expr` / `and_expr` / `or_expr` | 同名 |
| 5 | `var_decl` / `aug_assign` / `simple_stmt` | 同名 |
| 6 | `block` / `if_stmt` / `while_stmt` / `for_stmt` | 同名 |
| 7 | `type_ref` / `param` / `func_def` | 同名 |
| 8 | `class_def` / `field_decl` / `extern_def` / `import_stmt` | 同名 |
| 9 | `program` / `parse` | 同名 |

**⚠️ 優先順位の階層（3〜4）は、関数を 1 つも省略しないこと。**
「`mul_expr` と `add_expr` をまとめれば短くなる」は**改善**であり、
第16章で禁じた行為です（16.1 節）。

### ✍️ Parser の状態

```python
class Parser:
    toks: list[token.Token]
    pos: int
    hidden: int      # 隠し変数の連番（第11章の for の脱糖で使う）
```

C 版の `Parser` とフィールドまで 1 対 1 です。

### ⚠️ 「代表トークン」の持ち回り

C 版は `Token *` を持ち回りますが、Mython では `token.Token | None` になります。
**エラー報告のたびに絞り込むのは煩雑**なので、
`Node.tok` は**必ず入れる**約束にして、取り出しは 1 か所にまとめます。

```python
def tok_of(n: Node) -> token.Token:
    t: token.Token | None = n.tok
    if t is None:
        panic("ast: node without token")   # ★ 起きたらコンパイラのバグ
    return t
```

**★ 「起きないはず」を `panic` で受けるのは、第1章の `UNREACHABLE()` と同じ手です。**

---

## 17.5 脱糖も移す

構文解析器の中には**脱糖（desugaring）**が 3 つ入っています。

| 脱糖 | 章 | 移植で気をつけること |
|---|---|---|
| `elif` → `else` の中の `if` | ch7 | 木の形が変わるので、S 式で一致を確認できる |
| `x += 1` → `x = x + 1` | ch5 | **左辺を 1 回だけ評価**する（`xs[f()] += 1`） |
| `for` → `while` | ch11 | 隠し変数の**連番まで一致**させる（`for.ix.0`） |

**⚠️ 隠し変数の名前は S 式に出ます。**

```
(vardecl for.ix.0 ...
```

**つまり、連番の振り方が違うと diff が出ます。**
`Parser.hidden` の増やし方を C 版と揃えてください——
**これは「同じ結果を出す」ことの、いちばん厳しい確認になります。**

---

## 17.6 検証：S 式で diff

第16章の `tests/selfhost.sh` に**もう 1 本**足します。

```bash
# ① トークン列（第16章）
./build/mythonc --dump-tokens "$f" | diff - <(./build/stage1-lexer "$f")

# ② AST（第17章）
./build/mythonc --dump-ast "$f"    | diff - <(./build/stage1-ast "$f")
```

**★ 検証は「段階ごとに 1 本」増やします。**
第18章では型検査の結果、第19章では IR が加わります。
**どの段階で食い違ったかが、すぐ分かる形を保ちます。**

### ⚠️ 構文エラーのファイルは位置で比べる

`tests/cases/err_*.my` の多くは**構文エラー**です。
第16章と同じく、**エラーの位置（行:桁）だけ**を比べます。

---

## 17.7 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ AST が完全一致した

```bash
$ make test
全 312 件パス

トークン列一致 341 件 / 字句エラーの位置一致 9 件
AST 一致 304 件 / 構文エラーの位置一致 37 件
```

**304 ファイルで S 式がバイト単位で一致**し、
**構文エラー 37 件も位置（行:桁）が一致**しました。

比較対象には `selfhost/parser.my` 自身も含まれます。
**構文解析器が、自分自身のソースを C 版と同じ木に解析できています。**

### ★ 脱糖まで一致した（この章のいちばん厳しい確認）

`for` の脱糖は**隠し変数の名前**を作ります。名前には連番が入ります。

```
(vardecl for.it.0 ...
(vardecl for.ix.1 ...
```

**連番の振り方が 1 つでもずれたら diff が出ます。**
`elif` の脱糖（`else` の中の `if`）、複合代入の脱糖（`aug.obj.N` / `aug.idx.N`）も
同じです。**3 つの脱糖すべてが、木の形と名前まで一致しました。**

### ⚠️ 移植の準備で C 版のバグが 2 件、移植中に 1 件見つかった

**① `--dump-ast` が `import` で落ちる**（第13章の抜け）

```bash
$ ./build/mythonc --dump-ast lib/strings.my
mythonc internal error: src/ast.c:265: 到達しないはずのコードに来ました
```

`ND_IMPORT` の case を書き忘れていました。**1 章半のあいだ誰も気づかなかった**のは、
`--dump-ast` にテストが無かったからです。この章の検証で**毎回 300 ファイルに掛かる**
ようになりました。

**② `ND_NONE` が改行を出していなかった**（第15章の抜け）

```
          (var b)
          None        )      ← 桁が崩れている
```

**③ ★ `t0` という変数を書くとコンパイルが壊れる**（第1章からのバグ）

移植した `parser.my` に `t0: token.Token = self.peek()` と書いたところ、
生成された IR が `clang` に弾かれました。

```
build/stage1-ast.parser.ll:2009:3: error: multiple definition of local value named 't0'
```

```llvm
  %t0 = alloca ptr        ; 利用者の変数 t0
  %t0 = load ptr, ...     ; コンパイラの一時値
```

**コンパイラの一時値 `%tN` と、利用者の変数名が衝突していました。**
第1章から 16 章ぶんのあいだ、**誰も `t0` という変数を書かなかっただけ**です。

直し方は、この本で 4 回目の同じ手です。

```c
snprintf(buf, 24, "%%t.%d", e->tmp_counter++);   // ★ '.' を入れる
```

利用者の識別子に `.` は入れられないので、**衝突は原理的に起きません**
（第11章の `for.ix.0`、第12章の `Token.show`、第13章の `lexer.Token`）。

> **★ セルフホストは「もっとも厳しいテスト」です。**
> 4000 行の実プログラムを自分で書くと、
> **人間のテストが踏まない場所を必ず踏みます。**

### ✅ 速度

1.66MB（45,311 行）の入力で、字句解析＋構文解析＋S 式の出力：

```
C 版 (--dump-ast)   real 0.30
stage1-ast          real 0.47
```

**C 版の約 1.6 倍**です（第16章の字句解析だけのときは 2 倍でした）。
**構文解析の比率が増えるほど差が縮んでいます。**

### ⚠️ 移植して分かったこと

**① `panic()` が「戻らない」と分かってもらえない。**

```python
def tok_of(self, n: ast.Node) -> token.Token:
    t: token.Token | None = n.tok
    if t is None:
        panic("parser: node without token")
        return self.peek()      # ← ★ panic は戻らないのに、書かないと通らない
    else:
        return t
```

C 版は `_Noreturn void my_panic(...)` と書けるので、この問題が起きません。
**stage0 には「戻らない関数」という概念がありません。**

**⚠️ ここでも stage0 は直しません**（第16章の規律）。
`while True:` と合わせて**2 件目**の「到達可能性検査の穴」として記録します。

**② バックスラッシュによる行継続が無い。**

```python
if (lhs.kind != ast.ND_VAR and lhs.kind != ast.ND_INDEX and
        lhs.kind != ast.ND_FIELD):      # ★ 括弧の中なら改行できる（第4章）
```

第4章で「括弧の中では改行を無視する」を実装してあったので、**書けました**。
`\` を足す必要はありません。

**③ ダミーの先頭ノードが冗長になる。**

C 版の `Node head = {0}; Node *cur = &head;` は、Mython だと
`ast.new_node(ND_BLOCK, tok)` を毎回作ることになります。
**構造は同じですが、ゴミが増えます。**（第20章の後に測ります）

**④ 表示用の桁（文字数）を字句解析器が持つことにした。**

C 版は `Token.col` に**バイト数**を持ち、表示のとき（`src/diag.c`）に
**文字数**へ直しています。stage1 にはまだ `diag` が無いので、
**字句解析器が両方を持つ**ことにしました（`col` と `dcol`）。

**⚠️ これは一時的な形です。第18章で `diag.my` を移植したら、C 版と同じ形
（トークンは行頭の位置を持ち、描画側が数える）に直します。**

---

## 17.8 まとめと次章の予告

### できたこと

```
✅ selfhost/ast.my   — Node（全部入り）と S 式ダンプ（376 行）
✅ selfhost/parser.my — 再帰下降構文解析器（1034 行）
✅ selfhost/dump_ast.my — C 版と同じ書式で出す入口
✅ 3 つの脱糖（elif / 複合代入 / for）を隠し変数の連番まで一致させた
✅ AST 一致 304 件 / 構文エラーの位置一致 37 件
✅ C 版のバグを 3 件発見して修正（dump の抜け 2 件、%tN の衝突 1 件）
✅ C 版の約 1.6 倍の速度
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `selfhost/ast.my` | **新規 376 行**（C 版 `ast.h` + `ast.c` は 532 行） |
| `selfhost/parser.my` | **新規 1034 行**（C 版 `parser.c` は 1430 行） |
| `selfhost/dump_ast.my` | **新規 24 行** |
| `selfhost/token.my` | `dcol`（表示用の桁）を追加 |
| `src/ast.c` | **バグ修正 2 件**（`ND_IMPORT` の case、`ND_NONE` の改行） |
| `src/codegen.c` | **バグ修正 1 件**（一時値を `%t.N` に） |
| `docs/design/ir-conventions.md` | 一時値の命名規約を更新（履歴つき） |
| `tests/selfhost.sh` | AST の比較を追加 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch2 | `--dump-ast`（S 式）を作った | **そのまま「正解」になった** |
| ch3 | 位置はトークンが持つ | ノードに `tok` を持たせるだけで診断が動く |
| ch4 | 括弧の中では改行を無視 | **行継続なしで長い条件が書けた** |
| ch5/7/11 | 3 つの脱糖を構文解析器に置いた | 移植先も構文解析器 1 か所で済んだ |
| ch11 | 隠し変数に `.` と連番 | **連番まで一致させる**という厳しい検証になった |
| ch12 | class とメソッド | `Parser` / `Node` をクラスで表せた |
| **ch15** | **`T \| None`** | **AST の子ノードすべてがこれ。無ければ書けない** |
| ch15 | 絞り込みの解除（代入で戻る） | `cur = cur.next` の形が C の `for` ループの移植になった |
| ch16 | 「機械的に移す」規律 | 1430 行を**迷いなく**移せた |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| **木を歩く関数は `Node \| None` を受け取る** | 呼び出し側で絞り込まずに済み、C 版と同じ書き方になる。「絞り込みは受け取った側で 1 回」 |
| **`tok_of()` で代表トークンを取り出す** | `Node.tok` は `T \| None` だが、実際には必ずある。**1 か所で開いて panic で受ける**（C 版の `UNREACHABLE()` と同じ） |
| **引数リストの読み取りを関数にまとめた** | C 版は 3 か所に展開しているが、ダミー先頭の扱いが Mython だと冗長。**構造は同じ**なので許容範囲と判断 |
| **`dcol`（表示用の桁）を Token に持たせた** | `diag` がまだ無いため。**第18章で C 版と同じ形に戻す**と決めて先に進む |
| **見つけた C 版のバグは直す** | 「改善しない」の対象は**移植の書き方**であって、**バグは別**。正解が壊れていたら比較にならない |

### ⚠️ 予想が外れたこと

**「T | None だらけで書きにくい」と予想していたが、実際には楽だった。**

木を歩く関数の入口で 1 回開けば、あとは `Node` として扱えます。
**C 版の NULL チェックと同じ位置に、同じ回数だけ書くことになりました。**

むしろ効いたのは**「絞り込みが代入で解除される」**（第15章 15.5 節）ほうで、

```python
cur = cur.next
```

の後に `cur.kind` と書くと**エラーになります**。
C 版なら**そのまま NULL 参照でクラッシュ**するところです。

**★ 移植中に一度もセグフォしませんでした。** これは C からの移植として異例です。

### 既知の課題（担当章で解決する）

| 課題 | 解決する章 |
|---|---|
| 診断の描画（`-->` の抜粋・note・ヒント） | 第18章（`diag.my`） |
| `Token.dcol` という一時的な形 | 同上 |
| `panic` が「戻らない」と扱われない | 未定（`while True:` と合わせて 2 件目） |
| ダミー先頭ノードのゴミ | 第20章の後（測ってから） |

### ✍️ commit する

```bash
git add -A
git commit -m "第17章: Mython で構文解析器を書く"
```

---

## 次章：第18章 Mython で型検査器を書く

**達成目標**

```bash
$ ./build/stage1-check tests/cases/err_arg_type.my
error: 関数 'f' の第 1 引数: 型 'str' を 'int' に渡せません
   --> tests/cases/err_arg_type.my:6:12
    |
  6 |     return f("a")
    |              ^^^ これは 'str' 型です
```

**★ この章で診断（`src/diag.c`）も移植します。**
「エラーメッセージまで一致させる」ところまで行けば、
**C 版と stage1 の区別がほぼ付かなくなります。**

**やること**

| ファイル | 作業 |
|---|---|
| `selfhost/diag.my` | 診断の描画（抜粋・キャレット・note・ヒント） |
| `selfhost/types.my` | `Type` と `type_equal` / `type_assignable` |
| `selfhost/sema.my` | 意味解析（`src/sema.c` は 1800 行超。**最大の山**） |
| `tests/selfhost.sh` | エラーメッセージ全体の比較を追加 |

**⚠️ 予想される落とし穴**

- `sema.c` は**この本でいちばん大きいファイル**（1800 行超）
- スコープ・シンボルテーブル・絞り込みの状態管理が複雑
- 診断は**バイト単位で一致**させる（UTF-8 の桁計算、罫線の幅）
- 型検査は「エラーが出る」だけでなく「**出ない**」ことも確認する必要がある

### 🤔 第18章に入る前の練習問題

1. `selfhost/parser.my` の `hidden_name` の連番を `self.hidden + 1` から
   固定値にして、`make selfhost-test` が何と言うか見る（**必ず元に戻す**）
2. `ast.my` の `dump_siblings` を `dump` に統合しようとして、
   なぜ分けてあるのかを考える
3. C 版の `%t.N` を `%tN` に戻して、`t0` という変数を持つプログラムを
   コンパイルし、エラーメッセージを読む
4. `parser.my` と `parser.c` を並べて、**関数の順序が同じか**確かめる
5. **自分で構文を 1 つ足してみる**（例：`x if c else y`）。
   C 版と Mython 版の両方を直さないと `make test` が通らないことを体験する
