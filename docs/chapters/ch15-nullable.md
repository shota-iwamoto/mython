# 第15章 セルフホスト準備（T | None と棚卸し）

> **この章のゴール**
> `T | None` と絞り込み（narrowing）が使える。
> **そして「コンパイラを Mython で書けるか」を確認し終える。**
>
> ```bash
> $ cat t.my
> class Node:
>     v: int
>     next: Node | None          # ★ None かもしれない
>
>     def init(self, v: int) -> None:
>         self.v = v
>
> def main() -> int:
>     a: Node = Node(1)
>     b: Node = Node(2)
>     a.next = b                 # Node を Node | None に入れる（広げる方向）
>
>     cur: Node | None = a
>     while cur is not None:     # ★ この中では cur は Node
>         print(cur.v)
>         cur = cur.next
>     return 0
> $ ./build/mythonc t.my -o t && ./t
> 1
> 2
> ```

**この章は「新しい機能を思いつく章」ではありません。**

第12章から第14章まで、**「無いから諦めた」と書いた回数は 3 回**あります。

| 章 | 諦めたこと | 記録した場所 |
|---|---|---|
| ch12 | クラス型のフィールドが NULL から始まる | 12.6 節・既知の課題 |
| ch14 | `strings.find` が「無い」を `-1` で返す | 14.5 節 |
| ch14 | `dict.get` がキー無しで panic する | 14.6 節 |

**全部同じ原因です。「値が無い」を型で表せないこと。**

この章では、記録から拾って潰します。**思いつきでは何も足しません。**

---

## 目次

