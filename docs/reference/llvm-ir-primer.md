# LLVM IR 入門 — 手で書いて動かす

> LLVM を触ったことがない人向けの導入です。
> **読むだけでなく、必ず手を動かしてください。** 30 分で終わります。
>
> ここで LLVM IR が「読めて書ける」ようになれば、コード生成器を書く準備は完了です。

---

## 1. LLVM IR とは何か

**LLVM IR は「移植可能なアセンブリ言語」です。**

```
私たちの仕事              LLVM の仕事
─────────────────       ────────────────────────────
Mython → LLVM IR   →    LLVM IR → x86-64 の機械語
                        LLVM IR → ARM64 の機械語
                        LLVM IR → RISC-V の機械語
                        （+ 最適化）
```

IR には 3 つの表現形式があり、**内容は同じ**です。

| 形式 | 拡張子 | 用途 |
|---|---|---|
| **テキスト形式** | `.ll` | 人間が読める。**この教材で生成するもの** |
| ビットコード形式 | `.bc` | バイナリ。ファイルが小さく、読み込みが速い |
| メモリ内表現 | — | LLVM ライブラリ内のオブジェクト（C API で操作する形式） |

`llvm-as`（.ll → .bc）と `llvm-dis`（.bc → .ll）で相互変換できます。

---

## 2. 最初の IR：何もしない main

### ✍️ 手を動かす

```bash
mkdir -p /tmp/irlab && cd /tmp/irlab
cat > 01.ll <<'EOF'
define i32 @main() {
entry:
  ret i32 42
}
EOF
clang 01.ll -o 01
./01
echo "exit code = $?"
```

### ✅ 確認

```
exit code = 42
```

### 📖 解説：1 行ずつ読む

```llvm
define i32 @main() {
```

| 部分 | 意味 |
|---|---|
| `define` | 関数を**定義**する（本体を持つ）。宣言だけなら `declare` |
| `i32` | 戻り値の型。**`i` + ビット数**で整数型を表す |
| `@main` | 関数名。`@` は**グローバルな名前**の印 |
| `()` | 引数リスト（空） |
| `{` | 本体の開始 |

```llvm
entry:
```

**基本ブロック（basic block）のラベル**です。
基本ブロックとは「途中で分岐せず、上から下へ一直線に実行される命令の並び」です。
関数の最初のブロックの名前は慣習的に `entry` にします（名前は何でもよい）。

```llvm
  ret i32 42
```

`ret` は関数から戻る命令。**型を必ず書く**のが LLVM IR の特徴です。
`ret 42` ではなく `ret i32 42` と書きます。

### 🤔 なぜ型を毎回書くのか

LLVM IR は**強く型付けされた**中間表現です。
すべての値・すべての命令に型があり、検証器（verifier）が整合性をチェックします。

冗長に見えますが、これは私たち（IR を生成する側）にとって**利点**です。
型を間違えた IR は、実行時に不思議な動作をするのではなく、
**その場で明確なエラーになります**。

```
error: '%t0' defined with type 'i64' but expected 'i32'
```

---

## 3. 整数型

LLVM の整数型は `i` + ビット数です。**符号の情報は型に含まれません。**

| 型 | ビット数 | 主な用途 |
|---|---|---|
| `i1` | 1 | 真偽値（比較結果、条件分岐） |
| `i8` | 8 | バイト、`bool` のメモリ表現 |
| `i32` | 32 | C の `int`、`main` の戻り値 |
| `i64` | 64 | **Mython の `int`** |

### ⚠️ 符号は「型」ではなく「命令」が決める

```llvm
%a = sdiv i64 %x, %y     ; signed division   （符号付き除算）
%b = udiv i64 %x, %y     ; unsigned division （符号なし除算）

%c = icmp slt i64 %x, %y ; signed less than    （符号付き比較）
%d = icmp ult i64 %x, %y ; unsigned less than  （符号なし比較）

%e = ashr i64 %x, 1      ; arithmetic shift right（符号を保つ右シフト）
%f = lshr i64 %x, 1      ; logical shift right   （0 埋め右シフト）
```

**Mython の `int` は符号付き**なので、必ず `s` の付く命令（`sdiv`, `srem`, `icmp slt`, `ashr`）を使います。
`u` 版を使うと負数で誤動作します。**これは実際によくやるバグです。**

---

## 4. 計算する：1 + 2 * 3

### ✍️ 手を動かす

