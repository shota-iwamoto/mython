#include "lexer.h"

#include <ctype.h>

#include "diag.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// インデントの最大ネスト深さ。
// 実際のコードで 32 段以上インデントすることはまずないので、
// 超えたらエラーにします（無限に伸ばす価値がない）。
#define MAX_INDENT_DEPTH 64

// ── 字句解析器の状態 ────────────────────────────────────────
// ソース全体を指すポインタ p を前に進めながら読んでいきます。
// line / line_start は、エラー報告のために常に最新に保ちます。
typedef struct {
    const char *file;
    const char *src;         // ソース全体（先頭）
    const char *p;           // 現在の読み取り位置
    const char *line_start;  // 現在の行の先頭
    int line;                // 現在の行番号（1 起算）

    // 直前の行（EOF トークンの位置決めに使う。第3章 3.6 節を参照）
    const char *prev_line_start;
    int prev_line;

    // ── インデントの状態 ──
    //
    // インデント幅のスタック。常に先頭は 0（トップレベル）。
    //   例: 0 → 4 → 8 とネストしている状態なら {0, 4, 8}
    int indent_stack[MAX_INDENT_DEPTH];
    int indent_len;  // スタックに積まれている段数（最低 1）

    // 括弧の深さ。0 より大きいときは改行を無視する（論理行が続く）。
    //   x = (1 +
    //        2)      ← この改行では NEWLINE を出さない
    int paren_depth;

    TokenVec out;  // 出力先
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
    t->text = NULL;
    return t;
}

// ── キーワード ──────────────────────────────────────────────
//
// 言語仕様 2.5 の予約語。**現時点で使わないものも予約しておきます。**
//
// 🤔 なぜ未使用の語まで予約するのか
//   今 `class` を変数名に使えるようにしてしまうと、第12章で class 構文を
//   入れたときに既存のコードが壊れます。最初から予約しておけば、
//   後方互換を壊さずに機能を追加できます。
static const char *KEYWORDS[] = {
    // 言語仕様 v1 で使う語
    "and", "as", "break", "class", "continue", "def", "elif", "else",
    "extern", "False", "for", "if", "import", "in", "is", "None",
    "not", "or", "pass", "return", "True", "while",
    // 将来のために予約（使うとエラーになる）
    "assert", "const", "del", "except", "finally", "from", "global", "lambda",
    "match", "nonlocal", "raise", "try", "with", "yield",
    NULL,
};

static bool is_keyword(const char *s, int len) {
    for (int i = 0; KEYWORDS[i]; i++)
        if ((int)strlen(KEYWORDS[i]) == len && memcmp(KEYWORDS[i], s, (size_t)len) == 0)
            return true;
    return false;
}

