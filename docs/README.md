# Mython ドキュメント

**Mython** は、Python 風の文法をもつ **静的型付けのコンパイル言語** です。
このリポジトリは、その処理系（コンパイラ）を **C言語 + LLVM** で 1 から自作し、
最終的に **セルフホスト**（Mython で書いたコンパイラで Mython をコンパイルできる状態）
に到達するまでの記録と教材です。

このドキュメント群だけを読めば、**まったく同じものをゼロから再現できる**ことを目標にしています。

---

## 0. まず読むもの

| 順番 | ドキュメント | 内容 |
|---|---|---|
| 1 | [00-introduction.md](00-introduction.md) | なぜ作るのか / コンパイラとは何か / 全体像 |
| 2 | [roadmap.md](roadmap.md) | 全 20 章のロードマップと進捗表 |
| 3 | [reference/glossary.md](reference/glossary.md) | 用語集（わからない言葉が出たらここ） |

## 1. 設計ドキュメント（What を決める）

「何を作るのか」を先に固めます。実装より先にこちらを読んでください。

| ドキュメント | 内容 |
|---|---|
| [spec/language-spec.md](spec/language-spec.md) | **Mython 言語仕様書** — 構文・意味・組み込み関数 |
| [spec/grammar.md](spec/grammar.md) | **文法定義（EBNF）** — パーサを書くときの唯一の正解 |
| [spec/type-system.md](spec/type-system.md) | **型システム** — 型の一覧・型付け規則・型検査アルゴリズム |
| [design/architecture.md](design/architecture.md) | **コンパイラ全体アーキテクチャ** — パス構成とデータの流れ |
| [design/ir-conventions.md](design/ir-conventions.md) | **LLVM IR 生成規約** — どう IR に落とすかの取り決め |
| [design/memory-model.md](design/memory-model.md) | **メモリモデル** — 値の表現・確保・寿命 |
| [design/self-hosting.md](design/self-hosting.md) | **セルフホスト計画** — ブートストラップ戦略と検証方法 |

## 2. リファレンス（前提知識の補習）

| ドキュメント | 内容 |
|---|---|
| [reference/llvm-ir-primer.md](reference/llvm-ir-primer.md) | LLVM IR 入門。手で IR を書いて動かす |
| [reference/toolchain.md](reference/toolchain.md) | clang / llc / opt / lli の使い方チートシート |
| [reference/glossary.md](reference/glossary.md) | 用語集 |

## 3. 章（How を実装する）

各章は「**読む → 手を動かす → テストが通る**」で完結します。
章の終わりには必ず**動くコンパイラ**が残ります。

| 章 | タイトル | 状態 |
|---|---|---|
| [第1章](chapters/ch01-setup-and-minimal-compiler.md) | 環境構築と最小コンパイラ（整数を返す） | ✅ 完成 |
| [第2章](chapters/ch02-arithmetic-and-precedence.md) | 四則演算と演算子の優先順位 | ✅ 完成 |
| [第3章](chapters/ch03-diagnostics.md) | エラー報告と診断メッセージ | ✅ 完成 |
| [第4章](chapters/ch04-indentation.md) | インデント構文（NEWLINE / INDENT / DEDENT） | ✅ 完成 |
| [第5章](chapters/ch05-variables-and-typecheck.md) | 変数と型検査パスの導入 | ✅ 完成 |
| [第6章](chapters/ch06-bool-and-logical-ops.md) | bool・比較演算・論理演算 | ✅ 完成 |
| [第7章](chapters/ch07-control-flow.md) | 制御構文（if / elif / else / while） | ✅ 完成 |
| [第8章](chapters/ch08-functions.md) | 関数定義と呼び出し | ✅ 完成 |
| [第9章](chapters/ch09-strings-and-runtime.md) | 文字列と C ランタイム連携 | ✅ 完成 |
| [第10章](chapters/ch10-list.md) | list[T]（動的配列） | ✅ 完成 |
| [第11章](chapters/ch11-for-and-range.md) | for 文と range | ✅ 完成 |
| [第12章](chapters/ch12-class.md) | class（構造体とメソッド） | ✅ 完成 |
| [第13章](chapters/ch13-modules.md) | モジュールと import | ✅ 完成 |
| [第14章](chapters/ch14-stdlib.md) | 標準ライブラリ | ✅ 完成 |
| [第15章](chapters/ch15-nullable.md) | セルフホスト準備（T \| None と棚卸し） | ✅ 完成 |
| 第16章 | Mython で字句解析器を書く | ⬜ 未着手 |
| 第17章 | Mython で構文解析器を書く | ⬜ 未着手 |
| 第18章 | Mython で型検査器を書く | ⬜ 未着手 |
| 第19章 | Mython でコード生成器を書く | ⬜ 未着手 |
| 第20章 | ブートストラップと不動点検証 | ⬜ 未着手 |

## 4. 作業ログ

- [dev-log.md](dev-log.md) — 日付順の作業記録・判断の理由・つまずきメモ

---

## このドキュメントの読み方の約束

- **📖 解説** … 概念の説明。読むだけ。
- **✍️ 手を動かす** … ファイルを作る／コマンドを打つ。必ず実行する。
- **✅ 確認** … ここで動作確認する。通らなければ次に進まない。
- **🤔 なぜ？** … 設計判断の理由。飛ばしても動くが、読むと力がつく。
- **⚠️ 落とし穴** … よくある失敗。