```bash
cat > 02.ll <<'EOF'
define i32 @main() {
entry:
  %t0 = mul i64 2, 3
  %t1 = add i64 1, %t0
  %t2 = trunc i64 %t1 to i32
  ret i32 %t2
}
EOF
clang 02.ll -o 02 && ./02; echo "exit code = $?"
```

### ✅ 確認

```
exit code = 7
```

### 📖 解説

```llvm
  %t0 = mul i64 2, 3
```

- `%t0` … **ローカルな名前**。`%` はローカル、`@` はグローバル
- `mul i64` … 64bit 整数の乗算
- `2, 3` … オペランド。**即値を直接書ける**

```llvm
  %t2 = trunc i64 %t1 to i32
```

`trunc`（truncate）は**ビット幅を縮める**変換です。
`main` は `i32` を返す約束なので、`i64` の計算結果を `i32` に切り詰めます。

| 命令 | 変換 | 例 |
|---|---|---|
| `trunc` | 幅を縮める | `i64` → `i32`, `i8` → `i1` |
| `zext` | 幅を広げる（0 埋め） | `i1` → `i8`（`bool` の保存） |
| `sext` | 幅を広げる（符号拡張） | `i32` → `i64`（符号付き整数） |
| `sitofp` | 符号付き整数 → 浮動小数 | `i64` → `double`（`float(x)`） |
| `fptosi` | 浮動小数 → 符号付き整数 | `double` → `i64`（`int(x)`） |

### 🤔 SSA：`%t0` に 2 回代入できない

```bash
cat > 03bad.ll <<'EOF'
define i32 @main() {
entry:
  %x = add i64 1, 0
  %x = add i64 2, 0
  ret i32 0
}
EOF
clang 03bad.ll -o /dev/null
```

```
error: multiple definition of local value named 'x'
```

**LLVM IR のレジスタは 1 回しか定義できません。**
これが **SSA (Static Single Assignment)** 形式です。

「じゃあ変数の再代入はどう書くのか」という問題への答えが、次の節です。
これは Mython コンパイラの設計を決める最重要ポイントです。

---

## 5. 変数：alloca / store / load

再代入できる変数は、**レジスタではなくメモリ（スタック）**に置きます。

### ✍️ 手を動かす

```bash
cat > 04.ll <<'EOF'
define i32 @main() {
entry:
  %x = alloca i64          ; 8 バイトの箱をスタックに確保。%x はその箱のアドレス
  store i64 10, ptr %x     ; x = 10
  store i64 20, ptr %x     ; x = 20   ← 何度でも書ける！
  %t0 = load i64, ptr %x   ; x を読む
  %t1 = add i64 %t0, 2     ; 20 + 2
  store i64 %t1, ptr %x    ; x = 22
  %t2 = load i64, ptr %x
  %t3 = trunc i64 %t2 to i32
  ret i32 %t3
}
EOF
clang 04.ll -o 04 && ./04; echo "exit code = $?"
```

### ✅ 確認

```
exit code = 22
```

### 📖 解説：3 つの命令

```llvm
  %x = alloca i64
```

**スタック上に `i64` 1 個分の領域を確保**し、そのアドレスを `%x` に入れます。
`%x` の型は `ptr`（ポインタ）です。**`%x` は「値」ではなく「箱の場所」**です。

```
  スタック
  ┌──────────┐
  │  ????    │ ← %x はこの箱のアドレスを指す
  └──────────┘
```

```llvm
  store i64 10, ptr %x
```

`store <型> <値>, ptr <アドレス>` で書き込みます。

```llvm
  %t0 = load i64, ptr %x
```

`load <型>, ptr <アドレス>` で読み出します。

### ⚠️ opaque pointer：`ptr` は型情報を持たない

LLVM 15 以降、ポインタ型はすべて `ptr` です。

```llvm
; ❌ 古い書き方（LLVM 14 以前）— 今はエラーになる
%v = load i64, i64* %p

; ✅ 今の書き方
%v = load i64, ptr %p
```

だから `load` / `store` は**指す先の型を命令側に書く**必要があります。
インターネットの古い解説は `i8*` や `i64*` を使っているので、読み替えてください。

### 🤔 alloca は遅くないのか → mem2reg が消す

```bash
$(brew --prefix llvm)/bin/opt -passes=mem2reg -S 04.ll
```

```llvm
define i32 @main() {
entry:
  %t1 = add i64 20, 2
  %t3 = trunc i64 %t1 to i32
  ret i32 %t3
}
```

