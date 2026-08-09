# 開発ログ

> 日付順の作業記録です。**判断の理由**と**つまずいたこと**を残します。
> 「なぜこうなっているのか」を後から辿れるようにするのが目的です。
>
> 書式：
> ```
> ## YYYY-MM-DD — 見出し
> ### やったこと
> ### 判断したこと（と理由）
> ### つまずいたこと
> ### 次にやること
> ```

---

## 2026-08-09 — プロジェクト開始、設計と第1章

### やったこと

**環境構築**

- 既にあったもの：Apple clang 21.0.0、cmake、make、Xcode CLT
- 足りなかったもの：LLVM の開発ツール（`llvm-config` が無かった）
  → `brew install llvm` で **LLVM 22.1.8** を `/usr/local/opt/llvm` に導入

**設計ドキュメントを先に全部書いた**

```
docs/00-introduction.md            全体像・設計方針
docs/roadmap.md                    全20章のロードマップ
docs/spec/language-spec.md         言語仕様 v1
docs/spec/grammar.md               文法定義 (EBNF)
docs/spec/type-system.md           型システムと型付け規則
docs/design/architecture.md        コンパイラ構成
docs/design/ir-conventions.md      LLVM IR 生成規約（R1〜R12）
docs/design/memory-model.md        メモリモデル
docs/design/self-hosting.md        セルフホスト計画
docs/reference/llvm-ir-primer.md   LLVM IR 入門（手を動かす）
docs/reference/toolchain.md        ツールチェーン早見表
docs/reference/glossary.md         用語集
```

**第1章の実装**

- `src/util.{h,c}` — `xmalloc` / `StrBuf` / `read_file` / `error_at_pos`
- `src/lexer.{h,c}` — 整数・コメント・空白・タブ禁止、`--dump-tokens`
- `src/ast.{h,c}` — `Node`（全部入り構造体）、S 式ダンプ
- `src/parser.{h,c}` — 再帰下降の骨格（`peek`/`advance`/`expect`）
- `src/codegen.{h,c}` — 4 バッファ方式、`gen_expr`、main ラッパ
- `src/main.c` — CLI、パス起動、clang 呼び出し
- `Makefile` — triple 自動取得、ヘッダ依存自動生成、ASan ターゲット
- `tests/run_tests.sh` + テストケース 11 件

**結果**：警告 0 件でビルド、テスト 11 件全パス、ASan/UBSan もクリーン。

### 判断したこと（と理由）

| # | 判断 | 理由 |
|---|---|---|
| 1 | **LLVM IR テキストを出力する**（C API を使わない） | C 側のコードが `sb_printf` だけになり読みやすい。生成物を目で読んで学べる。**決定的な理由：セルフホスト時に Mython 側に必要なのが「文字列を組み立てて出力する」機能だけになる。** C API 方式だと FFI 機構が言語に必要になり難易度が跳ね上がる |
| 2 | **全変数を `alloca` に置き、`phi` を生成しない** | SSA 構築（支配辺境の計算）はコンパイラ理論の難所。LLVM の `mem2reg` が最適な形に直してくれるので、素朴に書いて任せる。実測で確認済み（下記） |
| 3 | **型注釈を必須にし、型推論を入れない** | 型検査器が「調べるだけ」になり「推論する」必要がなくなる。実装が劇的に単純になる |
| 4 | **`str` は NUL 終端バイト列**（`{ptr,len}` ではない） | C 関数にそのまま渡せる。ランタイムが極小で済む。代償は `len()` が O(n) と、文字列中に NUL を置けないこと。コンパイラ用途では問題にならない。第9章で再検討する |
| 5 | **`free()` を呼ばない** | コンパイラは短命プロセス。解放タイミングは実質「プロセス終了時」しかない。`free` を書くと二重解放バグの余地が生まれ、得るものがない |
| 6 | **`int / int` をエラーにする**（`//` を使わせる） | 暗黙の型変換を入れないため。型昇格ルールは型検査器を複雑にする |
| 7 | **`//` は `sdiv`（切り捨て）で Python の floor 除算と非互換にする** | `sdiv` 1 命令で済む。`-7 // 2` が Python では `-4`、Mython では `-3` になる。既知の非互換として仕様書に明記 |
| 8 | **truthiness を採用しない**（`if xs:` を禁止） | `bool` 以外を条件式に許すと型検査が緩む。`if len(xs) > 0:` を書かせる |
| 9 | **タブインデントを字句エラーにする** | タブ幅の解釈で意味が変わるコードは言語の欠陥。実装も単純になる |
| 10 | **トークンを配列で持つ**（リンクリストではない） | 任意の先読みが O(1)（第5章で `x : int` の判別に 2 トークン必要）。Mython 移植時に `list[Token]` として自然 |
| 11 | **AST は「全部入り」の 1 構造体** | C で共用体を安全に扱うと冗長。chibicc/tcc など実績ある方式。1 ノード 150 バイト程度なので無駄も許容範囲 |
| 12 | **`main` はラッパ方式**（`@mython_main` + `@main`） | `main` だけ特別扱いする分岐をコード生成器に入れたくない。`-O2` でインライン展開され消えるのでコストゼロ（実測で確認） |
| 13 | **`--dump-tokens` / `--dump-ast` を第1章で作る** | 開発速度を決める中核ツール。さらに**第16/17章でセルフホスト版の検証に使う「正解出力」になる** |
| 14 | **エラーは最初の 1 件で `exit(1)`** | エラー回復（複数報告）は独立した難問。v1 のスコープ外 |
| 15 | **CMake を使わず Makefile 1 枚** | 依存が増え、生成物が読めなくなる。10 ファイル程度に CMake は過剰 |
| 16 | **設計ドキュメントを実装より先に全部書く** | 「何を作るか」が曖昧なまま実装すると、後で全部書き直しになる |

