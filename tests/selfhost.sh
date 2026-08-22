#!/bin/bash
# Mython 版と C 版のトークン列を比較する（第16章）
#
# ★ テストケースをそのまま字句解析器の検証データに使います。
#   300 個以上のファイルが、追加のテストを 1 行も書かずに検証に使えます。
#
# 使い方:
#   tests/selfhost.sh                     全ケース
#   tests/selfhost.sh tests/cases/x.my    1 ケースだけ

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MYTHONC="$ROOT/build/mythonc"
STAGE1="$ROOT/build/stage1-lexer"
STAGE1_AST="$ROOT/build/stage1-ast"
STAGE1_CHECK="$ROOT/build/stage1-check"
STAGE1_CODEGEN="$ROOT/build/stage1-codegen"
TMP="$ROOT/tests/tmp"

mkdir -p "$TMP"

# ★ 標準ライブラリの場所を両者に同じ形で渡す（C 版も stage1 も
#   MYTHON_LIB_DIR を先に見ます。ch18 18.6 節）
export MYTHON_LIB_DIR="$ROOT/lib"

if [ ! -x "$MYTHONC" ]; then
    echo "コンパイラが見つかりません: $MYTHONC（先に make）"
    exit 1
fi

# stage1 の字句解析器と構文解析器をビルドする（Mython 製）
if ! "$MYTHONC" "$ROOT/selfhost/dump_tokens.my" -o "$STAGE1" > "$TMP/stage1-build.log" 2>&1; then
    echo "stage1-lexer のビルドに失敗しました:"
    cat "$TMP/stage1-build.log"
    exit 1
fi
if ! "$MYTHONC" "$ROOT/selfhost/dump_ast.my" -o "$STAGE1_AST" > "$TMP/stage1-build.log" 2>&1; then
    echo "stage1-ast のビルドに失敗しました:"
    cat "$TMP/stage1-build.log"
    exit 1
fi
if ! "$MYTHONC" "$ROOT/selfhost/check.my" -o "$STAGE1_CHECK" > "$TMP/stage1-build.log" 2>&1; then
    echo "stage1-check のビルドに失敗しました:"
    cat "$TMP/stage1-build.log"
    exit 1
fi
if ! "$MYTHONC" "$ROOT/selfhost/emit_ir.my" -o "$STAGE1_CODEGEN" > "$TMP/stage1-build.log" 2>&1; then
    echo "stage1-codegen のビルドに失敗しました:"
    cat "$TMP/stage1-build.log"
    exit 1
fi

# ★ target triple もビルド時に埋め込めないので環境変数で渡す（第19章）
export MYTHON_TARGET_TRIPLE="$("$MYTHONC" -S "$ROOT/tests/cases/int_42.my" \
    | sed -n 's/^target triple = "\(.*\)"$/\1/p')"