**alloca / store / load が全部消えました。**
`%x` という箱は無くなり、値が直接レジスタで受け渡されています。

**⚠️ ここで正確に理解しておくこと**：`mem2reg` は
「メモリ経由の受け渡しをレジスタ経由に変える」パスであり、**定数の計算まではしません**。
`add i64 20, 2` はそのまま残っています。

定数畳み込みまで見たいなら、最適化パイプライン全体を掛けます。

```bash
$(brew --prefix llvm)/bin/opt -O2 -S 04.ll
```

```llvm
define noundef i32 @main() local_unnamed_addr #0 {
entry:
  ret i32 22
}
```

**今度は答えだけになりました。**
`mem2reg` が「素朴な形を SSA に直す」土台を作り、
その上で他のパス（定数畳み込み、不要コード削除）が仕事をする、という分業です。

これが Mython のコード生成戦略の根拠です。

> **私たちは「全部 alloca」で素朴に書く。LLVM が SSA に直す。**

詳細は [../design/ir-conventions.md](../design/ir-conventions.md) の第1節にあります。

---

## 6. 関数と引数

### ✍️ 手を動かす

```bash
cat > 05.ll <<'EOF'
define i64 @add(i64 %a.arg, i64 %b.arg) {
entry:
  %a = alloca i64
  %b = alloca i64
  store i64 %a.arg, ptr %a       ; 引数をローカル変数にコピー
  store i64 %b.arg, ptr %b
  %t0 = load i64, ptr %a
  %t1 = load i64, ptr %b
  %t2 = add i64 %t0, %t1
  ret i64 %t2
}

define i32 @main() {
entry:
  %t0 = call i64 @add(i64 20, i64 22)
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
EOF
clang 05.ll -o 05 && ./05; echo "exit code = $?"
```

### ✅ 確認

```
exit code = 42
```

### 📖 解説

```llvm
define i64 @add(i64 %a.arg, i64 %b.arg) {
```

引数には**名前と型**を書きます。`%a.arg` という名前は自分で決めたものです。

```llvm
  %t0 = call i64 @add(i64 20, i64 22)
```

`call <戻り型> @<関数名>(<型> <値>, ...)`。**引数にも型を書きます。**

### 🤔 なぜ引数を alloca にコピーするのか

引数 `%a.arg` は SSA レジスタなので、**代入できません**。
Mython では引数に代入できます。

```python
def f(a: int) -> int:
    a = a + 1        # 引数への代入
    return a
```

なので引数もローカル変数と同じ扱いにします（`mem2reg` が余分なコピーを消します）。
これが [../design/ir-conventions.md](../design/ir-conventions.md) の規約 R8 です。

---

## 7. 条件分岐

### ✍️ 手を動かす

```bash
cat > 06.ll <<'EOF'
define i64 @max(i64 %a.arg, i64 %b.arg) {
entry:
  %t0 = icmp sgt i64 %a.arg, %b.arg      ; a > b ? （結果は i1）
  br i1 %t0, label %if.then.0, label %if.else.0

if.then.0:
  ret i64 %a.arg

if.else.0:
  ret i64 %b.arg
}

define i32 @main() {
entry:
  %t0 = call i64 @max(i64 3, i64 9)
  %t1 = trunc i64 %t0 to i32
  ret i32 %t1
}
EOF
clang 06.ll -o 06 && ./06; echo "exit code = $?"
```

### ✅ 確認

```
exit code = 9
```

### 📖 解説

```llvm
  %t0 = icmp sgt i64 %a.arg, %b.arg
```

`icmp <条件> <型> <左>, <右>` は比較して **`i1`（1 ビット）** を返します。

| 条件 | 意味 |
|---|---|
| `eq` / `ne` | == / != |
| `slt` / `sle` / `sgt` / `sge` | 符号付き < / <= / > / >= |
| `ult` / `ule` / `ugt` / `uge` | 符号なし |

浮動小数点は `fcmp` で、条件は `oeq`, `olt`, `ogt` などです（`o` = ordered、NaN でない）。

```llvm
  br i1 %t0, label %if.then.0, label %if.else.0
```

**条件分岐**：`%t0` が 1 なら第 1 のラベル、0 なら第 2 のラベルへ。

無条件分岐は引数が 1 つです。

```llvm
  br label %if.end.0
```

### ⚠️ 最大の落とし穴：フォールスルーが無い

