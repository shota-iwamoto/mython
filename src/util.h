// util.h — 全モジュールが使う基礎部品
//
// このファイルは他の Mython モジュールに依存しません（依存グラフの最下層）。
// ここに置くもの：メモリ確保、文字列バッファ、ファイル入出力、エラー終了。
#ifndef MYTHON_UTIL_H
#define MYTHON_UTIL_H

#include <stdarg.h>
#include <stddef.h>

// ── メモリ確保 ──────────────────────────────────────────────
// free() は一切呼びません（docs/design/memory-model.md 第8節）。
// calloc を使うので、確保された領域は必ず 0 / NULL で初期化されています。
void *xmalloc(size_t size);

// s の先頭 n バイトを複製し、NUL 終端した新しい文字列を返す
char *xstrndup(const char *s, size_t n);

// ── 伸長する文字列バッファ ────────────────────────────────────
// LLVM IR を組み立てるための道具。printf と同じ書式で追記できます。
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_printf(StrBuf *sb, const char *fmt, ...);
// バッファの中身を NUL 終端された文字列として返す（内部バッファをそのまま返す）
char *sb_str(StrBuf *sb);

// ── ファイル入出力 ──────────────────────────────────────────
// ファイル全体を読み込む。\r\n は \n に正規化し、末尾に改行を保証する。
char *read_file(const char *path);
void write_file(const char *path, const char *text);

// ── エラー終了 ──────────────────────────────────────────────
// 位置情報のないエラー（コマンドライン引数の誤りなど）
_Noreturn void error(const char *fmt, ...);

// 位置情報付きのエラー。ソース行を抜粋して下線を引きます。
// Token を引数に取るラッパ error_at() は lexer.h にあります。
//   line_start : その行の先頭を指すポインタ
//   line, col  : 1 起算の行・桁
//   len        : 下線を引く長さ
_Noreturn void error_at_pos(const char *file, const char *line_start,
                            int line, int col, int len, const char *fmt, ...);

// コンパイラ自身のバグ（ユーザーのミスではない）を報告して終了する
_Noreturn void internal_error(const char *file, int line, const char *fmt, ...);

// switch の default など「来ないはず」の場所に置く
#define UNREACHABLE() \
    internal_error(__FILE__, __LINE__, "到達しないはずのコードに来ました")

#endif  // MYTHON_UTIL_H