if [ $# -gt 0 ]; then
    FILES=("$@")
else
    FILES=()
    while IFS= read -r line; do FILES+=("$line"); done \
        < <(ls "$ROOT"/tests/cases/*.my "$ROOT"/tests/mods/*/*.my \
              "$ROOT"/selfhost/*.my "$ROOT"/lib/*.my "$ROOT"/examples/*.my 2>/dev/null | sort)
fi

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

pass=0; errpass=0; astpass=0; asterrpass=0; chkpass=0; chkerrpass=0
irpass=0; runpass=0; fail=0
failed_names=()

for f in "${FILES[@]}"; do
    name="${f#$ROOT/}"

    # ⚠️ わざと壊してあるケースは C 版の字句解析が失敗する。
    #   トークン列は比べられないが、**エラーの位置**は比べられる。
    if ! "$MYTHONC" --dump-tokens "$f" > "$TMP/c.tokens" 2>"$TMP/c.err"; then
        # ★ 第18章：diag を移植したので、**メッセージ全体**を比べます
        "$STAGE1" "$f" > /dev/null 2>"$TMP/m.err"

        if diff -q "$TMP/c.err" "$TMP/m.err" > /dev/null; then
            errpass=$((errpass + 1))
        else
            fail=$((fail + 1)); failed_names+=("$name")
            printf "  %sFAIL%s  %s（字句エラーの内容が違う）\n" "$C_NG" "$C_END" "$name"
            diff "$TMP/c.err" "$TMP/m.err" | head -12 | sed 's/^/          /'
        fi
        continue
    fi

    if ! "$STAGE1" "$f" > "$TMP/m.tokens" 2>"$TMP/m.err"; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（stage1 が失敗）\n" "$C_NG" "$C_END" "$name"
        sed 's/^/          /' "$TMP/m.err" | head -3
        continue
    fi

    if diff -q "$TMP/c.tokens" "$TMP/m.tokens" > /dev/null; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（トークン列）\n" "$C_NG" "$C_END" "$name"
        diff "$TMP/c.tokens" "$TMP/m.tokens" | head -10 | sed 's/^/          /'
        continue
    fi

    # ── ② AST（第17章）──
    #
    # ⚠️ 構文エラーのファイルは S 式を比べられない。位置だけ比べる。
    if ! "$MYTHONC" --dump-ast "$f" > "$TMP/c.ast" 2>"$TMP/c.err"; then
        # ★ 第18章：構文エラーもメッセージ全体で比べます
        "$STAGE1_AST" "$f" > /dev/null 2>"$TMP/m.err"

        if diff -q "$TMP/c.err" "$TMP/m.err" > /dev/null; then
            asterrpass=$((asterrpass + 1))
        else
            fail=$((fail + 1)); failed_names+=("$name")
            printf "  %sFAIL%s  %s（構文エラーの内容が違う）\n" "$C_NG" "$C_END" "$name"
            diff "$TMP/c.err" "$TMP/m.err" | head -14 | sed 's/^/          /'
        fi
        continue
    fi

    if ! "$STAGE1_AST" "$f" > "$TMP/m.ast" 2>"$TMP/m.err"; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（stage1-ast が失敗）\n" "$C_NG" "$C_END" "$name"
        sed 's/^/          /' "$TMP/m.err" | head -3
        continue
    fi

    if diff -q "$TMP/c.ast" "$TMP/m.ast" > /dev/null; then
        astpass=$((astpass + 1))
    else
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（AST）\n" "$C_NG" "$C_END" "$name"
        diff "$TMP/c.ast" "$TMP/m.ast" | head -12 | sed 's/^/          /'
        continue
    fi

    # ── ③ 型検査（第18章）──
    #
    # ★ エラーが「出る / 出ない」だけでなく、メッセージ全体を比べます。
    #   ⚠️ import を含むファイルは入口として型検査できないもの（main が無い
    #     ライブラリなど）があるので、C 版がエラーにするかどうかで揃えます。
    "$MYTHONC" --check "$f" > /dev/null 2>"$TMP/c.err"; crc=$?
    "$STAGE1_CHECK" "$f" > /dev/null 2>"$TMP/m.err"; mrc=$?

    if [ "$crc" -ne "$mrc" ]; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（型検査の成否が違う: C=%d stage1=%d）\n" \
               "$C_NG" "$C_END" "$name" "$crc" "$mrc"
        head -6 "$TMP/c.err" | sed 's/^/          C : /'
        head -6 "$TMP/m.err" | sed 's/^/          M : /'
        continue
    fi

    if ! diff -q "$TMP/c.err" "$TMP/m.err" > /dev/null; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（型エラーの内容が違う）\n" "$C_NG" "$C_END" "$name"
        diff "$TMP/c.err" "$TMP/m.err" | head -14 | sed 's/^/          /'
        continue
    fi

    if [ "$crc" -ne 0 ]; then
        chkerrpass=$((chkerrpass + 1))
        continue
    fi
    chkpass=$((chkpass + 1))

    # ── ④ IR（第19章）──
    "$MYTHONC" -S "$f" > "$TMP/c.ll" 2>/dev/null
    "$STAGE1_CODEGEN" "$f" > "$TMP/m.ll" 2>"$TMP/m.err"

    if ! diff -q "$TMP/c.ll" "$TMP/m.ll" > /dev/null; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（IR）\n" "$C_NG" "$C_END" "$name"
        diff "$TMP/c.ll" "$TMP/m.ll" | head -14 | sed 's/^/          /'
        continue
    fi
    irpass=$((irpass + 1))

    # ── ⑤ stage1 の IR が本当に動くか（第19章）──
    #
    # ★ 単一モジュールのケースだけ。複数モジュールの -S 出力は
    #   区切りを入れて並べたものなので、そのままではリンクできません。
    if grep -q '^; ── module:' "$TMP/c.ll"; then
        continue
    fi
    if ! clang "$TMP/m.ll" "$ROOT/build/runtime.o" -o "$TMP/m.bin" 2>/dev/null; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（stage1 の IR がリンクできない）\n" \
               "$C_NG" "$C_END" "$name"
        continue
    fi
    m_out="$("$TMP/m.bin" 2>/dev/null)"; m_rc=$?

    "$MYTHONC" "$f" -o "$TMP/c.bin" 2>/dev/null
    c_out="$("$TMP/c.bin" 2>/dev/null)"; c_rc=$?

    if [ "$m_out" != "$c_out" ] || [ "$m_rc" -ne "$c_rc" ]; then
        fail=$((fail + 1)); failed_names+=("$name")
        printf "  %sFAIL%s  %s（stage1 の IR の実行結果が違う）\n" \
               "$C_NG" "$C_END" "$name"
        printf "          C : rc=%s %s\n          M : rc=%s %s\n" \
               "$c_rc" "$c_out" "$m_rc" "$m_out"
        continue
    fi
    runpass=$((runpass + 1))
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    printf "%sトークン列一致 %d 件 / 字句エラーの内容一致 %d 件%s\n" \
           "$C_OK" "$pass" "$errpass" "$C_END"
    printf "%sAST 一致 %d 件 / 構文エラーの内容一致 %d 件%s\n" \
           "$C_OK" "$astpass" "$asterrpass" "$C_END"
    printf "%s型検査 一致 %d 件 / 型エラーの内容一致 %d 件%s\n" \
           "$C_OK" "$chkpass" "$chkerrpass" "$C_END"
    printf "%sIR 一致 %d 件 / stage1 の IR で実行して一致 %d 件%s\n" \
           "$C_OK" "$irpass" "$runpass" "$C_END"
    exit 0
else
    printf "%s%d 件一致 / %d 件不一致%s\n" "$C_NG" "$pass" "$fail" "$C_END"
    for n in "${failed_names[@]}"; do echo "  - $n"; done
    exit 1
fi