```bash
cat > 07bad.ll <<'EOF'
define i32 @main() {
entry:
  %t0 = add i64 1, 2
next:
  ret i32 0
}
EOF
clang 07bad.ll -o /dev/null
```

```
error: expected instruction opcode
```

`entry` ブロックが**終端命令で終わっていません**。
C では「次の行に進む」のが自然ですが、**IR では明示的にジャンプが必要**です。

```llvm
entry:
  %t0 = add i64 1, 2
  br label %next          ; ← これが必要
next:
  ret i32 0
```

> **すべての基本ブロックは `ret` / `br` / `switch` / `unreachable` のいずれかで終わる。**

これがコード生成で最もよくハマる点です。
[../design/ir-conventions.md](../design/ir-conventions.md) の 6.2 節で、
この落とし穴を吸収する `emit_label()` 関数を用意します。

---

## 8. ループ

### ✍️ 手を動かす

1 から 10 までの和を計算します。

```bash
cat > 08.ll <<'EOF'
define i32 @main() {
entry:
  %i   = alloca i64
  %sum = alloca i64
  store i64 1, ptr %i
  store i64 0, ptr %sum
  br label %while.cond.0

while.cond.0:
  %t0 = load i64, ptr %i
  %t1 = icmp sle i64 %t0, 10        ; i <= 10
  br i1 %t1, label %while.body.0, label %while.end.0

while.body.0:
  %t2 = load i64, ptr %sum
  %t3 = load i64, ptr %i
  %t4 = add i64 %t2, %t3
  store i64 %t4, ptr %sum           ; sum += i
  %t5 = load i64, ptr %i
  %t6 = add i64 %t5, 1
  store i64 %t6, ptr %i             ; i += 1
  br label %while.cond.0            ; ループバック

while.end.0:
  %t7 = load i64, ptr %sum
  %t8 = trunc i64 %t7 to i32
  ret i32 %t8
}
EOF
clang 08.ll -o 08 && ./08; echo "exit code = $?"
```

### ✅ 確認

```
exit code = 55
```

**この IR は、Mython の `while` 文からコード生成器が出力する形とほぼ同じです。**
第7章で、この形を機械的に生成するコードを書きます。

### 🤔 mem2reg 後を見てみる

```bash
$(brew --prefix llvm)/bin/opt -passes=mem2reg -S 08.ll
```

`phi` 命令が現れます。

```llvm
while.cond.0:                                     ; preds = %while.body.0, %entry
  %sum.0 = phi i64 [ 0, %entry ], [ %t4, %while.body.0 ]
  %i.0 = phi i64 [ 1, %entry ], [ %t6, %while.body.0 ]
  %t1 = icmp sle i64 %i.0, 10
```

`phi` は「**どのブロックから来たかによって値を選ぶ**」命令です。
`%i.0` の行は「`entry` から来たなら 1、`while.body.0` から来たなら `%t6`」という意味です。
つまり「初回は 1、2 回目以降は前回の更新結果」を 1 命令で表現しています。

行末の `; preds = ...` は LLVM が付けたコメントで、
**このブロックにジャンプしてくる可能性のあるブロックの一覧**です。デバッグに便利です。

**私たちはこれを自分で書きません。** LLVM が作ってくれます。
それが「全部 alloca 方式」の価値です。

### さらに `-O2` を掛けると

```bash
$(brew --prefix llvm)/bin/opt -O2 -S 08.ll
```

```llvm
define i32 @main() {
entry:
  ret i32 55
}
```

**ループが完全に消え、答えだけが残りました。**
LLVM がループを解析して、コンパイル時に計算してしまったのです。

---

## 9. 文字列と外部関数呼び出し

### ✍️ 手を動かす

```bash
cat > 09.ll <<'EOF'
target triple = "TRIPLE_HERE"

@.str.0 = private unnamed_addr constant [15 x i8] c"hello, mython\0A\00"

declare i32 @printf(ptr, ...)

define i32 @main() {
entry:
  %t0 = call i32 @printf(ptr @.str.0)
  ret i32 0
}
EOF
# 環境の triple を埋め込む
TRIPLE=$(clang -S -emit-llvm -x c /dev/null -o - | sed -n 's/^target triple = "\(.*\)"$/\1/p')
sed -i '' "s|TRIPLE_HERE|$TRIPLE|" 09.ll
clang 09.ll -o 09 && ./09
```

### ✅ 確認

```
hello, mython
```

