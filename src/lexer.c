#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── 字句解析器の状態 ────────────────────────────────────────
// ソース全体を指すポインタ p を前に進めながら読んでいきます。
// line / line_start は、エラー報告のために常に最新に保ちます。
typedef struct {
    const char *file;
    const char *src;         // ソース全体（先頭）
    const char *p;           // 現在の読み取り位置
    const char *line_start;  // 現在の行の先頭
    int line;                // 現在の行番号（1 起算）
    TokenVec out;            // 出力先
} Lexer;

// ── TokenVec の操作 ────────────────────────────────────────

static void tv_init(TokenVec *tv) {
    tv->cap = 64;
    tv->len = 0;
    tv->data = xmalloc(sizeof(Token) * (size_t)tv->cap);
}

// 新しいトークンを追加し、そのポインタを返す。
// 位置情報（file/line/col/line_start）はここで一括して埋めるので、
// 呼び出し側は kind と値だけを設定すればよい。
static Token *tv_push(Lexer *lx, TokenKind kind, const char *loc, int len) {
    if (lx->out.len == lx->out.cap) {
        lx->out.cap *= 2;
        Token *newdata = xmalloc(sizeof(Token) * (size_t)lx->out.cap);
        memcpy(newdata, lx->out.data, sizeof(Token) * (size_t)lx->out.len);
        lx->out.data = newdata;
    }
    Token *t = &lx->out.data[lx->out.len++];
    t->kind = kind;
    t->file = lx->file;
    t->line_start = lx->line_start;
    t->line = lx->line;
    t->col = (int)(loc - lx->line_start) + 1;  // 桁は 1 起算
    t->loc = loc;
    t->len = len;
    t->ival = 0;
    return t;
}

// ── 文字の判定 ──────────────────────────────────────────────
// <ctype.h> の関数は引数が負の値だと未定義動作なので、
// unsigned char にキャストしてから渡します（非 ASCII 対策）。

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// ── 数値リテラルの読み取り ──────────────────────────────────

static void read_int(Lexer *lx) {
    const char *start = lx->p;

    // 桁区切りの '_' を飛ばしながら、数字を文字列に集める（言語仕様 2.7）
    char digits[64];
    int n = 0;
    while (is_digit(*lx->p) || *lx->p == '_') {
        if (*lx->p == '_') {
            lx->p++;
            continue;
        }
        if (n < (int)sizeof(digits) - 1) digits[n++] = *lx->p;
        lx->p++;
    }
    digits[n] = '\0';

    // 数字の直後が識別子文字なら、それは 123abc のような不正なリテラル
    if (isalpha((unsigned char)*lx->p) || *lx->p == '_') {
        Token tmp = {0};
        tmp.file = lx->file;
        tmp.line_start = lx->line_start;
        tmp.line = lx->line;
        tmp.col = (int)(start - lx->line_start) + 1;
        tmp.len = (int)(lx->p - start) + 1;
        error_at(&tmp, "数値リテラルの直後に文字が続いています");
    }

    // 文字列 → long long。オーバーフローを errno で検出する。
    errno = 0;
    char *end;
    long long v = strtoll(digits, &end, 10);
    if (errno == ERANGE) {
        Token tmp = {0};
        tmp.file = lx->file;
        tmp.line_start = lx->line_start;
        tmp.line = lx->line;
        tmp.col = (int)(start - lx->line_start) + 1;
        tmp.len = (int)(lx->p - start);
        error_at(&tmp, "整数リテラルが int の範囲 (64bit) を超えています");
    }

    Token *t = tv_push(lx, TK_INT, start, (int)(lx->p - start));
    t->ival = v;
}

// ── 本体 ───────────────────────────────────────────────────

TokenVec tokenize(const char *file, const char *src) {
    Lexer lx = {0};
    lx.file = file;
    lx.src = src;
    lx.p = src;
    lx.line_start = src;
    lx.line = 1;
    tv_init(&lx.out);

    while (*lx.p) {
        // 改行：行番号を進める
        // 第4章ではここで NEWLINE トークンと INDENT/DEDENT を生成します。
        if (*lx.p == '\n') {
            lx.p++;
            lx.line++;
            lx.line_start = lx.p;
            continue;
        }

        // 空白（スペース）
        if (*lx.p == ' ') {
            lx.p++;
            continue;
        }

        // タブは字句エラー（言語仕様 2.4：インデントの曖昧さを排除するため）
        if (*lx.p == '\t') {
            Token tmp = {0};
            tmp.file = lx.file;
            tmp.line_start = lx.line_start;
            tmp.line = lx.line;
            tmp.col = (int)(lx.p - lx.line_start) + 1;
            tmp.len = 1;
            error_at(&tmp, "タブ文字は使えません。半角スペースを使ってください");
        }

        // コメント：# から行末まで（改行は次の周回で処理する）
        if (*lx.p == '#') {
            while (*lx.p && *lx.p != '\n') lx.p++;
            continue;
        }

        // 整数リテラル
        if (is_digit(*lx.p)) {
            read_int(&lx);
            continue;
        }

        // ここに来たら、第1章では扱えない文字
        Token tmp = {0};
        tmp.file = lx.file;
        tmp.line_start = lx.line_start;
        tmp.line = lx.line;
        tmp.col = (int)(lx.p - lx.line_start) + 1;
        tmp.len = 1;
        error_at(&tmp, "解釈できない文字です: '%c'", *lx.p);
    }

    // 入力の終わりを示すトークンを必ず 1 個置く。
    // これがあると、パーサが「配列の終わりを越えたか」を毎回気にせずに済む。
    tv_push(&lx, TK_EOF, lx.p, 0);
    return lx.out;
}

// ── デバッグ出力 ────────────────────────────────────────────

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TK_EOF: return "EOF";
        case TK_INT: return "INT";
        default: UNREACHABLE();
    }
}

void dump_tokens(TokenVec toks) {
    for (int i = 0; i < toks.len; i++) {
        Token *t = &toks.data[i];
        printf("%3d  %-8s  %d:%-3d  ", i, token_kind_name(t->kind), t->line, t->col);
        switch (t->kind) {
            case TK_INT:
                printf("%lld", t->ival);
                break;
            case TK_EOF:
                break;
            default:
                printf("%.*s", t->len, t->loc);
                break;
        }
        printf("\n");
    }
}

// ── エラー報告 ──────────────────────────────────────────────

_Noreturn void error_at(Token *tok, const char *fmt, ...) {
    // error_at_pos は可変長引数を取るので、ここで一度整形してから渡す。
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    error_at_pos(tok->file, tok->line_start, tok->line, tok->col, tok->len, "%s", msg);
}
