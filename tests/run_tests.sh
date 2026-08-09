#!/bin/bash
# Mython テストランナー
#
# テストケースは tests/cases/*.my です。期待値はファイル先頭のコメントに書きます。
#
#   # EXIT: 42        → コンパイル・実行して終了コードが 42 であること
#   # OUTPUT: hello   → 標準出力が "hello" であること（複数行は行ごとに書く）
#   # ERROR: メッセージ → コンパイルが失敗し、stderr にその文字列を含むこと
#
# 使い方:
#   tests/run_tests.sh                    全ケース
#   tests/run_tests.sh tests/cases/x.my   1 ケースだけ

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MYTHONC="$ROOT/build/mythonc"
TMP="$ROOT/tests/tmp"

if [ ! -x "$MYTHONC" ]; then
    echo "コンパイラが見つかりません: $MYTHONC"
    echo "先に 'make' を実行してください。"
    exit 1
fi

mkdir -p "$TMP"

if [ $# -gt 0 ]; then
    CASES=("$@")
else
    # ソートして順序を安定させる（テスト結果が実行ごとに変わらないように）
    CASES=()
    while IFS= read -r line; do CASES+=("$line"); done < <(ls "$ROOT"/tests/cases/*.my | sort)
fi

pass=0
fail=0
failed_names=()

# 色（端末でないときは付けない）
if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

report_fail() {
    local name="$1" reason="$2"
    printf "  %sFAIL%s  %s\n" "$C_NG" "$C_END" "$name"
    # 理由をインデントして表示する
    printf "%s" "$reason" | sed 's/^/          /'
    echo
    fail=$((fail + 1))
    failed_names+=("$name")
}

for case_file in "${CASES[@]}"; do
    name="$(basename "$case_file")"
    base="${name%.my}"
    exe="$TMP/$base"

    # ── 期待値をヘッダコメントから読み取る ──
    want_exit="$(sed -n 's/^# *EXIT: *//p'   "$case_file" | head -1)"
    # ERROR は複数行書ける。すべてが stderr に含まれることを要求する。
    # 診断メッセージの note: / ヒント: 行まで検証できるようにするため。
    want_error="$(sed -n 's/^# *ERROR: *//p' "$case_file")"
    # OUTPUT は複数行を許す
    want_output="$(sed -n 's/^# *OUTPUT: *//p' "$case_file")"

    if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ]; then
        report_fail "$name" "期待値のコメント（# EXIT: / # OUTPUT: / # ERROR:）がありません"
        continue
    fi

    # ── コンパイル ──
    compile_err="$("$MYTHONC" "$case_file" -o "$exe" 2>&1 >/dev/null)"
    compile_rc=$?

    # ── ERROR: コンパイルが失敗し、指定文字列を含むことを期待 ──
    if [ -n "$want_error" ]; then
        if [ "$compile_rc" -eq 0 ]; then
            report_fail "$name" "コンパイルが成功してしまいました（失敗を期待）
期待するエラー: $want_error"
            continue
        fi

        # 期待する文字列を 1 行ずつ確認する
        missing=""
        nchecks=0
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            nchecks=$((nchecks + 1))
            printf '%s' "$compile_err" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_WANT
$want_error
EOF_WANT

        if [ -n "$missing" ]; then
            report_fail "$name" "エラー出力に含まれていない期待文字列があります:$missing
実際の出力:
$compile_err"
        else
            printf "  %sok%s    %s %s(error x%d)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$nchecks" "$C_END"
            pass=$((pass + 1))
        fi
        continue
    fi

    # ── ここから先はコンパイル成功を期待 ──
    if [ "$compile_rc" -ne 0 ]; then
        report_fail "$name" "コンパイルに失敗しました
$compile_err"
        continue
    fi

    # ── 実行 ──
    actual_output="$("$exe" 2>/dev/null)"
    actual_exit=$?

    ok=1
    reason=""

    if [ -n "$want_exit" ] && [ "$actual_exit" -ne "$want_exit" ]; then
        ok=0
        reason="終了コードが違います: 期待 $want_exit, 実際 $actual_exit"
    fi

    if [ -n "$want_output" ] && [ "$actual_output" != "$want_output" ]; then
        ok=0
        reason="$reason
標準出力が違います:
--- 期待 ---
$want_output
--- 実際 ---
$actual_output"
    fi

    if [ "$ok" -eq 1 ]; then
        detail="exit=$actual_exit"
        printf "  %sok%s    %s %s(%s)%s\n" "$C_OK" "$C_END" "$name" "$C_DIM" "$detail" "$C_END"
        pass=$((pass + 1))
    else
        report_fail "$name" "$reason"
    fi
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    printf "%s全 %d 件パス%s\n" "$C_OK" "$pass" "$C_END"
    exit 0
else
    printf "%s%d 件パス / %d 件失敗%s\n" "$C_NG" "$pass" "$fail" "$C_END"
    for n in "${failed_names[@]}"; do echo "  - $n"; done
    exit 1
fi