### 📖 解説

```llvm
@.str.0 = private unnamed_addr constant [15 x i8] c"hello, mython\0A\00"
```

| 部分 | 意味 |
|---|---|
| `@.str.0` | グローバル名。`.` から始めるのは「コンパイラが作った内部シンボル」の慣習 |
| `private` | このモジュール外から見えない |
| `unnamed_addr` | アドレスの一意性を要求しない（LLVM が同じ内容の定数を統合できる） |
| `constant` | 読み取り専用（`.rodata` に置かれる） |
| `[15 x i8]` | **`i8` が 15 個の配列型** |
| `c"..."` | バイト列リテラル |
| `\0A` | 改行（16進 2 桁で書く） |
| `\00` | **NUL 終端**。これを忘れると `printf` が暴走する |

**⚠️ 長さの数え方**：`hello, mython` は 13 文字、`\0A` で 14、`\00` で 15。
**この数を間違えるのが最頻出バグです。**

試しに `[15 x i8]` を `[14 x i8]` に書き換えるとこうなります。

```
error: constant expression type mismatch: got type '[15 x i8]' but expected '[14 x i8]'
```

**読み方に注意**：`got`（実際）が文字列リテラルから数えた本当の長さ 15、
`expected`（期待）が自分が書いた `[14 x i8]` です。
つまり「**あなたは 14 と書いたが、その文字列は 15 バイトある**」という意味です。
直感と逆に感じるかもしれませんが、正しい長さが `got` 側に出るので、
**その数字をそのまま書き写せば直ります。**

```llvm
declare i32 @printf(ptr, ...)
```

`declare` は「**この関数は他のどこかにある**」という宣言（C のプロトタイプ宣言）。
`...` は可変長引数です。

### 🤔 なぜ getelementptr が要らないのか

古い LLVM の解説では、配列からポインタを取り出すのに `getelementptr` が必要でした。

```llvm
; 昔の書き方
%t0 = getelementptr [15 x i8], [15 x i8]* @.str.0, i64 0, i64 0
%t1 = call i32 @printf(i8* %t0)
```

opaque pointer では **`@.str.0` をそのまま `ptr` として渡せます**。
グローバル変数の名前は、それ自体がアドレス（`ptr`）だからです。

---

## 10. 構造体

### ✍️ 手を動かす

```bash
cat > 10.ll <<'EOF'
%Point.type = type { i64, i64 }

define i32 @main() {
entry:
  %p = alloca %Point.type              ; 構造体をスタックに確保（16 バイト）

  ; p.x = 10
  %t0 = getelementptr %Point.type, ptr %p, i32 0, i32 0
  store i64 10, ptr %t0

  ; p.y = 32
  %t1 = getelementptr %Point.type, ptr %p, i32 0, i32 1
  store i64 32, ptr %t1

  ; return p.x + p.y
  %t2 = getelementptr %Point.type, ptr %p, i32 0, i32 0
  %t3 = load i64, ptr %t2
  %t4 = getelementptr %Point.type, ptr %p, i32 0, i32 1
  %t5 = load i64, ptr %t4
  %t6 = add i64 %t3, %t5
  %t7 = trunc i64 %t6 to i32
  ret i32 %t7
}
EOF
clang 10.ll -o 10 && ./10; echo "exit code = $?"
```

### ✅ 確認

```
exit code = 42
```

### 📖 解説：getelementptr の 2 つのインデックス

`getelementptr`（略して GEP）は **アドレス計算だけをする命令**です。
メモリにはアクセスしません。

```llvm
getelementptr %Point.type, ptr %p, i32 0, i32 1
              ~~~~~~~~~~~       ~~~~~~  ~~~~~~
                  (A)             (B)     (C)
```

| | 意味 |
|---|---|
| (A) | ベースの型。`%p` が何を指しているか |
| (B) | **`%Point.type` を配列とみなして何個目か**。単一オブジェクトなら常に `0` |
| (C) | **構造体の何番目のフィールドか**（0 起算） |

### ⚠️ (B) を間違えるとどうなるか

```llvm
getelementptr %Point.type, ptr %p, i32 1, i32 0
;                                  ^^^^ 1 にすると…
```

これは「**次の Point の x**」のアドレス、つまり `%p + 16` を指します。
確保していない領域なので、書き込むとスタックが壊れます。

**GEP のインデックスの意味を理解していないと、原因不明のクラッシュに悩まされます。**
迷ったら「(B) は 0」と覚えておけば、単一オブジェクトのフィールドアクセスは常に正しくなります。

