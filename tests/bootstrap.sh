#!/bin/bash
# ブートストラップと不動点の検証（第20章）
#
#   stage1 … C 版（build/mythonc）がビルドした Mython 製コンパイラ
#   stage2 … stage1 が自分自身のソースをビルドしたもの
#   stage3 … stage2 が自分自身のソースをビルドしたもの
#
# ★ stage2 と stage3 が一致すれば「不動点」に到達したことになります。
#   stage1 は C 版が作ったので中身が違ってもよいのですが、
#   stage2 以降は「Mython 製コンパイラが作った Mython 製コンパイラ」なので、
#   出力が変わる理由がありません。変わるなら、どこかに
#   「誰がコンパイルしたかによって変わる何か」が残っています。

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MYTHONC="$ROOT/build/mythonc"
BOOT="$ROOT/build/boot"

# ★ C 版がビルド時に埋め込む値を、stage1 には環境変数で渡します
#   （両者とも環境変数を先に見る規則。ch18 18.6 / ch20 20.2）
export MYTHON_LIB_DIR="$ROOT/lib"
export MYTHON_RUNTIME_O="$ROOT/build/runtime.o"
export MYTHON_TARGET_TRIPLE="$("$MYTHONC" -S "$ROOT/tests/cases/int_42.my" \
    | sed -n 's/^target triple = "\(.*\)"$/\1/p')"

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

rm -rf "$BOOT"
mkdir -p "$BOOT"

step() { printf "%s──%s %s\n" "$C_DIM" "$C_END" "$1"; }

# ── stage1：C 版が Mython 製コンパイラをビルドする ──
step "stage1 = C 版がビルド"
if ! "$MYTHONC" "$ROOT/selfhost/main.my" -o "$BOOT/stage1"; then
    echo "stage1 のビルドに失敗しました"
    exit 1
fi

# ── stage2：stage1 が自分自身をビルドする ──
step "stage2 = stage1 がビルド（Mython 製コンパイラが自分自身を）"
if ! "$BOOT/stage1" "$ROOT/selfhost/main.my" -o "$BOOT/stage2" --keep-ll; then
    echo "stage2 のビルドに失敗しました"
    exit 1
fi

# ── stage3：stage2 が自分自身をビルドする ──
step "stage3 = stage2 がビルド"
if ! "$BOOT/stage2" "$ROOT/selfhost/main.my" -o "$BOOT/stage3" --keep-ll; then
    echo "stage3 のビルドに失敗しました"
    exit 1
fi

echo

# ── ① 生成された IR が一致するか（これが本体）──
n=0; bad=0
for f in "$BOOT"/stage2.*.ll; do
    g="${f/stage2./stage3.}"
    n=$((n + 1))
    cmp -s "$f" "$g" || { bad=$((bad + 1)); echo "  差分: $(basename "$f")"; }
done

if [ "$bad" -ne 0 ]; then
    printf "%s✗ stage2 と stage3 の IR が違います（%d / %d 本）%s\n" \
           "$C_NG" "$bad" "$n" "$C_END"
    exit 1
fi
printf "%s★ stage2 と stage3 が出す IR が完全一致（%d 本）%s\n" "$C_OK" "$n" "$C_END"

# ── ② 実行ファイルも一致するか ──
#
# ⚠️ macOS のリンカは実行ファイルに UUID（毎回変わる 16 バイト）を埋めます。
#    そのままだと必ず差が出るので、比較のときだけ -Wl,-no_uuid で作り直します。
#    ★ 「違いが出た」ではなく「どこが違うのか」を調べてから判断すること。
if ! clang -O0 "$BOOT"/stage2.*.ll "$MYTHON_RUNTIME_O" -Wl,-no_uuid \
        -o "$BOOT/cmp2" 2>/dev/null; then
    echo "比較用のリンクに失敗しました"
    exit 1
fi
clang -O0 "$BOOT"/stage3.*.ll "$MYTHON_RUNTIME_O" -Wl,-no_uuid -o "$BOOT/cmp3" 2>/dev/null

if cmp -s "$BOOT/cmp2" "$BOOT/cmp3"; then
    printf "%s★ 実行ファイルもバイト単位で一致（UUID を除く）%s\n" "$C_OK" "$C_END"
else
    printf "%s✗ 実行ファイルが違います%s\n" "$C_NG" "$C_END"
    cmp "$BOOT/cmp2" "$BOOT/cmp3" | head -3
    exit 1
fi

echo
printf "%s★ 不動点に到達しました（stage2 == stage3）%s\n" "$C_OK" "$C_END"
printf "%s   Mython コンパイラは、自分自身をコンパイルできます。%s\n" "$C_DIM" "$C_END"
