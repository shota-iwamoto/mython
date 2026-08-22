# selfhost/ — Mython 製の Mython コンパイラ (stage1)

**第16章から書き始めました。** 現在はコード生成まで（IR まで出せます）。

`src/` の C 版と **1:1 で対応**させます。この対応を崩さないでください。
崩すと「C 版のどこを見れば正解がわかるか」が失われます。

| C 版 | Mython 版 | 章 |
|---|---|---|
| `src/lexer.h` | `selfhost/token.my` | 第16章 ✅ |
| `src/lexer.c` | `selfhost/lexer.my` | 第16章 ✅ |
| `src/parser.c` | `selfhost/parser.my` | 第17章 ✅ |
| `src/ast.c` | `selfhost/ast.my` | 第17章 ✅ |
| `src/sema.c` | `selfhost/sema.my` | 第18章 ✅ |
| `src/diag.c` | `selfhost/diag.my` | 第18章 ✅ |
| `src/types.c` | `selfhost/ast.my` に同居 | 第18章 ✅ |
| `src/module.c` | `selfhost/module.my` | 第18章 ✅ |
| `src/codegen.c` | `selfhost/codegen.my` | 第19章 ✅ |
| `src/main.c` | `selfhost/main.my` | 第20章 |

## 検証方法

各段階で C 版が「正解」を持っていることを利用します。

```bash
# 第16章：トークン列が一致するか（tests/selfhost.sh が全ファイルで自動比較）
make selfhost-test
#   → トークン列一致 338 件 / 字句エラーの位置一致 9 件
#
# 1 ファイルだけ見るなら
./build/mythonc --dump-tokens tests/cases/x.my > /tmp/c.txt
./build/stage1-lexer          tests/cases/x.my > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 第17章：AST（S 式）が一致するか
./build/mythonc --dump-ast tests/cases/x.my > /tmp/c.txt
./build/stage1-ast         tests/cases/x.my > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 第18章：型検査の診断が一致するか（メッセージ全文）
./build/mythonc --check tests/cases/x.my 2> /tmp/c.txt
./build/stage1-check    tests/cases/x.my 2> /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 第19章：IR が一致するか／その IR が動くか
./build/mythonc -S       tests/cases/x.my > /tmp/c.ll
./build/stage1-codegen   tests/cases/x.my > /tmp/m.ll
diff /tmp/c.ll /tmp/m.ll
clang /tmp/m.ll build/runtime.o -o /tmp/x && /tmp/x

# 第20章：不動点の検証
make bootstrap    # stage2 == stage3 なら成功
```

**移植は機械的に行ってください。ここで独創性を発揮しないこと。**
アルゴリズムと評価順序を C 版と揃えることで、出力の完全一致を目指せます。

詳細は [../docs/design/self-hosting.md](../docs/design/self-hosting.md)。