// ── 文字の判定 ──────────────────────────────────────────────
// <ctype.h> の関数は引数が負の値だと未定義動作なので、
// unsigned char にキャストしてから渡します（非 ASCII 対策）。

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// 基数 base における有効な数字かどうか
static int is_digit_of(char c, int base) {
    if (base == 2) return c == '0' || c == '1';
    if (base == 8) return c >= '0' && c <= '7';
    if (base == 10) return is_digit(c);
    // base == 16
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// エラー報告のために、指定範囲を指す一時 Token を組み立てる。
// 本物の Token をまだ tv_push していない段階でもエラーを出せるようにするため。
static Token span_token(Lexer *lx, const char *start, const char *end) {
    Token t = {0};
    t.file = lx->file;
    t.line_start = lx->line_start;
    t.line = lx->line;
    t.col = (int)(start - lx->line_start) + 1;
    t.len = (int)(end - start);
    if (t.len < 1) t.len = 1;
    return t;
}

// ── 数値リテラルの読み取り ──────────────────────────────────

static void read_int(Lexer *lx) {
    const char *start = lx->p;

    // 基数の接頭辞を判定する（言語仕様 2.7）
    //   0x / 0X → 16 進、0o / 0O → 8 進、0b / 0B → 2 進、それ以外 → 10 進
    int base = 10;
    if (lx->p[0] == '0' && lx->p[1] != '\0') {
        char c = lx->p[1];
        if (c == 'x' || c == 'X') base = 16;
        else if (c == 'o' || c == 'O') base = 8;
        else if (c == 'b' || c == 'B') base = 2;
        if (base != 10) lx->p += 2;  // 接頭辞を読み飛ばす
    }

    // 桁区切りの '_' を飛ばしながら、数字を文字列に集める（1_000 == 1000）
    char digits[64];
    int n = 0;
    while (is_digit_of(*lx->p, base) || *lx->p == '_') {
        if (*lx->p == '_') {
            lx->p++;
            continue;
        }
        if (n < (int)sizeof(digits) - 1) digits[n++] = *lx->p;
        lx->p++;
    }
    digits[n] = '\0';

    // 接頭辞の後に有効な数字が 1 つもない（0x や 0b だけ）
    if (n == 0) {
        Token tmp = span_token(lx, start, lx->p);
        const char *valid = base == 16 ? "0-9 a-f A-F"
                          : base == 8  ? "0-7"
                                       : "0-1";
        error_at_hint(&tmp, diag_fmt("基数 %d で使える数字は %s です", base, valid),
                      "数値リテラルに数字がありません");
    }

    // 数字の直後が識別子文字なら、それは 123abc や 0xFFg のような不正なリテラル
    if (isalpha((unsigned char)*lx->p) || *lx->p == '_') {
        Token tmp = span_token(lx, start, lx->p + 1);
        error_at_hint(&tmp, "数値と識別子の間に空白が必要かもしれません",
                      "数値リテラルの直後に文字が続いています");
    }

    // 文字列 → long long。オーバーフローを errno で検出する。
    //
    // ⚠️ strtoll は失敗を戻り値で表現できません（LLONG_MAX は正当な値でもある）。
    //    必ず errno を 0 にリセットしてから呼び、ERANGE を確認します。
    errno = 0;
    char *end;
    long long v = strtoll(digits, &end, base);
    if (errno == ERANGE) {
        Token tmp = span_token(lx, start, lx->p);
        error_at_hint(&tmp,
                      "int が表せるのは -9223372036854775808 〜 9223372036854775807 です",
                      "整数リテラルが int の範囲 (64bit) を超えています");
    }

    Token *t = tv_push(lx, TK_INT, start, (int)(lx->p - start));
    t->ival = v;
}

// ── 改行の処理 ──────────────────────────────────────────────

// '\n' を 1 つ読み進め、行番号と行頭ポインタを更新する。
// 3 か所から呼ばれるので関数にしています。
static void advance_newline(Lexer *lx) {
    // 今終えた行を「直前の行」として覚えておく（EOF の位置決め用）
    lx->prev_line_start = lx->line_start;
    lx->prev_line = lx->line;

    lx->p++;  // '\n' を消費
    lx->line++;
    lx->line_start = lx->p;
}

// ── インデントの処理（この章の核心）──────────────────────────

// 行頭にいる状態で呼ぶ。
//
// 空行・コメントだけの行を読み飛ばし、実質的な内容がある行の
// インデント幅（先頭の空白の個数）を返す。入力が終わったら -1 を返す。
//
// ★ 空行とコメント行を「インデント計算の対象外」にするのが重要です。
//   Python と同じ規則（言語仕様 2.4）。これをしないと、
//   ブロックの途中に空行を入れただけでエラーになってしまいます。
static int scan_indent(Lexer *lx) {
    for (;;) {
        int width = 0;
        while (*lx->p == ' ') {
            width++;
            lx->p++;
        }

        // タブは字句エラー（言語仕様 2.4）
        if (*lx->p == '\t') {
            Token tmp = span_token(lx, lx->p, lx->p + 1);
            error_at_hint(&tmp,
                          "タブ幅の解釈によってインデントの意味が変わるのを防ぐため、"
                          "Mython ではタブを禁止しています（半角スペース 4 個を推奨）",
                          "タブ文字は使えません");
        }

        if (*lx->p == '\n') {  // 空行 → インデントに影響させない
            advance_newline(lx);
            continue;
        }
        if (*lx->p == '#') {  // コメントだけの行 → 同じく影響させない
            while (*lx->p && *lx->p != '\n') lx->p++;
            if (*lx->p == '\n') {
                advance_newline(lx);
                continue;
            }
            return -1;  // ファイル末尾（改行なしでコメントが終わった）
        }
        if (*lx->p == '\0') return -1;  // 入力終了

        return width;  // 内容のある行が見つかった
    }
}

// 行のインデント幅とスタックを比較し、必要な INDENT / DEDENT を出す。
static void emit_indent_tokens(Lexer *lx, int width) {
    int top = lx->indent_stack[lx->indent_len - 1];

    if (width > top) {
        // 深くなった → INDENT を 1 個だけ出す
        //
        // ⚠️ 「4 段深くなったから INDENT 4 個」ではありません。
        //    インデント 1 段 = INDENT 1 個です。幅は任意（言語仕様 2.4）。
        if (lx->indent_len >= MAX_INDENT_DEPTH) {
            Token tmp = span_token(lx, lx->p, lx->p + 1);
            error_at(&tmp, "インデントが深すぎます（最大 %d 段）", MAX_INDENT_DEPTH - 1);
        }
        lx->indent_stack[lx->indent_len++] = width;
        tv_push(lx, TK_INDENT, lx->p, 0);
        return;
    }

    if (width < top) {
        // 浅くなった → 戻った段数ぶん DEDENT を出す
        //
        // ★ DEDENT は一度に複数個出ることがあります。
        //   深いネストから一気にトップレベルへ戻る場合です:
        //
        //       if a:          indent 0
        //           if b:      indent 4  → INDENT
        //               x      indent 8  → INDENT
        //       y              indent 0  → DEDENT, DEDENT （2 個！）
        while (lx->indent_len > 1 && lx->indent_stack[lx->indent_len - 1] > width) {
            lx->indent_len--;
            tv_push(lx, TK_DEDENT, lx->p, 0);
        }

        // 戻った先がスタックに無い＝どのブロックにも揃っていない
        //
        //       if a:
        //               x       indent 8
        //           y           indent 4 ← 8 でも 0 でもない。不正
        if (lx->indent_stack[lx->indent_len - 1] != width) {
            Token tmp = span_token(lx, lx->line_start, lx->p);
            error_at_hint(&tmp,
                          "外側のブロックのインデント幅と正確に一致させてください",
                          "インデントが揃っていません（どのブロックにも対応しません）");
        }
    }
    // width == top なら何も出さない（同じブロックの続き）
}

// ── 識別子・キーワードの読み取り ────────────────────────────

static int is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_ident_cont(char c) { return is_ident_start(c) || is_digit(c); }

static void read_ident(Lexer *lx) {
    const char *start = lx->p;
    while (is_ident_cont(*lx->p)) lx->p++;
    int len = (int)(lx->p - start);

    TokenKind kind = is_keyword(start, len) ? TK_KEYWORD : TK_IDENT;
    Token *t = tv_push(lx, kind, start, len);

    // 名前は何度も比較するので、NUL 終端した複製を持たせる。
    // （記号と違い、strcmp で比較できるほうが圧倒的に扱いやすい）
    t->text = xstrndup(start, (size_t)len);
}

// ── 記号の読み取り ──────────────────────────────────────────

// ★ 長い記号を先に並べること。
//    上から順に試すので、"//" より先に "/" を書くと
//    "//" が "/" 2 個に読まれてしまいます（最長一致の原則）。
//
// ⚠️ 第5章で複合代入を足すときも同じ原則が効きます。
//    "//=" は "//" より先、"+=" は "+" より先に置かなければなりません。
static const char *PUNCTS[] = {
    // 3 文字
    "//=",
    // 2 文字
    "//", "**", "<<", ">>", "+=", "-=", "*=", "%=",
    // 2 文字（第6章：比較演算子）
    //   ⚠️ "<=" は "<" より、"==" は "=" より先に来ること（最長一致）。
    //      "<<" が既に上の段にあるので、"<" を足しても壊れません。
    "==", "!=", "<=", ">=",
    // 2 文字（第8章：戻り型の矢印）
    //   ⚠️ "->" は "-" より先（最長一致 4 度目）
    "->",
    // 1 文字
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "(", ")", ":", "=",
    "<", ">",
    ",",  // 第8章：引数の区切り
    NULL,
};

// 開き括弧・閉じ括弧の対応表。
// 第10章で "[" "]"、第12章で "{" "}" を足すときはここに 1 文字ずつ加えるだけです。
static const char *OPEN_BRACKETS = "(";
static const char *CLOSE_BRACKETS = ")";

// 記号を 1 つ読む。読めたら 1、読めなければ 0 を返す。
static int read_punct(Lexer *lx) {
    for (int i = 0; PUNCTS[i]; i++) {
        size_t len = strlen(PUNCTS[i]);
        if (strncmp(lx->p, PUNCTS[i], len) == 0) {
            tv_push(lx, TK_PUNCT, lx->p, (int)len);

            // 括弧の深さを追跡する（括弧の中では改行を無視するため）
            if (len == 1) {
                if (strchr(OPEN_BRACKETS, *lx->p)) {
                    lx->paren_depth++;
                } else if (strchr(CLOSE_BRACKETS, *lx->p)) {
                    // ⚠️ 負にしない。対応しない ')' は構文解析器が報告します。
                    //    ここで負にすると、以降ずっと改行が無視されて
                    //    まったく別の場所で不可解なエラーになります。
                    if (lx->paren_depth > 0) lx->paren_depth--;
                }
            }

            lx->p += len;
            return 1;
        }
    }
    return 0;
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

    // スタックの底は常に 0（トップレベルのインデント幅）
    lx.indent_stack[0] = 0;
    lx.indent_len = 1;

    // 次に読むのが「論理行の先頭」かどうか。最初は当然そう。
    bool at_line_start = true;

    while (*lx.p) {
        // ── ① 行頭処理：インデントを測って INDENT / DEDENT を出す ──
        //
        // 括弧の中（paren_depth > 0）では論理行が続いているので、
        // 行頭であってもインデントは計算しません。
        if (at_line_start && lx.paren_depth == 0) {
            int width = scan_indent(&lx);
            if (width < 0) break;  // 空行・コメントだけで入力が終わった
            emit_indent_tokens(&lx, width);
            at_line_start = false;
            continue;
        }
        at_line_start = false;

        // ── ② 改行：NEWLINE を出して行頭に戻る ──
        if (*lx.p == '\n') {
            // ★ 括弧の中では改行を無視する（論理行が続く）
            //     x = (1 +
            //          2)     ← ここで NEWLINE を出してはいけない
            if (lx.paren_depth == 0) {
                tv_push(&lx, TK_NEWLINE, lx.p, 0);
                at_line_start = true;
            }
            advance_newline(&lx);
            continue;
        }

        // 空白（スペース）。行頭の空白は ① で処理済みなので、ここは行中の空白。
        if (*lx.p == ' ') {
            lx.p++;
            continue;
        }

        // タブは字句エラー（言語仕様 2.4：インデントの曖昧さを排除するため）
        if (*lx.p == '\t') {
            Token tmp = span_token(&lx, lx.p, lx.p + 1);
            error_at_hint(&tmp,
                          "タブ幅の解釈によってインデントの意味が変わるのを防ぐため、"
                          "Mython ではタブを禁止しています（半角スペース 4 個を推奨）",
                          "タブ文字は使えません");
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

        // 識別子・キーワード
        //
        // ⚠️ 数値より後に判定すること。先にすると 123 の 1 文字目で
        //    識別子の判定に失敗するだけですが、順序を意識する習慣をつけます。
        if (is_ident_start(*lx.p)) {
            read_ident(&lx);
            continue;
        }

        // 記号
        if (read_punct(&lx)) continue;

        // ここに来たら、現時点では扱えない文字。
        // 非 ASCII バイトは '%c' で出すと化けるので、16 進で示す。
        Token tmp = span_token(&lx, lx.p, lx.p + 1);
        unsigned char c = (unsigned char)*lx.p;
        if (c < 0x20 || c >= 0x7f)
            error_at(&tmp, "解釈できない文字です (0x%02X)", c);
        error_at(&tmp, "解釈できない文字です: '%c'", c);
    }

    // 入力の終わりを示すトークンを必ず 1 個置く。
    // これがあると、パーサが「配列の終わりを越えたか」を毎回気にせずに済む。
    //
    // ★ EOF の「位置」に一手間かける。
    //
    //   read_file() が末尾の改行を保証しているため、素朴に現在位置を使うと
    //   EOF は「最後の行の次にある空行」を指してしまいます。すると
    //   「閉じ括弧がありません」のエラーで抜粋が空行になり、役に立ちません:
    //
    //       --> t.my:3:1
    //        |
    //      3 |
    //        | ^ ここに ')' が必要です     ← 何も見えない
    //
    //   そこで、現在行が空なら直前の行の末尾を指すようにします:
    //
    //       --> t.my:2:7
    //        |
    //      2 | (1 + 2
    //        |       ^ ここに ')' が必要です   ← 読める！
    const char *end_loc = lx.p;
    if (lx.p == lx.line_start && lx.prev_line_start) {
        lx.line_start = lx.prev_line_start;
        lx.line = lx.prev_line;
        const char *eol = lx.prev_line_start;
        while (*eol && *eol != '\n') eol++;
        end_loc = eol;
    }

    // ★ ファイル末尾では、開いているインデントぶんの DEDENT を全部出す。
    //
    //   これを忘れると、ブロックの中でファイルが終わったときに
    //   構文解析器が「ブロックが閉じられていない」ことに気づけません。
    //
    //       if a:
    //           x        ← ファイルがここで終わる
    //                       DEDENT を出さないと block 規則が終われない
    while (lx.indent_len > 1) {
        lx.indent_len--;
        tv_push(&lx, TK_DEDENT, end_loc, 0);
    }

    tv_push(&lx, TK_EOF, end_loc, 0);
    return lx.out;
}

// ── デバッグ出力 ────────────────────────────────────────────

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TK_EOF: return "EOF";
        case TK_INT: return "INT";
        case TK_PUNCT: return "PUNCT";
        case TK_IDENT: return "IDENT";
        case TK_KEYWORD: return "KEYWORD";
        case TK_NEWLINE: return "NEWLINE";
        case TK_INDENT: return "INDENT";
        case TK_DEDENT: return "DEDENT";
        default: UNREACHABLE();
    }
}

bool tok_is(Token *tok, const char *op) {
    size_t len = strlen(op);
    return tok->kind == TK_PUNCT && (size_t)tok->len == len &&
           memcmp(tok->loc, op, len) == 0;
}

bool tok_is_kw(Token *tok, const char *kw) {
    return tok->kind == TK_KEYWORD && strcmp(tok->text, kw) == 0;
}

void dump_tokens(TokenVec toks) {
    for (int i = 0; i < toks.len; i++) {
        Token *t = &toks.data[i];
        printf("%3d  %-8s  %d:%-3d  ", i, token_kind_name(t->kind), t->line, t->col);
        switch (t->kind) {
            case TK_INT:
                printf("%lld", t->ival);
                break;
            // 仮想トークンと EOF は長さ 0 なので表示する実体がない
            case TK_EOF:
            case TK_NEWLINE:
            case TK_INDENT:
            case TK_DEDENT:
                break;
            default:
                printf("%.*s", t->len, t->loc);
                break;
        }
        printf("\n");
    }
}

// エラー報告（error_at / diag_fail）は diag.c に移しました。
