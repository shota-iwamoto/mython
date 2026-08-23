# Mython コンパイラ (stage0) のビルド
#
# 使い方:
#   make            コンパイラをビルド
#   make test       テストを全部実行（C 版のテスト + セルフホストの検証）
#   make selfhost-test  Mython 版と C 版の出力を比較（5 本）
#   make bootstrap      3 段ビルドと不動点の検証（第20章）
#   make bootstrap-test Mython 製コンパイラでテストを全部通す
#   make asan       AddressSanitizer 付きでビルド（メモリバグ調査用）
#   make clean      生成物を削除

CC      := clang
CFLAGS  := -std=c11 -g -O0 -Wall -Wextra -Wno-unused-parameter

# ── ターゲット triple の自動取得 ──────────────────────────────
# 生成する LLVM IR に書き込む triple。
#
# ⚠️ `clang -print-target-triple` を使ってはいけません。
#    macOS ではそれが返す値（x86_64-apple-darwin25.5.0）と、clang が実際に
#    IR に書く値（x86_64-apple-macosx26.0.0）が異なり、警告の原因になります。
#    「clang 自身に空の C ファイルの IR を吐かせて、そこから抜き出す」のが確実です。
HOST_TRIPLE := $(shell $(CC) -S -emit-llvm -x c /dev/null -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p')
CFLAGS  += -DMYTHON_TARGET_TRIPLE='"$(HOST_TRIPLE)"'

# ── Homebrew LLVM のツール（opt / lli / llvm-as）─────────────
# clang は Apple 版を使いますが、opt などは Homebrew 版が必要です。
# Linux など brew がない環境では PATH から探します。
LLVM_BIN := $(shell brew --prefix llvm 2>/dev/null)/bin
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(patsubst %/,%,$(dir $(shell which opt 2>/dev/null)))
endif
OPT      := $(LLVM_BIN)/opt
LLI      := $(LLVM_BIN)/lli
LLVM_AS  := $(LLVM_BIN)/llvm-as

# ── ランタイム（第9章）─────────────────────────────────────
# ユーザーのプログラムにリンクされる C のコード。
#
# ⚠️ コンパイラ本体（-O0 -g）とは目的が違うので -O2 でビルドします。
#    ランタイムは「ユーザーのプログラムの一部」として動くからです。
RUNTIME_SRC := runtime/runtime.c
RUNTIME_OBJ := build/runtime.o
RUNTIME_CFLAGS := -std=c11 -O2 -Wall -Wextra

# コンパイラにランタイムの場所を教える。
# ⚠️ stage0 だけの割り切り（ビルドツリー内で完結すればよい）。第20章で見直します。
CFLAGS  += -DMYTHON_RUNTIME_O='"$(abspath $(RUNTIME_OBJ))"'

# ── 標準ライブラリ（第14章）─────────────────────────────────
# import が探す 2 つ目の場所。Mython で書かれた lib/*.my があります。
CFLAGS  += -DMYTHON_LIB_DIR='"$(abspath lib)"'

SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
DEPS    := $(OBJS:.o=.d)
TARGET  := build/mythonc

.PHONY: all clean test test-one selfhost-test bootstrap bootstrap-test asan info

all: $(TARGET) $(RUNTIME_OBJ)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(RUNTIME_OBJ): $(RUNTIME_SRC)
	@mkdir -p build
	$(CC) $(RUNTIME_CFLAGS) -c $< -o $@

# -MMD -MP でヘッダの依存関係を自動生成する。
# これがないと、ヘッダを直したのに再ビルドされず不思議なバグに悩まされます。
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# ── テスト ──────────────────────────────────────────────────
test: $(TARGET) $(RUNTIME_OBJ)
	@tests/run_tests.sh
	@tests/selfhost.sh

# 1 ケースだけ実行: make test-one CASE=tests/cases/int_42.my
test-one: $(TARGET) $(RUNTIME_OBJ)
	@tests/run_tests.sh $(CASE)

# ── セルフホストの検証（第16〜19章）─────────────────────────
# Mython 製のコンパイラ（stage1）が C 版と同じものを出すか。
#   トークン列 → AST → 診断 → IR → 実行結果 の 5 本を比べます。
#   ★ テストケースをそのまま検証データに使います。
selfhost-test: $(TARGET) $(RUNTIME_OBJ)
	@tests/selfhost.sh

# ── ブートストラップ（第20章）───────────────────────────────
# stage1（C 版がビルド）→ stage2（stage1 がビルド）→ stage3（stage2 がビルド）
# stage2 == stage3 なら不動点に到達＝セルフホスト完成。
bootstrap: $(TARGET) $(RUNTIME_OBJ)
	@tests/bootstrap.sh

# Mython 製コンパイラ（stage2）でテストを全部通す。
#   ★ 「C 版と同じ出力を出す」より強い確認です。
bootstrap-test: bootstrap
	@MYTHONC=$(abspath build/boot/stage2) \
	 MYTHON_LIB_DIR=$(abspath lib) \
	 MYTHON_RUNTIME_O=$(abspath $(RUNTIME_OBJ)) \
	 MYTHON_TARGET_TRIPLE=$(HOST_TRIPLE) \
	 tests/run_tests.sh

# ── AddressSanitizer ビルド ─────────────────────────────────
# セグフォの原因が分からないときに使います。
#   make asan && ./build/mythonc-asan tests/cases/int_42.my
asan: $(RUNTIME_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(SRCS) -o build/mythonc-asan

# ── 情報表示 ────────────────────────────────────────────────
info:
	@echo "CC           = $(CC)"
	@echo "HOST_TRIPLE  = $(HOST_TRIPLE)"
	@echo "LLVM_BIN     = $(LLVM_BIN)"
	@echo "SRCS         = $(SRCS)"

clean:
	rm -rf build a.out a.out.ll tests/tmp
