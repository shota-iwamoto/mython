# runtime/ — C 製ランタイムライブラリ

**第9章から使います。** 現在は空です。

生成される LLVM IR を単純に保つため、制御フローを含む処理は
ここに C 関数として置き、IR 側は `call` 1 行にします
（[../docs/design/ir-conventions.md](../docs/design/ir-conventions.md) 規約 R10）。

予定している内容：

- `my_alloc` — ゼロ初期化つきメモリ確保（失敗時は即終了）
- `my_print_int` / `my_print_str` / `my_print_bool` — `print` の実体
- `my_str_concat` / `my_str_eq` / `my_str_len` / `my_str_sub` — 文字列操作
- `my_str_from_int` / `my_int_from_str` — 相互変換
- `my_list_new` / `my_list_push_*` / `my_list_get_*` — `list[T]`（第10章）
- `my_check_not_none` — None 参照の親切なエラー（第12章）

API の一覧は [../docs/design/memory-model.md](../docs/design/memory-model.md) 第4節にあります。
