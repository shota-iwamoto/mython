# ツールチェーン・チートシート

> この環境で使えるツールと、その使い方の早見表です。

---

## 1. この環境の構成（2026-08-09 時点で確認済み）

| ツール | バージョン | 場所 | 用途 |
|---|---|---|---|
| Apple clang | 21.0.0 | `/usr/bin/clang` | **C のビルド**、`.ll` → 実行ファイル |
| Homebrew LLVM | 22.1.8 | `/usr/local/opt/llvm/bin/` | `opt` / `llc` / `lli` / `llvm-as` など |
| make | GNU make | `/usr/bin/make` | ビルド |
| Xcode CLT | — | `/Applications/Xcode.app/...` | リンカ、システムヘッダ |

### ターゲット triple

```
x86_64-apple-macosx26.0.0
```

**⚠️ `clang -print-target-triple` は違う値（`x86_64-apple-darwin25.5.0`）を返します。**
IR に書くべきなのは前者です。詳細は
[../design/ir-conventions.md](../design/ir-conventions.md) 第2節を参照。

正しい取得方法：

```bash
clang -S -emit-llvm -x c /dev/null -o - | sed -n 's/^target triple = "\(.*\)"$/\1/p'
```

### PATH の設定（任意）

Homebrew LLVM のツールを毎回フルパスで書くのが面倒なら、
シェルの設定ファイル（`~/.zshrc`）に追記します。

```bash
export PATH="/usr/local/opt/llvm/bin:$PATH"
```

**⚠️ ただし注意**：これをすると `clang` も Homebrew 版になります。
Homebrew 版 clang は macOS の SDK パスを自動で見つけられないことがあり、
C のビルドで問題が出る場合があります。

**この教材の方針**：
- **`clang` は Apple 版（`/usr/bin/clang`）を使う**
- **`opt` / `llc` / `lli` は Homebrew 版をフルパスまたは変数で呼ぶ**

Makefile ではこう書きます。

```makefile
CC       = clang                          # Apple clang
LLVM_BIN = $(shell brew --prefix llvm)/bin
OPT      = $(LLVM_BIN)/opt
LLI      = $(LLVM_BIN)/lli
```

---

## 2. clang（Apple clang）

### C をビルドする

```bash
clang -std=c11 -g -O0 -Wall -Wextra -c src/lexer.c -o build/lexer.o
clang build/*.o -o build/mythonc
```

| オプション | 意味 |
|---|---|
| `-std=c11` | C11 規格を使う |
| `-g` | デバッグ情報を含める（lldb で追える） |
| `-O0` | 最適化なし（デバッグしやすい） |
| `-Wall -Wextra` | 警告を多めに出す。**必ず付ける** |
| `-Wno-unused-parameter` | 未使用引数の警告を抑制（AST を扱うと多発するので） |
| `-c` | コンパイルのみ（リンクしない） |
| `-fsanitize=address` | **メモリバグ検出**（後述） |

### `.ll` を実行ファイルにする

```bash
clang foo.ll -o foo
clang -O2 foo.ll -o foo                    # 最適化する
clang foo.ll runtime/runtime.o -o foo      # ランタイムとリンク
```

**⚠️ `.ll` を渡すときは `-std=c11` を付けないこと**（C の指定は無意味で警告が出ます）。

### C から IR を教わる（最重要テクニック）

```bash
clang -S -emit-llvm -O0 ref.c -o -         # 標準出力に IR を出す
clang -S -emit-llvm -O0 ref.c              # ref.ll に出す
```

「この構文はどう IR にするのか」で迷ったときの答えは常にここにあります。
**`-O0` を忘れないこと**（最適化されると消えて学べません）。

### 機械語（アセンブリ）を見る

```bash
clang -S -O0 ref.c -o -                    # x86-64 アセンブリ
```

---

## 3. opt — IR に最適化パスを掛ける

```bash
OPT=/usr/local/opt/llvm/bin/opt

$OPT -passes=mem2reg -S foo.ll             # alloca を SSA に変換
$OPT -passes=verify  -S foo.ll -o /dev/null # 構造の検証だけ
$OPT -O2 -S foo.ll                          # 標準の最適化パイプライン
$OPT -passes='mem2reg,instcombine' -S foo.ll # パスを並べる
```

| オプション | 意味 |
|---|---|
| `-S` | 出力をテキスト（`.ll`）にする。**付けないとビットコードが出る** |
| `-o file` | 出力先。省略すると標準出力 |
| `-passes=...` | 掛けるパスを指定 |

### よく使うパス

| パス | 効果 |
|---|---|
| `mem2reg` | `alloca`/`load`/`store` を SSA レジスタ + `phi` に変換 |
| `verify` | IR の構造的整合性を検証（終端命令、型など） |
| `instcombine` | 命令の簡約（`add x, 0` → `x` など） |
| `simplifycfg` | 制御フローグラフの簡約（空ブロックの削除など） |
| `dce` | 到達しないコードの削除 |

**⚠️ 古い書き方 `opt -mem2reg` は LLVM 13 以降で使えません。**
`-passes=mem2reg` の形（新しい PassManager）を使います。

---

## 4. lli — IR を直接実行する

```bash
LLI=/usr/local/opt/llvm/bin/lli

$LLI foo.ll
echo "exit=$?"
```

**リンクせずに IR をその場で実行**できます。開発中の確認が速くなります。

**⚠️ 制限**：外部ライブラリとのリンクが必要な IR は動かない場合があります。
`printf` や `puts`（libc）は通常動きます。自作の `runtime.o` を使うものは動きません。