- [15.1 棚卸し：何が足りないか](#151-棚卸し何が足りないか)
- [15.2 T | None を型に足す](#152-t--none-を型に足す)
- [15.3 None リテラルと is / is not](#153-none-リテラルと-is--is-not)
- [15.4 代入の互換性は「等価」ではない](#154-代入の互換性は等価ではない)
- [15.5 絞り込み（narrowing）](#155-絞り込みnarrowing)
- [15.6 未初期化のフィールドを型で塞ぐ](#156-未初期化のフィールドを型で塞ぐ)
- [15.7 s[i] の速度：測ってから決める](#157-si-の速度測ってから決める)
- [15.8 動作確認](#158-動作確認)
- [15.9 まとめと次章の予告](#159-まとめと次章の予告)

---

## 15.1 棚卸し：何が足りないか

### 📖 チェックリストを見る

[design/self-hosting.md](../design/self-hosting.md) の 3.6 節に、
**第14章で標準ライブラリを書きながら見つけた穴**を 6 件記録してあります。

| 見つかった穴 | この章での判断 |
|---|---|
| **`T \| None` が無い** | **入れる**（この章の主題） |
| `s[i]` が毎回ヒープ確保する | **測ってから決める**（15.7 節） |
| 文字列スライス `s[a:b]` が無い | 入れない（`strings.substr` で足りる。15.7 節で実測） |
| `in` 演算子が無い | 入れない（`strings.contains` で足りる） |
| 利用者定義のジェネリクス | 入れない（ハンドル方式で足りる。14.6 節） |
| 複数戻り値 | 入れない（クラスを返す） |

**★ 「入れない」と判断したものにも理由を書きます。**
書いておかないと、同じ議論を何度もやることになります。

### 📖 この章で作るもの

```python
t: Token | None = find()      # ① nullable な型
if t is None:                 # ② None リテラルと is / is not
    return 0
xs: list[Token | None] = []   # ③ 型の一部としても使える

if t is not None:
    print(t.kind)             # ④ 絞り込み（narrowing）

class Node:
    next: Node | None         # ⑤ フィールドにも
```

さらに、**第12章から持ち越した宿題**を型の側から塞ぎます（15.6 節）。

---

## 15.2 T | None を型に足す

### 🤔 判断：フラグではなく「別の種類」にする

[type-system.md](../spec/type-system.md) 2 節には、当初こう書いてありました。

```c
struct Type {
    TypeKind kind;
    ...
    bool nullable;     // T | None なら true
};
```

**この設計をやめます。** 理由は 1 つです。

```c
// 既存のコードはあちこちでこう書いています
if (t->kind == TY_CLASS) { ... t->cls ... }     // ← Token | None も通ってしまう
```

フラグ方式だと、**`Token | None` が「クラス型」として既存の判定をすり抜けます。**
すり抜けた先でフィールドを読めば、`None` のときに壊れます。
**判定を書き忘れた場所が、そのまま実行時のクラッシュになります。**

種類（`TypeKind`）を分けると、逆になります。

```c
typedef enum {
    TY_INT, TY_BOOL, TY_NONE, TY_STR, TY_LIST, TY_CLASS,
    TY_OPT,   // T | None（第15章）→ ptr。elem が中身の型
    TY_NULL,  // None リテラルの型（第15章）。変数の型にはならない
} TypeKind;
```

**書き忘れた場所は「型 'Token | None' にフィールドはありません」と言って止まります。**

> **★ 「書き忘れたときにどうなるか」で設計を選ぶ。**
> 正しく書いたときの動作は、どちらの設計でも同じです。
> 違うのは**間違えたとき**で、そこが設計の質を決めます。

### ✍️ 型を作る

```c
// T | None を作る（第15章）。
// ★ 同じ T に対しては 1 個だけ作ります（キャッシュ）。
Type *type_opt(Type *elem);
```

```c
bool type_equal(Type *a, Type *b) {
    ...
    if (a->kind == TY_OPT) return type_equal(a->elem, b->elem);   // ★ 追加
    ...
}
```

**⚠️ `type_equal` を触るのは 4 度目です**（ch5 → ch10 → ch12 → ch15）。
型の種類が増えるたび、ここに 1 行ずつ足しています。

### ⚠️ nullable にできるのは参照型だけ

```python
n: int | None = None       # ✗ エラー
```

```
error: 'int | None' は書けません
   = ヒント: None はポインタとして表すので、int や bool には付けられません
             （nullable にできるのは str / list[T] / class です）
```

**🤔 なぜ禁止するのか**

`Token | None` の実体は「ポインタ、ただし NULL かもしれない」です。
`int` は `i64` なので、NULL に相当する値がありません。
表現できるようにするには**箱に入れる（boxing）**必要があり、
そこから先は「`int` が値か参照か」という話が始まります。

**v1 では「`None` はヌルポインタ」という 1 つの表現で通します。**

---

## 15.3 None リテラルと is / is not

### ✍️ None は式にもなる

第9章から `None` は「戻り型」として書けました（`-> None`）。
今回から**式**としても書けます。

```python
t: Token | None = None
```

| 書き方 | 意味 | 型 |
|---|---|---|
| `def f() -> None:` | 値を返さない | `TY_NONE`（void） |
| `x = None` | ヌルポインタという**値** | `TY_NULL` |

**⚠️ この 2 つは別物です。** Python では同じ `None` ですが、
Mython では「値を返さない」を `void` にマップしているので分けます。

```
error: 値を返さない関数では 'return' とだけ書きます
 3 |     return None
   |            ^^^^ ここに値は書けません
```

### ✍️ is / is not は None 専用

```python
if t is None:      ...
if t is not None:  ...
```

```
error: is は None との比較にだけ使えます
 5 |     if a is b:
   |            ^ ここには None を書いてください
   = ヒント: 値が等しいかを調べるには == を使ってください
```

**🤔 なぜ一般の `is`（同一性比較）にしないのか**

Python の `is` は「同じオブジェクトか」です。Mython でも
クラスの `==` が既に参照比較なので（第12章）、**`is` を一般化すると
`==` と区別がつかなくなります。**

**★ 記号を増やすときは「既にあるものとの違い」を言えるかを確かめます。**
言えないなら、増やす価値はありません。

### 📖 コード生成は 1 命令

```llvm
%t1 = icmp ne ptr %t0, null      ; t is not None
```

**★ 第12章で入れた `my_check_not_none` は、これで要らなくなる……
とはいきません**（15.6 節）。

---

## 15.4 代入の互換性は「等価」ではない

### 📖 一方向だけ許す

```
assignable(S → T) =
    type_equal(S, T)                              (a) 完全一致
    または (T が T2|None で、S が TY_NULL)          (b) None を入れる
    または (T が T2|None で、assignable(S → T2))    (c) T2 を入れる（広げる）
```

```python
t: Token | None = None            # (b) OK
t = Token(1, "x")                 # (c) OK — 狭い型を広い型に入れる
u: Token = t                      # ✗ エラー（広い型は狭い型に入らない）
```

```
error: 型が一致しません
 6 |     u: Token = t
   |                ^ 型 'Token | None' の式
note: 変数 'u' は 'Token' 型として宣言されています
   = ヒント: None かもしれない値です。'if t is not None:' の中で使ってください
```

### ⚠️ `type_equal` を使っている場所を全部見直す

第5章から、代入・引数・戻り値・`append`・フィールドへの代入は
**すべて `type_equal` で検査してきました。**
それらは全部「代入できるか」の検査なので、`type_assignable` に置き換えます。

| 場所 | 変更後 |
|---|---|
| 変数宣言・代入 | `type_assignable(実際, 宣言)` |
| 関数・メソッドの引数 | `type_assignable(実引数, 仮引数)` |
| `return` | `type_assignable(式, 戻り型)` |
| `list.append` / `xs[i] = v` | `type_assignable(値, 要素型)` |
| フィールドへの代入 | `type_assignable(値, フィールド型)` |
| **リストリテラルの要素** | 期待型があればそれに対して |

**★ 「等価」と「代入できる」を分けるのは、部分型を入れた言語の宿命です。**
Mython の部分型は `T <: T | None` の 1 種類だけですが、
**1 種類でも入れた瞬間に、この分離が必要になります。**

---

## 15.5 絞り込み（narrowing）

### 📖 「変数の型は 1 つ」という前提が崩れる

第5章からずっと、変数の型は宣言のときに決まって変わりませんでした。

```python
t: Token | None = find()
if t is not None:
    print(t.kind)      # ★ ここでだけ t は Token
```

**この機能だけは、第12章で章を分けてまで後回しにしました**（ch12 12.1 節）。
「変数の型は 1 つ」を崩すからです。

### ✍️ 実装：スコープの変数の型を一時的に上書きする

```c
struct VarEntry {
    char *name;
    char *ir_name;
    Type *declared;   // ★ 宣言された型（Token | None）
    Type *type;       // ★ 今の型（絞り込まれていれば Token）
    ...
};
```

```c
// if の条件から「絞り込める変数」を集める
//   t is not None            → t を絞る
//   a is not None and b is not None → 両方絞る（and でつながっていれば）
static void narrow_from_cond(Sema *s, Node *cond, NarrowSave *saved);
```

- `then` 節に入る前に `v->type = elem` にする
- 節を抜けたら**必ず元に戻す**
- `while` の条件も同じ（本体の中で絞られる）

**★ 「入る前に変えて、抜けたら戻す」は第7章のスコープと同じ形です。**
C の呼び出しスタックがそのまま「絞り込みのスタック」になります。

### ⚠️ 代入したら絞り込みは解除される

```python
cur: Node | None = head
while cur is not None:
    print(cur.v)        # ここでは Node
    cur = cur.next      # ★ 代入した瞬間、cur は Node | None に戻る
    print(cur.v)        # ✗ エラー：None かもしれない
```

**これは正しい動作です。** `cur.next` は `Node | None` なので、
代入後の `cur` が `None` である可能性は本当にあります。

```c
// check_assign の最後
v->type = v->declared;    // ★ 絞り込みを解除する
```

**★ 3 行で「代入は絞り込みを壊す」を表現できます。**
本格的なフロー解析（各文の前後で型環境を計算する）を書かずに、
**「代入したら忘れる」という保守的な近似**で済ませています。

### 📖 v1 で絞り込めないもの

| 形 | 絞り込むか | 理由 |
|---|---|---|
| `if t is not None:` | ✅ | |
| `while t is not None:` | ✅ | 連結リストの走査に必須 |
| `if a is not None and b is not None:` | ✅ | `and` の枝をたどるだけ |
| `if t is None:` の **else 節** | ✅ | 反対側を絞る |
| `if t is None: return` の**後ろ**（ガード節） | ✅ | 第8章の `always_returns` を再利用する（15.8 節の「予想が外れたこと」参照） |
| `if a is not None or ...` | ❌ | `or` では両方が保証されない |
| `self.next is not None` | ❌ | **フィールドは絞らない**（次項） |
| グローバル変数 | ❌ | 呼んだ関数の中で変えられるかもしれない |

### ⚠️ フィールドを絞らないのはなぜか

```python
if node.next is not None:
    print(node.next.v)      # ✗ エラー
```

```
   = ヒント: 一度ローカル変数に入れてから絞り込んでください:
             nxt: Node | None = node.next
             if nxt is not None:
                 print(nxt.v)
```

`node.next` を絞ると、**その間に `node.next` が書き換わらないことを
保証しなければなりません。** メソッド呼び出し 1 つで壊れます。
**v1 では「絞れるのはローカル変数だけ」に限定します。**

---

## 15.6 未初期化のフィールドを型で塞ぐ

### 📖 第12章から持ち越した宿題

```python
class Node:
    v: int
    next: Node      # ← 既定値を作れないので NULL から始まる（ch12 12.6）
```

第12章では**ランタイムで検査**しました（`my_check_not_none`）。
「壊れる前に止まる」だけで、**コンパイル時には何も言えていません。**

### ✍️ 判断：非 nullable なクラス型フィールドは init で代入させる

```
error: フィールド 'next' は init で代入されていません
 3 |     next: Node
   |     ^^^^ このフィールドは None から始まってしまいます
note: init はここです
 5 |     def init(self, v: int) -> None:
   = ヒント: 次のどちらかにしてください:
             ・型を 'Node | None' にする
             ・init の中で self.next = ... と代入する
```

`init` を持たないクラスも同じです（そもそも代入する場所がない）。

**★ これで「クラス型のフィールドは必ず有効な値から始まる」が
コンパイル時にほぼ保証されます。**

### ⚠️ 「ほぼ」の中身：この検査は構文的です

```python
def init(self, ok: bool) -> None:
    if ok:
        self.next = ...      # ★ 条件つきの代入も「代入あり」と数える
```

代入が**実行されるか**までは見ていません（フロー解析が要る）。
だから**ランタイムの検査（`my_check_not_none`）は残します。**

| 層 | 何を捕まえるか |
|---|---|
| 型検査（この章） | 代入が**どこにも無い**場合 → コンパイルエラー |
| ランタイム（第12章） | 条件つき代入がすり抜けた場合 → 親切なメッセージで停止 |

**★ 静的検査と動的検査は、どちらかを選ぶものではありません。**
静的検査で**多くを早く**捕まえ、残りを動的検査で**安全に**受け止めます。

---

## 15.7 s[i] の速度：測ってから決める

### 📖 記録された疑い

第14章の 3.6 節にこう書きました。

> **`s[i]` が毎回ヒープ確保する**（`my_str_index` が 2 バイト確保する）。
> 字句解析器は文字単位で回るので効く。

**疑いのままにせず、測ります。**

### ✍️ 比べる相手を用意する（コンパイラは変更しない）

第14章で `extern` を作ってあるので、**ライブラリに 2 行足すだけ**で済みます。

```python
# lib/strings.my
extern def my_byte_at(s: str, i: int) -> int   # ★ 確保しない。i 番目のバイトを返す

def byte_at(s: str, i: int) -> int:
    return my_byte_at(s, i)
```

**★ 「言語に足す」のではなく「ライブラリに足す」で済みました。**
第14章で引いた境界線（14.1 節）が、そのまま働いています。

### ⚠️ 測ったら、疑っていた原因は主犯ではなかった

200,000 バイトのファイルを読み、全文字を走査します。

```python
def scan_index(s: str) -> int:      # s[i] で走査（1 文字ごとに str を確保）
def scan_byte(s: str) -> int:       # strings.byte_at(s, i) で走査（確保しない）
```

| | `s[i]` で走査 | `byte_at` で走査 |
|---|---|---|
| 実測 | **1.54 秒** | **0.94 秒** |

**確保をやめても 0.94 秒かかっています。** 確保は主犯ではありませんでした。

**真犯人は `strlen` です。**

```c
long long my_str_len(const char *s) { return (long long)strlen(s); }   // O(n)

long long my_byte_at(const char *s, long long i) {
    long long n = (long long)strlen(s);     // ★ 1 回のアクセスごとに全部走る
    ...
}
```

`str` が「ただの NUL 終端文字列」だったので、**長さを知るのに毎回全部走っていました。**
1 文字読むたびに O(n) ——**字句解析は O(n²)** です。
120KB のソースを読むだけで数秒かかる計算で、**セルフホストできません。**

### ✍️ 判断：str の表現に「長さ」を持たせる

```
[ i64 長さ ][ バイト列 ... ][ '\0' ]
             ^ str の「値」が指すのはここ
```

- `my_str_len(s)` は `((long long *)s)[-1]` ——**O(1)**
- **値が指すのはデータの先頭のまま**なので、C から見れば今までどおり
  NUL 終端の `char *`。`extern` にそのまま渡せます
- 文字列リテラルも同じ形で出します

```llvm
@.str.0 = private unnamed_addr constant { i64, [6 x i8] } { i64 5, [6 x i8] c"hello\00" }

; 使うときはデータ部を指す定数式
call void @my_print_str(
    ptr getelementptr inbounds ({ i64, [6 x i8] }, ptr @.str.0, i32 0, i32 1))
```

**⚠️ `extern` が返す `str` も「長さ付き」でなければなりません。**
`argv` のような素の C 文字列は `my_str_from_cstr()` で作り直します。
**境界を広げると、境界のルールが増えます**（第14章 14.2 節の続き）。

### ✍️ もう 1 つ：連結が O(n²)

```python
out: str = ""
for line in lines:
    out = out + line      # ★ 毎回すべてコピーする
```

セルフホストの IR 出力は**何万行も組み立てます**。ここが O(n²) だと終わりません。
`my_str_join` をランタイムに置き、`strings.join` から呼びます。

```python
# Mython での文字列の組み立て方（第16章以降はこの形で書く）
parts: list[str] = []
parts.append("...")
out: str = strings.join(parts, "")     # ★ ここで 1 回だけ確保する
```

**結果は 15.8 節に載せます。**

---

## 15.8 動作確認

**以下はすべて実際に実行した結果です。**

### ✅ テスト全実行

```
全 312 件パス
```

20 件追加し、5 件を書き換えました（15.6 節の検査で書けなくなったため）。
ビルド警告 0 件、ASan/UBSan も 312 ケースすべてで検出 0 件。

### ✅ ゴールのプログラム（連結リストの走査）

```python
cur: Node | None = a
while cur is not None:
    print(cur.v)
    cur = cur.next
```

```
1
2
```

**`while` の条件で絞り込み、`cur = cur.next` で解除される**——
この 2 つが噛み合わないと、連結リストは書けません。

### ✅ 生成される IR

```llvm
%t8 = icmp ne ptr %t7, null        ; cur is not None
```

**`is` / `is not` は 1 命令です。** `None` は `null` という即値なので、
命令すら出ません。

ガード節（`if b is None: return 0`）の場合：

```llvm
define i64 @g.value(ptr %b.arg) {
entry:
  %t0 = load ptr, ptr %b
  %t1 = icmp eq ptr %t0, null
  br i1 %t1, label %if.then.0, label %if.end.0
if.then.0:
  ret i64 0
if.end.0:
  %t2 = load ptr, ptr %b
  %t3 = call ptr @my_check_not_none(ptr %t2)   ← ★ 検査は残っている
  %t4 = getelementptr %g.Box.type, ptr %t3, i32 0, i32 0
  %t5 = load i64, ptr %t4
  ret i64 %t5
}
```

**⚠️ 絞り込んでも `my_check_not_none` は消えません。**
型が保証したのは「**変数** `b` が `None` でない」ことで、
「クラスのフィールドが `init` されている」ことではないからです（15.6 節）。
**この 2 つは別の保証です。**

### ★ 文字列の表現を変えた効果（実測）

200,000 バイトのファイルを 1 文字ずつ走査：

| | 長さが O(n)（旧表現） | 長さが O(1)（新表現） |
|---|---|---|
| `s[i]` で走査 | 1.54 秒 | **0.53 秒** |
| `strings.byte_at` で走査 | 0.94 秒 | **0.00 秒**（測定限界以下） |

**★ 2 つの原因がそれぞれ効いていたことが、表で見えます。**

- 縦に見る（`s[i]` → `byte_at`）… **確保をやめた**効果
- 横に見る（旧 → 新）… **長さを O(1) にした**効果

**⚠️ 疑っていた原因（確保）だけを直していたら、0.94 秒で止まっていました。**
測らずに直していたら、「対策したのに遅い」と悩むことになります。

> **★ 「遅い」と思ったら、まず測る。次に、直したあとにもう一度測る。**

生成された文字列リテラル：

```llvm
@.str.0 = private unnamed_addr constant { i64, [1 x i8] } { i64 0, [1 x i8] c"\00" }
@.str.1 = private unnamed_addr constant { i64, [2 x i8] } { i64 1, [2 x i8] c" \00" }
```

### ✅ 型エラーの診断

**絞り込まずに触った**（いちばんよく出るエラー）：

```
error: 型 'Box | None' の値にはフィールドがありません
 10 |     print(cur.v)
    |               ^ None かもしれない値です
note: この式は 'Box | None' 型です
    = ヒント: 先に None を除いてください:
             if cur is not None:
                 ...
```

**絞り込みが代入で解除されることも、エラーで分かります：**

```python
if cur is not None:
    cur = cur.next
    print(cur.v)      # ← エラー
```

**フィールドは絞り込まない：**

```
error: 型 'Node | None' の値にはフィールドがありません
    = ヒント: 一度ローカル変数に入れてから絞り込んでください:
             x: T | None = ...
             if x is not None:
                 ...
```

**`int | None` は書けない：**

```
error: 'int | None' は書けません
    = ヒント: None はヌルポインタとして表すので、int や bool には付けられません
             （nullable にできるのは str / list[T] / class です）
```

**`is` の右辺は None だけ：**

```
error: is は None との比較にだけ使えます
    = ヒント: 値が等しいかを調べるには == を使ってください
```

### ✅ 第12章の宿題（未初期化フィールド）

```
error: フィールド 'next' は init で代入されていません
 3 |     next: Node
   |     ^^^^ このフィールドは None から始まってしまいます
note: このクラスには init がありません
   = ヒント: 次のどちらかにしてください:
             ・型を 'Node | None' にする
             ・init の中で self.next = ... と代入する
```

**★ この検査を入れたら、既存のテストが 5 件書けなくなりました。**
`class_field_chain` / `class_mutual` / `rt_field_none` / `rt_method_on_none` /
`mod_class_across` ——全部「クラス型のフィールドを持つクラス」です。

**書き換えた結果、すべて `T | None` を使う形になりました。**
第12章のとき「本当は `T | None` が要る」と書いたのは、正しかったわけです。

**⚠️ すり抜けは残ります**（15.6 節）。

```python
def init(self, ok: bool) -> None:
    self.v = 1
    if ok:
        self.next = self      # ★ 条件つきの代入は「代入あり」と数える
```

```bash
$ ./t                      # N(False) で作って a.next.v を読む
runtime error: field access on None (uninitialized reference field?)
$ echo $?
1
```

**静的検査が多くを早く捕まえ、残りを動的検査が安全に受け止めています。**

---

## 15.9 まとめと次章の予告

### できたこと

```
✅ T | None（TY_OPT）— フラグではなく「別の種類」にした
✅ None リテラル（TY_NULL）と is / is not
✅ 代入互換性（type_assignable）を type_equal から分離
✅ 絞り込み（narrowing）: if / while / and / else / ガード節（early return）
✅ 代入すると絞り込みは解除される（保守的な近似）
✅ 未初期化のクラス型フィールドをコンパイル時に弾く（第12章からの宿題）
✅ str の表現を「長さ付き」に変更 — len と byte_at が O(1) に
✅ strings.byte_at / my_str_join — 実測にもとづく追加
✅ セルフホストのチェックリストの判断がすべて終わった
✅ テスト 312 件全パス、警告 0、ASan/UBSan クリーン
```

### 触ったファイル

| ファイル | 変更 |
|---|---|
| `src/types.h` / `types.c` | `TY_OPT` / `TY_NULL`、`type_opt`、**`type_assignable`** |
| `src/parser.c` | `T \| None`、`None` リテラル、`is` / `is not` |
| `src/sema.c` | 絞り込み（`NarrowSet`）、`declared` と `type` の分離、未初期化フィールドの検査 |
| `src/codegen.c` | `icmp ... null`、`null` 即値、長さ付き文字列リテラル |
| `runtime/runtime.c` | `my_str_alloc` / `my_str_from_cstr` / `my_byte_at` / `my_str_join`、str 全般 |
| `lib/strings.my` | `byte_at` を使う書き換え、`join` をランタイムへ |
| `tests/` | 20 件追加・**5 件書き換え** |
| `docs/spec/type-system.md` | nullable の設計を「フラグ」から「別の種類」に改訂 |

### この章で回収された設計判断

| 投資した章 | 内容 | この章での回収 |
|---|---|---|
| ch5 | 「変数の型は 1 つ」 | **ここで初めて崩した**（崩す場所を 1 か所に閉じ込めた） |
| ch7 | ブロックスコープ（入る前・抜けた後） | 絞り込みの「入る前に変えて、抜けたら戻す」がそのまま同じ形 |
| ch8 | `always_returns`（全経路 return の検査） | **ガード節の絞り込みにそのまま再利用** |
| ch9 | `str` は NUL 終端の C 文字列 | 表現を変えても**値が指すのはデータ先頭**にしたので extern が無傷 |
| ch10 | `type_equal` の再帰比較 | `T \| None` にも同じ形で 1 行足すだけ |
| ch10 | 期待型（`s->expected`） | `[Box(1), None]` を `list[Box \| None]` として読める |
| ch12 | ランタイムの NULL 検査 | **静的検査と二段構えになった**（どちらも残す） |
| ch12 | 「章を分ける」判断 | 分けたおかげで、この章では narrowing のバグだけを疑えた |
| ch14 | `extern`（言語を変えずに機能を足す） | `byte_at` / `my_str_join` を**言語に触らず**足せた |
| ch14 | ライブラリを Mython で書いて穴を見つけた | **この章の作業リストがそこから来た** |

### 判断したこと（と理由）

| 判断 | 理由 |
|---|---|
| **nullable はフラグではなく別の種類（`TY_OPT`）** | 「書き忘れたときにどうなるか」で選ぶ。フラグは既存の判定をすり抜けて実行時に壊れる |
| **nullable にできるのは参照型だけ** | `None` はヌルポインタ。`int` を nullable にすると箱詰めの話が始まる |
| **`is` / `is not` は None 専用** | クラスの `==` が既に参照比較。区別を説明できない記号は増やさない |
| **絞り込めるのはローカル変数だけ** | グローバルは呼んだ関数が書き換えうる。フィールドは「その間変わらない」保証ができない |
| **代入したら絞り込みを解除する** | フロー解析の代わりの保守的な近似。3 行で書けて、連結リストの走査が正しく通る |
| **ガード節（early return）も絞り込む** | いちばん自然な書き方なので、これが無いと `T \| None` が使いにくい。第8章の `always_returns` を再利用するだけ |
| **未初期化フィールドの検査は「構文的」に留める** | 実行されるかまで見るにはフロー解析が要る。静的で多くを捕まえ、残りはランタイムに任せる |
| **str に長さを持たせる**（表現の変更） | 実測で「1 文字アクセスが O(n)」だと分かったから。**セルフホストの前提が崩れる** |
| **`join` をランタイムに置く** | 文字列の組み立ては IR 出力で何万回も走る。`list[str]` に溜めて最後に `join` を定石にする |

### ⚠️ 予想が外れたこと

**① `bool nullable` フラグでいくつもりだった。**

[type-system.md](../spec/type-system.md) にそう書いてありました。
実装直前に「書き忘れたときにどうなるか」を考えて、**別の種類**に変えました。
**仕様のほうを直しました。**

**② 遅さの原因を読み違えていた。**

第14章で「`s[i]` が毎回ヒープ確保するのが問題」と記録していました。
測ったら、**確保をやめても 4 割しか速くなりませんでした**。
真犯人は `strlen`（長さの取得が O(n)）で、**気づいてすらいませんでした**。

**★ 記録した疑いは正しいとは限りません。だから測ります。**

**③ 早期 return の絞り込みは「入れない」と書いていた。**

15.5 節の表には最初 ❌ と書いていました。しかしテストを書いた瞬間に
`if b is None: return 0` が通らず、**いちばん自然な書き方**だと分かりました。
第8章の `always_returns` を再利用して 10 行で入りました。

**★ 「使ってみて初めて必要だと分かる」は、この章で 3 回目です。**

### 既知の課題（第16章以降で判断する）

| 課題 | どうするか |
|---|---|
| `or` の絞り込み | ✅ **第20章で一部を解決**（20.7 節。「成り立たない側」だけはド・モルガンで絞れる） |
| フィールドの絞り込み | 入れない（不変性を保証できない） |
| `strings.substr` が O(n²) | トークンの切り出しは短いので実害なし。第20章の後に測って判断 |
| `dict.Dict` の線形探索 | 同上 |
| `my_check_not_none` の呼び出しが残る | 絞り込み済みの変数なら消せるが、効果を測ってから |

### ✍️ commit する

```bash
git add -A
git commit -m "第15章: セルフホスト準備（T | None と棚卸し）"
```

---

## 次章：第16章 Mython で字句解析器を書く

**ここから第V部（セルフホスト）です。** 作るものが変わります。

```
これまで： C で Mython コンパイラを書く（src/*.c）
ここから： Mython で Mython コンパイラを書く（selfhost/*.my）
```

**達成目標**

```bash
$ ./build/mythonc selfhost/lexer.my -o lexer_my
$ ./lexer_my tests/cases/int_42.my > mine.txt
$ ./build/mythonc --dump-tokens tests/cases/int_42.my > theirs.txt
$ diff mine.txt theirs.txt        # ★ 完全一致すること
```

**★ C 版が「正解」を持っているのが、この部の強みです。**
自分の出力と比べられるので、どこで間違えたかが必ず分かります。

**やること**

| ファイル | 作業 |
|---|---|
| `selfhost/lexer.my` | 字句解析器（`src/lexer.c` の移植） |
| `selfhost/token.my` | `Token` クラスと種類の定数 |
| `tests/` | C 版と Mython 版のトークン列を比較する仕組み |

**⚠️ 予想される落とし穴**

- **機械的に移植する。工夫しない**（`self-hosting.md` 6 節）。
  アルゴリズムを変えると、出力が違ったときに原因を切り分けられません
- 1 文字ずつ回るところは **`strings.byte_at`** を使う（15.7 節の実測）
- 文字列の組み立ては **`list[str]` に溜めて `join`**（同上）
- `T | None` を使う場面が一気に増えます（トークンの `next`、エラーの有無）
- インデント処理（第4章）は状態を持つので、移植で最も難しい部分

### 🤔 第16章に入る前の練習問題

1. `narrow_restore()` の呼び出しを 1 か所コメントアウトして、
   `if` の外でも絞り込みが残ることを確かめる（**必ず元に戻す**）
2. `check_assign` の `v->type = v->declared;`（絞り込みの解除）を消して、
   `nullable_linked_list.my` が**通ってしまう**ことを確かめる。
   なぜそれが危険か説明する
3. `my_str_len` を `strlen` に戻して、15.8 節のベンチマークを走らせる
4. `type_assignable` を `type_equal` に戻すと、どのテストが落ちるか予想してから試す
5. **自分で `T | None` を使う小さなプログラムを書く**（二分木の探索など）。
   絞り込みが効かなくて困る場面を 1 つ見つけ、なぜ効かないかを説明する
