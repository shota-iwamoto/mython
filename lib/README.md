# lib/ — Mython 製の標準ライブラリ

**第14章で作りました。**

「Mython 自身で書けるものは Mython で書く」方針です。
C ランタイム（`runtime/`）に置くのは、C の機能が必要なものだけに限ります。

内容：

| ファイル | 中身 |
|---|---|
| `strings.my` | 文字列ヘルパ（`substr` / `find` / `split` / `join` / `strip` / `replace` …）。**C は 1 行も無い** |
| `io.my` | ファイル入出力。`extern` をここに閉じ込める |
| `sys.my` | `argv` と外部コマンド実行 |
| `dict.my` | 文字列キーの表（`str → int`。線形探索） |

`import strings` と書けば、コンパイラが `lib/` から自動で見つけます
（探索場所は「入口ファイルのディレクトリ」と `lib/` の 2 つ。
両方に同じ名前があればエラーです）。

なぜ f-string を言語機能にせずヘルパ関数で済ませるのかは
[../docs/design/self-hosting.md](../docs/design/self-hosting.md) 3.7 節を参照。
