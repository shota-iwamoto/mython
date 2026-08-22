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
#include <sys/wait.h>

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

// ── None（null ポインタ）の検査（第12章）────────────────────
//
// ★ クラス型のフィールドは calloc により NULL から始まります
//   （既定値を作ろうとすると無限再帰するため。docs/chapters/ch12-class.md 12.6）。
//   init で入れ忘れたまま参照すると segfault しますが、
//   ここを通しておけば「何が起きたか分かるメッセージ」に変わります。
//
// ⚠️ 本来の解決策は型システム側（T | None と narrowing）です。第15章で塞ぎます。
void *my_check_not_none(void *p) {
    if (!p) my_panic("field access on None (uninitialized reference field?)");
    return p;
}

// ── 出力（print のオーバーロード）──────────────────────────

void my_print_int(long long v) { printf("%lld\n", v); }

// stderr にそのまま書く（改行は付けない）。第18章。
//
// ★ コンパイラは診断を stderr に書きます。print は stdout なので、
//   セルフホストの診断にはこれが要ります（移植で見つかった穴）。
void my_eprint(const char *s) { fputs(s, stderr); }

void my_print_str(const char *s) { printf("%s\n", s); }

void my_print_bool(long long v) { printf("%s\n", v ? "True" : "False"); }

// ── 文字列 ─────────────────────────────────────────────────
//
// ★ 第15章：str の表現に「長さ」を持たせました。
//
//     [ i64 長さ ][ バイト列 ... ][ '\0' ]
//                  ^ str の値が指すのはここ
//
//   ⚠️ なぜ変えたか（ch15 15.7 節）
//     それまでの str は「ただの NUL 終端文字列」でした。すると
//     len(s) も s[i] も毎回 strlen する＝ O(n) になり、
//     字句解析器のように 1 文字ずつ回るコードが O(n^2) になります。
//     4000 行のソースを読むだけで数秒かかる計算で、セルフホストできません。
//
//   ★ 値が指すのは「データの先頭」のままなので、C から見ると今までどおり
//     NUL 終端の char * です（extern に渡してもそのまま使えます）。
//     長さは p[-1] の位置（8 バイト手前）にあります。

// 長さ len のバイト列を置ける str を確保する（NUL の分も含めて確保）。
char *my_str_alloc(long long len) {
    char *base = my_alloc(8 + len + 1);
    *(long long *)base = len;
    return base + 8;
}

// C 文字列から str を作る（argv など、ヘッダを持たない文字列から作るとき）
char *my_str_from_cstr(const char *s) {
    long long n = (long long)strlen(s);
    char *p = my_str_alloc(n);
    memcpy(p, s, (size_t)n + 1);
    return p;
}

// ★ O(1) になりました（第15章）。
long long my_str_len(const char *s) { return ((const long long *)s)[-1]; }

char *my_str_concat(const char *a, const char *b) {
    long long la = my_str_len(a);
    long long lb = my_str_len(b);
    char *p = my_str_alloc(la + lb);
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
    char *p = my_str_alloc(n);
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

char *my_str_from_bool(long long v) {
    return my_str_from_cstr(v ? "True" : "False");
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
    char *p = my_str_alloc(1);
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

// ── ファイル入出力（第14章）────────────────────────────────
//
// ★ ここは「C でしか書けないもの」です。Mython で書けるものは lib/*.my に置きます
//   （docs/chapters/ch14-stdlib.md 14.1 節）。
//
// ⚠️ 失敗したら panic で落とします。エラー値を返して利用者に検査させる形は
//    `T | None` がまだ無いので書けません（第15章）。

char *my_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot open file: %s", path);
        my_panic(buf);
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = my_str_alloc((long long)size);
    size_t got = fread(buf, 1, (size_t)size, fp);
    buf[got] = '\0';
    // ⚠️ テキストモードの差などで読めたバイト数が減ることがあるので、
    //    実際に読めた長さで書き直します（長さは 8 バイト手前）。
    ((long long *)buf)[-1] = (long long)got;
    fclose(fp);
    return buf;
}

void my_write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot write file: %s", path);
        my_panic(buf);
    }
    fputs(text, fp);
    fclose(fp);
}

// ⚠️ bool ではなく int を返します。extern の境界を bool は越えられません
//    （14.2 節。C の _Bool と i1 の ABI が環境依存のため）。
long long my_file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

