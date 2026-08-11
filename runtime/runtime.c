// runtime/runtime.c — Mython のランタイムライブラリ
//
// ★ ここに置くもの：ループ・分岐・メモリ確保を含む処理（規約 R10）。
//   生成する LLVM IR を単純に保つために、複雑さをこちら側に押し出します。
//
// ⚠️ 関数名は全部 my_ で始めます。libc のシンボルと衝突させないためです
//    （第8章でグローバル変数に @g. を付けたのと同じ理由）。
//
// メモリは解放しません（docs/design/memory-model.md 3 節）。
// コンパイラは「起動して、変換して、終了する」プログラムなので、
// プロセス終了時に OS がまとめて回収します。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── エラー ─────────────────────────────────────────────────

// 回復不能なエラー。stderr に出して終了コード 1 で死ぬ。
// 例外機構（try / except）は v1 では採用しません（言語仕様 8 節）。
_Noreturn void my_panic(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}

// ── メモリ ─────────────────────────────────────────────────

// ★ calloc でゼロ初期化し、失敗したら即終了する。
//   即終了にすることで、生成する IR に NULL チェックを入れずに済みます。
void *my_alloc(long long size) {
    void *p = calloc(1, (size_t)size);
    if (!p) my_panic("out of memory");
    return p;
}

// ── 出力（print のオーバーロード）──────────────────────────

void my_print_int(long long v) { printf("%lld\n", v); }

void my_print_str(const char *s) { printf("%s\n", s); }

void my_print_bool(long long v) { printf("%s\n", v ? "True" : "False"); }

// ── 文字列 ─────────────────────────────────────────────────

long long my_str_len(const char *s) { return (long long)strlen(s); }

char *my_str_concat(const char *a, const char *b) {
    long long la = (long long)strlen(a);
    long long lb = (long long)strlen(b);
    char *p = my_alloc(la + lb + 1);
    memcpy(p, a, (size_t)la);
    memcpy(p + la, b, (size_t)lb);
    p[la + lb] = '\0';
    return p;
}

// strcmp の符号をそのまま返す。
// ★ これ 1 つで == != < <= > >= の 6 種類すべてに使えます
//   （生成側は結果を 0 と比べる述語を変えるだけ）。
long long my_str_cmp(const char *a, const char *b) {
    int r = strcmp(a, b);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

char *my_str_from_int(long long v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    char *p = my_alloc(n + 1);
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

char *my_str_from_bool(long long v) {
    const char *s = v ? "True" : "False";
    long long n = (long long)strlen(s);
    char *p = my_alloc(n + 1);
    memcpy(p, s, (size_t)n + 1);
    return p;
}

// 文字列を整数にする。パースできなければ実行時エラー（言語仕様 7 節）。
long long my_str_to_int(const char *s) {
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') my_panic("int(): not a number");
    return v;
}

long long my_ord(const char *s) {
    if (s[0] == '\0') my_panic("ord(): empty string");
    return (long long)(unsigned char)s[0];
}

char *my_chr(long long v) {
    if (v < 0 || v > 255) my_panic("chr(): out of range");
    char *p = my_alloc(2);
    p[0] = (char)v;
    p[1] = '\0';
    return p;
}

// ── 検査つきの算術（規約 R10）──────────────────────────────
//
// ★ 0 除算は SIGFPE でプロセスが死にます。何が起きたか分からないより、
//   メッセージを出して死ぬほうがずっと親切です。
//   分岐を IR に出さず、ランタイム関数に押し込むのが R10 の実践です。

long long my_floordiv(long long a, long long b) {
    if (b == 0) my_panic("division by zero");
    return a / b;
}

long long my_mod(long long a, long long b) {
    if (b == 0) my_panic("division by zero");
    return a % b;
}

// 繰り返し二乗法。ループがあるので当然ランタイム側（R10）。
// 負の指数は int で表せないので実行時エラーにします（第2章から先送りしていた宿題）。
long long my_ipow(long long base, long long exp) {
    if (exp < 0) my_panic("negative exponent");
    long long r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}

// ── プロセス ───────────────────────────────────────────────

_Noreturn void my_exit(long long code) { exit((int)code); }

// ── list[T]（第10章）────────────────────────────────────────
//
// ★ 要素はすべて 8 バイトに統一します（int/bool は i64、str/list はポインタ）。
//   要素サイズが型ごとに違うと getelementptr のオフセット計算が型ごとに
//   変わりますが、8 バイト固定なら「long long の配列」と「void* の配列」の
//   2 種類だけで済みます。bool で 7 バイト無駄になりますが、
//   実装の単純さと引き換えにするなら安い代償です。

typedef struct {
    void *data;  // 要素の配列（8 バイト × cap）
    long long len;
    long long cap;
} MyList;

MyList *my_list_new(void) {
    MyList *l = my_alloc((long long)sizeof(MyList));
    l->cap = 4;
    l->data = my_alloc(l->cap * 8);
    l->len = 0;
    return l;
}

long long my_list_len(MyList *l) { return l->len; }

// ⚠️ realloc を使わないのは「一度渡したポインタは永久に有効」という
//    方針（メモリモデル 3 節）と噛み合わないためです。
//    memcpy して古い領域を捨てるほうが、方針と一貫します。
//
// ★ 倍々に増やすので、n 回の append の総コストは O(n) です。
static void my_list_grow(MyList *l) {
    if (l->len < l->cap) return;
    long long ncap = l->cap * 2;
    void *nd = my_alloc(ncap * 8);
    memcpy(nd, l->data, (size_t)l->len * 8);
    l->data = nd;
    l->cap = ncap;
}

// 範囲検査（規約 R10）。
// ★ 検査をここに置くので、生成する IR に分岐が 1 つも出ません。
// ⚠️ 負の添字は「範囲外」です。Python の xs[-1] は採用しません。
static void my_list_check(MyList *l, long long i) {
    if (i < 0 || i >= l->len) {
        char buf[128];
        snprintf(buf, sizeof(buf), "index out of range: %lld (len=%lld)", i, l->len);
        my_panic(buf);
    }
}

void my_list_push_i64(MyList *l, long long v) {
    my_list_grow(l);
    ((long long *)l->data)[l->len++] = v;
}

void my_list_push_ptr(MyList *l, void *v) {
    my_list_grow(l);
    ((void **)l->data)[l->len++] = v;
}

long long my_list_get_i64(MyList *l, long long i) {
    my_list_check(l, i);
    return ((long long *)l->data)[i];
}

void *my_list_get_ptr(MyList *l, long long i) {
    my_list_check(l, i);
    return ((void **)l->data)[i];
}

void my_list_set_i64(MyList *l, long long i, long long v) {
    my_list_check(l, i);
    ((long long *)l->data)[i] = v;
}

void my_list_set_ptr(MyList *l, long long i, void *v) {
    my_list_check(l, i);
    ((void **)l->data)[i] = v;
}

// 文字列の添字：1 文字の str を返す（char 型は作らない。型システム 5.8）
char *my_str_index(const char *s, long long i) {
    long long n = (long long)strlen(s);
    if (i < 0 || i >= n) {
        char buf[128];
        snprintf(buf, sizeof(buf), "index out of range: %lld (len=%lld)", i, n);
        my_panic(buf);
    }
    char *p = my_alloc(2);
    p[0] = s[i];
    p[1] = '\0';
    return p;
}
