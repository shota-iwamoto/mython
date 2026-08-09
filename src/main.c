// main.c — コマンドライン処理と各パスの起動
//
//   mythonc [options] <input.my>
//
// パイプライン：
//   read_file → tokenize → parse → codegen → clang
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "util.h"

static void usage(int status) {
    FILE *out = status == 0 ? stdout : stderr;
    fprintf(out,
            "Mython コンパイラ (stage0)\n"
            "\n"
            "使い方: mythonc [オプション] <入力.my>\n"
            "\n"
            "オプション:\n"
            "  -o <file>       出力する実行ファイル名（既定: a.out）\n"
            "  -S              LLVM IR (.ll) を出力して終了\n"
            "  --dump-tokens   トークン列を表示して終了（字句解析のデバッグ用）\n"
            "  --dump-ast      AST を S 式で表示して終了（構文解析のデバッグ用）\n"
            "  --keep-ll       実行ファイル生成後も .ll を残す\n"
            "  -O0|-O1|-O2|-O3 clang に渡す最適化レベル（既定: -O0）\n"
            "  -h, --help      この使い方を表示\n");
    exit(status);
}

// 実行するステージ
typedef enum {
    STAGE_ALL,          // 実行ファイルまで作る
    STAGE_DUMP_TOKENS,  // 字句解析まで
    STAGE_DUMP_AST,     // 構文解析まで
    STAGE_EMIT_IR,      // コード生成まで（-S）
} Stage;

typedef struct {
    const char *input;
    const char *output;
    const char *opt_level;
    Stage stage;
    int keep_ll;
} Options;

static Options parse_args(int argc, char **argv) {
    Options o = {0};
    o.output = "a.out";
    o.opt_level = "-O0";
    o.stage = STAGE_ALL;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) usage(0);

        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) error("-o の後に出力ファイル名が必要です");
            o.output = argv[++i];
            continue;
        }
        if (strcmp(a, "-S") == 0) { o.stage = STAGE_EMIT_IR; continue; }
        if (strcmp(a, "--dump-tokens") == 0) { o.stage = STAGE_DUMP_TOKENS; continue; }
        if (strcmp(a, "--dump-ast") == 0) { o.stage = STAGE_DUMP_AST; continue; }
        if (strcmp(a, "--keep-ll") == 0) { o.keep_ll = 1; continue; }

        if (strcmp(a, "-O0") == 0 || strcmp(a, "-O1") == 0 ||
            strcmp(a, "-O2") == 0 || strcmp(a, "-O3") == 0) {
            o.opt_level = a;
            continue;
        }

        if (a[0] == '-' && a[1] != '\0') error("不明なオプション: %s", a);

        if (o.input) error("入力ファイルが複数指定されています: %s と %s", o.input, a);
        o.input = a;
    }

    if (!o.input) usage(1);
    return o;
}

// 出力ファイル名から .ll のパスを作る（a.out → a.out.ll）
static char *ll_path_for(const char *output) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.ll", output);
    return sb_str(&sb);
}

int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);

    // ── ソースを読む ──
    char *src = read_file(opt.input);

    // ── ① 字句解析 ──
    TokenVec toks = tokenize(opt.input, src);
    if (opt.stage == STAGE_DUMP_TOKENS) {
        dump_tokens(toks);
        return 0;
    }

    // ── ② 構文解析 ──
    Node *ast = parse(toks);
    if (opt.stage == STAGE_DUMP_AST) {
        dump_ast(ast);
        return 0;
    }

    // ── ③ 意味解析・型検査 ──
    // 第5章で sema(ast) がここに入ります。

    // ── ④ コード生成 ──
    char *ir = codegen(ast, opt.input);

    if (opt.stage == STAGE_EMIT_IR) {
        // -S : .ll を書き出して終了
        if (strcmp(opt.output, "a.out") == 0) {
            // -o が指定されていなければ標準出力へ
            fputs(ir, stdout);
        } else {
            write_file(opt.output, ir);
        }
        return 0;
    }

    // ── ⑤ clang に丸投げして実行ファイルを作る ──
    char *ll = ll_path_for(opt.output);
    write_file(ll, ir);

    StrBuf cmd;
    sb_init(&cmd);
    sb_printf(&cmd, "clang %s '%s' -o '%s'", opt.opt_level, ll, opt.output);

    int rc = system(sb_str(&cmd));
    if (rc != 0) {
        // ここに来たら、生成した IR に問題があるということ。
        // .ll を残して調査できるようにする。
        fprintf(stderr,
                "error: clang の実行に失敗しました（生成した IR に問題があります）\n"
                "  生成された IR を残しました: %s\n"
                "  次のコマンドで詳しく調べられます:\n"
                "    clang %s -o /dev/null\n",
                ll, ll);
        return 1;
    }

    if (!opt.keep_ll) unlink(ll);
    return 0;
}