// ── コマンドライン引数と外部コマンド（第14章）──────────────
//
// ★ この 2 つが無いと、Mython 製コンパイラは「コマンド」になれません
//   （docs/design/self-hosting.md 3.4）。

static long long g_argc;
static char **g_argv;

// 生成される C の main が、いちばん最初に呼びます。
void my_set_args(long long argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

// ⚠️ system() が返すのは「終了コード」ではなく wait(2) の状態値です。
//    そのまま返すと exit 3 が 768（3 << 8）に見えて驚くので、
//    ここで終了コードに直します。境界の食い違いはランタイムで吸収します。
// 環境変数を読む（無ければ空文字列）。第18章。
//
// ★ stage1 が標準ライブラリの場所を知るために使います。C 版はビルド時に
//   埋め込みますが（-DMYTHON_LIB_DIR）、Mython にプリプロセッサは無いので
//   実行時に読みます。C 版も同じ環境変数を見るようにして、挙動を揃えます。
char *my_getenv(const char *name) {
    const char *v = getenv(name);
    if (!v) return my_str_from_cstr("");
    return my_str_from_cstr(v);
}

long long my_system(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
    if (WIFEXITED(st)) return (long long)WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return (long long)(128 + WTERMSIG(st));
    return (long long)st;
}

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

// list[str] を sep でつないだ str を作る（第15章）。
//
// ★ Mython 側で out = out + xs[i] と書くと O(n^2) になります
//   （1 回ごとに全部コピーするため）。文字列を「組み立てる」のは
//   セルフホストの IR 出力で毎回やることなので、ここだけは C で用意します。
//   利用者は list[str] に溜めて最後に join する、という形で O(n) になります。
char *my_str_join(MyList *xs, const char *sep) {
    long long n = xs->len;
    long long sep_len = my_str_len(sep);

    long long total = n > 0 ? sep_len * (n - 1) : 0;
    for (long long i = 0; i < n; i++)
        total += my_str_len(((char **)xs->data)[i]);

    char *p = my_str_alloc(total);
    long long at = 0;
    for (long long i = 0; i < n; i++) {
        if (i > 0) {
            memcpy(p + at, sep, (size_t)sep_len);
            at += sep_len;
        }
        const char *e = ((char **)xs->data)[i];
        long long el = my_str_len(e);
        memcpy(p + at, e, (size_t)el);
        at += el;
    }
    p[total] = '\0';
    return p;
}

// argv を list[str] にして返す（第14章）。
//
// ★ MyList を使うので、list の実装より後ろに置いています。
//   argv の文字列はプロセスの寿命のあいだ有効なので、複製せずそのまま指します。
MyList *my_argv(void) {
    MyList *l = my_list_new();
    // ⚠️ argv の文字列は C のものなので長さヘッダがありません。
    //    Mython の str として渡すには作り直す必要があります（第15章）。
    for (long long i = 0; i < g_argc; i++)
        my_list_push_ptr(l, my_str_from_cstr(g_argv[i]));
    return l;
}

// 文字列の i 番目のバイトを int で返す（第15章）。
//
// ★ my_str_index と違い、確保しません。字句解析器のように 1 文字ずつ回る
//   コードでは、1 文字ごとの 2 バイト確保が効いてきます（ch15 15.7 で実測）。
// ⚠️ 言語には足していません。lib/strings.my から extern で呼ぶだけです
//   （第14章で引いた境界線のとおり）。
long long my_byte_at(const char *s, long long i) {
    long long n = my_str_len(s);
    if (i < 0 || i >= n) {
        char buf[128];
        snprintf(buf, sizeof(buf), "index out of range: %lld (len=%lld)", i, n);
        my_panic(buf);
    }
    return (long long)(unsigned char)s[i];
}

// 文字列の添字：1 文字の str を返す（char 型は作らない。型システム 5.8）
char *my_str_index(const char *s, long long i) {
    long long n = my_str_len(s);
    if (i < 0 || i >= n) {
        char buf[128];
        snprintf(buf, sizeof(buf), "index out of range: %lld (len=%lld)", i, n);
        my_panic(buf);
    }
    char *p = my_str_alloc(1);
    p[0] = s[i];
    p[1] = '\0';
    return p;
}
