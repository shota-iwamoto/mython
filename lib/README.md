# lib/ — Mython 製の標準ライブラリ

**第14章から使います。** 現在は空です。

「Mython 自身で書けるものは Mython で書く」方針です。
C ランタイム（`runtime/`）に置くのは、C の機能が必要なものだけに限ります。

予定している内容：

- `strutil.my` — 文字列ヘルパ（`cat3` / `cat5` など。f-string の代わり）
- `io.my` — ファイル読み書き
- `dict.my` — ハッシュマップ（シンボルテーブル用）

なぜ f-string を言語機能にせずヘルパ関数で済ませるのかは
[../docs/design/self-hosting.md](../docs/design/self-hosting.md) 3.6 節を参照。