### つまずいたこと

**① `clang -print-target-triple` を使うと警告が消えない** ⚠️

IR に `target triple` を書かないと clang が警告を出すので、
ビルド時に triple を取得しようとした。直感的に `-print-target-triple` を使ったが、**警告が消えなかった**。

```bash
$ clang -print-target-triple
x86_64-apple-darwin25.5.0          # ← これを .ll に書くと…

$ clang t.ll -o t
warning: overriding the module target triple with x86_64-apple-macosx26.0.0
```

macOS の clang は `-print-target-triple`（`darwin<カーネル版>`）と
IR に書く triple（`macosx<OS版>`）で**表記が違う**。

**解決**：clang 自身に空の C ファイルの IR を吐かせて抜き出す。

```makefile
HOST_TRIPLE := $(shell $(CC) -S -emit-llvm -x c /dev/null -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p')
```

Linux でも同じ方法が使える。

**② `mem2reg` は定数畳み込みをしない**

ドキュメントに「`mem2reg` を掛けると `ret i32 22` になる」と書いていたが、実測すると違った。

```llvm
; opt -passes=mem2reg の実際の出力
define i32 @main() {
entry:
  %t1 = add i64 20, 2      ; ← 定数計算は残る
  %t3 = trunc i64 %t1 to i32
  ret i32 %t3
}
```

`mem2reg` は「メモリ経由をレジスタ経由に変える」だけ。
定数畳み込みは別のパスの仕事なので、`-O2` が必要だった。

**正しい理解**：`mem2reg` が土台を作り、その上で他のパスが働く分業。
`docs/reference/llvm-ir-primer.md` を実測結果に合わせて修正した。

**③ 配列長リテラルのエラーメッセージが直感と逆**

```llvm
@.str.0 = private unnamed_addr constant [14 x i8] c"hello, mython\0A\00"
```

```
error: constant expression type mismatch: got type '[15 x i8]' but expected '[14 x i8]'
```

`got` が**正しい長さ**（15）、`expected` が**自分が書いた値**（14）。
「got = 自分が書いたもの」と読むと混乱する。
→ `got` の数字をそのまま書き写せば直る、と用語集ドキュメントに注記した。

**④ `-Wall -Wextra` で未使用関数の警告が出た**

第5章で使う予定の `peek_at()` を先に書いたら
`warning: unused function 'peek_at'` が出た。

**判断**：警告 0 件の状態を保つことを優先し、コメントアウトして
「第5章で追加する」と注記した。
**理由**：警告を放置する習慣がつくと、本物のバグを見逃す。

### 実測して確認したこと

「たぶんこうなる」で済ませず、実際にコマンドを叩いて確認した項目。

| 確認したこと | 結果 |
|---|---|
| Apple clang は `.ll` を直接コンパイルできるか | ✅ できる（brew LLVM への依存は必須ではない） |
| `mem2reg` は alloca/load/store を消すか | ✅ 消す（`add i64 %n, 1` + `ret` だけになった） |
| `mem2reg` は定数畳み込みするか | ❌ しない（`-O2` が必要） |
| `-O2` で main ラッパのコストは消えるか | ✅ 消える（`ret i32 42` になった） |
| `lli` で生成 IR がそのまま動くか | ✅ 動く（exit=42） |
| IR 入門の全サンプル（10 個）が動くか | ✅ 全部動作確認済み |
| ASan/UBSan で全テストがクリーンか | ✅ クリーン |

### 次にやること

**第2章 四則演算と演算子の優先順位**

達成目標：`1 + 2 * (3 - 1) // 2 + 3` が計算できる

| ファイル | 作業 |
|---|---|
| `lexer.c` | `TK_PUNCT` 追加。`+ - * / // % ** ( )` を読む（**2 文字記号を先に判定する**のを忘れない） |
| `ast.h/c` | `ND_BINOP` / `ND_UNARY`、`lhs`/`rhs`/`op` フィールド |
| `parser.c` | 優先順位の階層（`add_expr`→`mul_expr`→`unary`→`power`→`primary`） |
| `codegen.c` | `gen_expr` に `ND_BINOP` の case。`add`/`sub`/`mul`/`sdiv`/`srem` |

注意点のメモ：
- `//` と `/` は**長い記号から先に**マッチさせる（`//` を `/` 2 個に読まないため）
- `-x` に LLVM の `neg` 命令はない → `sub i64 0, %x`
- `int` は符号付きなので `sdiv`/`srem`/`ashr`（`udiv`/`urem`/`lshr` ではない）
- `**` は**右結合**なので再帰で書く（`{ }` ループではない）
- 0 除算のテストケースを追加すること（実行時エラーの設計は第9章だが、意識しておく）

---

<!--
## テンプレート（コピーして使う）

## YYYY-MM-DD — 第N章 タイトル

### やったこと

### 判断したこと（と理由）

### つまずいたこと

### 実測して確認したこと

### 次にやること
-->