その場合は `-load` でライブラリを渡すか、素直に `clang` でビルドします。

---

## 5. llvm-as / llvm-dis — IR の文法チェックと変換

```bash
AS=/usr/local/opt/llvm/bin/llvm-as
DIS=/usr/local/opt/llvm/bin/llvm-dis

$AS foo.ll -o /dev/null       # 文法チェックだけ（エラーなら行番号が出る）
$AS foo.ll -o foo.bc          # .ll → .bc
$DIS foo.bc -o foo2.ll        # .bc → .ll
```

**★ `llvm-as foo.ll -o /dev/null` は、生成した IR の文法検証に最速です。**
`clang` を通すより速く、エラーメッセージも同じくらい親切です。

---

## 6. llc — IR から機械語へ

```bash
LLC=/usr/local/opt/llvm/bin/llc

$LLC foo.ll -o foo.s          # アセンブリを出す
$LLC -filetype=obj foo.ll -o foo.o   # オブジェクトファイルを出す
$LLC -O2 foo.ll -o foo.s
```

この教材では `clang` に丸投げするので `llc` は必須ではありませんが、
「自分の IR がどんな機械語になるのか」を見たいときに使います。

---

## 7. デバッグツール

### lldb（デバッガ）

```bash
lldb ./build/mythonc
(lldb) run tests/cases/simple.my
(lldb) bt                     # クラッシュ時のバックトレース
(lldb) frame variable          # 現在のフレームの変数
(lldb) p *tok                  # 構造体の中身を表示
(lldb) b parser.c:120          # ブレークポイント
```

**⚠️ `-g` を付けてビルドしていないと変数名が見えません。**

### AddressSanitizer（メモリバグ検出）★強く推奨

```bash
clang -fsanitize=address -g -O0 src/*.c -o build/mythonc-asan
./build/mythonc-asan tests/cases/simple.my
```

セグメンテーション違反の**原因の行**を教えてくれます。

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
    #0 0x... in tokenize src/lexer.c:87
```

**コンパイラ開発ではポインタのバグが必ず出ます。** ASan を使うと解決時間が 10 分の 1 になります。
Makefile に `make asan` ターゲットを用意します。

### UndefinedBehaviorSanitizer

```bash
clang -fsanitize=undefined -g -O0 src/*.c -o build/mythonc-ubsan
```

符号付き整数オーバーフロー、NULL ポインタ参照などを検出します。

---

## 8. その他の便利ツール

### FileCheck — IR のパターンテスト

```bash
FC=/usr/local/opt/llvm/bin/FileCheck
./build/mythonc -S test.my -o - | $FC test.my
```

テストファイルにチェック行を書いておく方式です。

```python
# CHECK: define i64 @add
# CHECK: add i64
def add(a: int, b: int) -> int:
    return a + b
```

**この教材では自作のシェルスクリプトでテストします**が、
IR の細部を検証したくなったら FileCheck が便利です。

### 差分の見やすい表示

```bash
diff -u expected.ll actual.ll
git diff --no-index expected.ll actual.ll     # 色が付いて読みやすい
```

第20章のブートストラップ検証で活躍します。

---

## 9. インストール記録

この環境で実際に行ったこと。

```bash
# 確認：Apple clang, cmake, make, Xcode CLT は既に存在した
clang --version         # Apple clang 21.0.0
cmake --version         # /usr/local/bin/cmake
xcode-select -p         # /Applications/Xcode.app/Contents/Developer

# LLVM がなかったので追加
brew install llvm       # → 22.1.8 が /usr/local/opt/llvm に入った
```

### 他の環境で再現する場合

**macOS（Homebrew）**

```bash
xcode-select --install
brew install llvm
```

**Ubuntu / Debian**

```bash
sudo apt update
sudo apt install -y build-essential clang llvm lld
# llvm-config が llvm-config-18 のような名前になることがある
```

**⚠️ Linux の場合の差異**
- target triple が `x86_64-pc-linux-gnu` などになる（Makefile が自動取得するので問題なし）
- `sed -i ''` が `sed -i` になる（macOS と BSD sed の差異）
- `opt` / `lli` が PATH に直接入っている場合がある

Makefile は `brew --prefix llvm` が使えない環境でもフォールバックするように書きます。

```makefile
LLVM_BIN := $(shell brew --prefix llvm 2>/dev/null)/bin
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(dir $(shell which opt 2>/dev/null))
endif
```

---

## 10. トラブルシューティング

| 症状 | 原因 | 対処 |
|---|---|---|
| `warning: overriding the module target triple` | IR の triple が clang のものと違う | 第1節の正しい取得方法を使う |
| `opt: Unknown command line argument '-mem2reg'` | 古い書き方 | `-passes=mem2reg` に変える |
| `error: expected type` で `i8*` の行 | opaque pointer 非対応の古い IR | `i8*` → `ptr` に変える |
| `error: instruction expected to be numbered '%N'` | 数値名を自分で使った | 一時値に `%t0` のような英字名を付ける |
| `error: expected instruction opcode`（ラベル行で） | 直前のブロックに終端命令がない | `br label %次` を追加 |
| `Undefined symbols for architecture` | ランタイムをリンクしていない | `clang foo.ll runtime.o -o foo` |
| `llvm-config not found` | Homebrew LLVM が PATH にない | フルパスで呼ぶ（第1節の方針） |
| セグフォするが場所が不明 | ポインタバグ | ASan を使う（第7節） |