### 🤔 パディングは LLVM が計算する

```llvm
%Mixed.type = type { i8, i64, i8 }
```

`i8` の後に `i64` が来るので、7 バイトのパディングが入ります。
GEP でフィールド 1 のアドレスを取ると、**LLVM が自動的に `+8` を計算**します。
私たちがオフセットを計算する必要はありません。

---

## 11. デバッグの道具

| やりたいこと | コマンド |
|---|---|
| IR の文法を検査 | `llvm-as foo.ll -o /dev/null` |
| IR の構造を検証（終端命令など） | `opt -passes=verify -S foo.ll -o /dev/null` |
| IR を直接実行（コンパイル不要） | `lli foo.ll; echo $?` |
| alloca が消えるか確認 | `opt -passes=mem2reg -S foo.ll` |
| 最適化後を見る | `opt -O2 -S foo.ll` |
| **C から正解を教わる** | `clang -S -emit-llvm -O0 ref.c -o -` |
| 機械語を見る | `llc foo.ll -o - ` |

### ★ 最強の手法：C に書かせる

「この機能はどう IR にすればいいのか？」で詰まったら、
**同じ意味の C を書いて clang に IR を吐かせます。**

```bash
cat > ref.c <<'EOF'
struct Point { long x; long y; };
long sum(struct Point *p) { return p->x + p->y; }
EOF
clang -S -emit-llvm -O0 ref.c -o -
```

```llvm
define i64 @sum(ptr noundef %0) #0 {
  %2 = alloca ptr, align 8
  store ptr %0, ptr %2, align 8
  %3 = load ptr, ptr %2, align 8
  %4 = getelementptr inbounds nuw %struct.Point, ptr %3, i32 0, i32 0
  %5 = load i64, ptr %4, align 8
  ...
```

**GEP の書き方が丸ごとわかります。**
この教材で扱う機能はすべて C に対応物があるので、この方法が常に使えます。

**⚠️ `-O0` を付けること。** 付けないと最適化で消えて何も学べません。

---

## 12. この教材で使う命令の全リスト

第20章までに使うのはこれだけです。**驚くほど少ない**ことに注目してください。

### 算術・論理

```
add sub mul sdiv srem            ; 整数
fadd fsub fmul fdiv fneg         ; 浮動小数
and or xor shl ashr              ; ビット演算
icmp fcmp                        ; 比較
```

### メモリ

```
alloca load store getelementptr
```

### 制御フロー

```
br ret call unreachable
```

### 型変換

```
trunc zext sext sitofp fptosi ptrtoint
```

### 使わない命令（参考）

```
phi        ← 全部 alloca 方式なので不要（LLVM が作る）
switch     ← if の連鎖で済ませる
invoke     ← 例外機構がないので不要
landingpad ← 同上
atomicrmw  ← 並行機能がないので不要
select     ← if で済ませる
```

**約 30 命令覚えれば、実用的なコンパイラが書けます。**

---

## 13. 練習問題

次に進む前に、自分で書いて動かしてみてください。答えは自分で `clang` に確認できます。

1. **階乗**：`fact(5)` = 120 を返す IR を、再帰で書く
2. **FizzBuzz**：1〜15 について `printf` で出力する IR を書く
3. **配列の合計**：`[1,2,3,4,5]` をグローバル定数として置き、GEP で走査して合計する
4. **bool の保存**：`i8` の alloca に `i1` を `zext` して保存し、`trunc` して読み出す
5. **`i64` を `double` に変換**して割り算し、結果を `printf("%f")` で出力する

**⚠️ 3 と 4 は、第10章と第6章でそのまま必要になります。** ここでやっておくと後が楽です。

---

## 14. 参考資料

- [LLVM Language Reference Manual](https://llvm.org/docs/LangRef.html) — 命令の正確な定義。**辞書として使う**（通読はしない）
- [Mapping High Level Constructs to LLVM IR](https://mapping-high-level-constructs-to-llvm-ir.readthedocs.io/) — 高級言語の構文 → IR の対応集
- `llvm.org/docs/tutorial/` — Kaleidoscope チュートリアル（C++ API 方式なので今回は方式が違うが、考え方は参考になる）

**⚠️ 古い記事に注意**：opaque pointer（LLVM 15、2022 年）より前の記事は
`i8*` 形式で書かれています。読み替えが必要です。
