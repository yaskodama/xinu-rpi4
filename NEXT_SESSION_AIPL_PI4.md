# Pi 4 を最新 AIPL へ — 再開メモ

最終更新: 2026-09-05

## 0. まずこれ

焼いてあるのは `431da890…`（正典ガイド 10/10 が通った版）。
**手元にビルド済みの `6708b55e…` が未焼き** ―― 連続投入の停止の真因を直したもの。

コミットは `dba9456` まで（**push していない**。実機で未確認のため）。
移植前に戻すならカード上の `kernel8.img.bak-pre-aipl-f66854f0…`。

## 1. 到達点 ―― 10 本すべて正典どおり動いた（2026-09-05）

生の AIPL をそのまま `POST /cc`（カーネル `431da890…`）:

| | 出力 |
|---|---|
| g1 | `hello, AIPL / tick 1 / tick 2` |
| g2 | `twice = 43 / awaited = 20 / v=7` |
| g3 | `ok, left=7 / sold out` |
| g4 | `waiting / got 1 / via select:1` ← **select が Pi 3 / Pi 5 と同じ** |
| g5 | `/api/x/echo` の案内 |
| g6 | `1 / 1 / n = 1` |
| g7 | `fast = 42 / slow(timed out) = 0 / await = 10` ← **wait が効く** |
| g8 | `literal true ok / literal false ok / b = true / not-eq: true` |
| g9 | `got actor / after send / pong` |
| g10 | `fork = 1 / latch = 2` |

**前段は完成**: 10 本すべてで機内 `abcl2c` の翻訳がホストとバイト単位で一致
（`/cc?stage=xlat`、`stkbad=0`）。

## 2. ★ 停止の真因 ―― アクタを kill() で叩き落としていた

決定的な証拠: g1 の出力に**前のプログラムのアクタの行が混ざった**
（`hello, AIPL / tick 1 / tick 2` のあとに `0 / tick 1 / 0`）。
アクタが載せ替えを生き延び、新しい `dispatch` で走っていた。

固まり方（**ping ごと消える**＝割り込みが止まる形）は、
**`irq_save` 区間の最中のアクタを kill した**症状と一致する。アクタは
`vheap_alloc` / `ap_post` の割り込み禁止区間や `aipl_lock` の中に居ることがある。

**Pi 3 の VM でも同じ穴を踏んでいる**（`apps/abcl_program.c` の `abcl_vm_kill_prev`）。
あちらも kill をやめて解決した。**この系統では kill が常に危ない。**

直したもの（`6708b55e…`、未焼き）:
- `ap_recv` が `g_dead` を見ていなかった ―― 待っているアクタは起こしても待ちに
  戻るだけで永久に抜けず、だから `ap_killall` は kill に頼るしかなかった
- `ap_killall`/`ap_reset` を協調的に。印をつけて起こし 3 秒だけ譲り、
  残ったものだけ kill。強制 kill の回数は `/api/aprun` に出る（0 なら健全）

## 3. 潰した仮説（すべて実機のデータで否定）

| 仮説 | 判定 |
|---|---|
| `ap_run` の暴走 | ✗ 5 秒の番犬が発火しない |
| `/cc` ハンドラ自体 | ✗ 同じ生成 C を素の C として投げると通る |
| `abcl2c` の翻訳誤り | ✗ 10 本ホストとバイト一致 |
| JIT→C→JIT の再入 | ✗ 自作 `dispatch` を `cc_future` 経由で呼んで 41 |
| `ap_spawn` の枯渇 | ✗ `spawn_fails=0` |
| アプリ・ワーカーのスタック 16 KB | ✗ 64 KB でも同じ |
| メモリの断片化 | ✗ `/api/mem` で `blocks=2` のまま一定 |
| 位置（何本目か） | ✗ g1 を 6 回続けても無事 |

## 4. 入れた「観測できるようにする」手当て

これが無いと落ちるたびに電源を入れ直すしかなく、何も分からなかった。

- **例外からの復帰**（`system/exception.c`）: `recover_spin` は「タイマが割り込んで
  他プロセスが動く」前提だが、`/cc` 実行中は `g_actor_pump` が立っていて
  `proc_preempt()` が何もしない。`proc_actor_pump_force_clear()` で落とす
- **アプリ・ワーカーの立て直し**（`loader/main.c` の `app_watchdog`）: 単一枠が
  `APP_IDLE` 以外で 25 秒続いたら枠を空にして代わりを立てる。HTTP が自力で戻る
- **`ap_run` の壁時計打ち切り**（5 秒）と `/api/aprun`
- **`/api/mem`**（空き・最大ブロック・断片数）、`/fault`（ESR/FAR/ELR）は元からある
- **`/cc?stage=xlat`**: 翻訳結果だけ返す（板を落とさずに前段を検証できる）
- アクタのスタックを静的な池（16 体）から配る ―― 毎回の getmem/freemem を無くす

## 5. 移植の設計（三機の対照）

- Pi 4 のアクタは **実プロセス**（`ap_spawn`/`ap_send`）。Pi 3 と同じで、Pi 5 だけが協調ポンプ
- したがって `cc_pump_now` は Pi 4 では `ap_run()`（Pi 5 は `cc_pump()`）
  **★ 値ヒープの錠を握ったまま `ap_run()` を呼んではいけない**（`aipl_unlock_all`/`aipl_relock` で挟む）
- Pi 4 は HTTP を専用アプリ・ワーカーで処理し、AIPL/LLM 専用プロセスも持つ。
  そのおかげで `wait()` が自然に通る（Pi 5 は専用プロセスを立てる必要があった）
- `cc/ccpriv.h` と `cc/codegen.c` は Pi 4 と Pi 5 で完全一致。`parse.c`・`abcl2c.c` は移植済み
- **Pi 4 固有の `cc_gfx_*` / `cc_win_*` / `cc_chat` / `cc_llm` は温存してある（消さないこと）**

## ★★ 6. 犯人は g4（select）―― その次に何を投げても死ぬ（2026-09-05 最終）

`6708b55e…`（協調的に畳む版）でも連続投入は直らなかった。切り分けを進めて、
**g4 のあとは g1 でも落ちる**ことが分かった。

```
起動 → g4  ○（waiting / got 1 / via select:1）
     → g1  ×（板ごと死ぬ。ping も消える）
```

逆に **g5 を g4 より先に走らせた回はすべて通っている**（6×g1 → g5 の変種 →
本物の g5 → g2 → g3 → g4 → g6…g10 が全部通った）。
つまり「5 本目だから」でも「g5 だから」でもなく、**g4 が板に何かを残す**。

### g4 だけが持つもの

- **`select`** ―― 隠しフィールド `__sel` と、`m_Worker_job` の先頭に差し込まれる横取り
- `cc_pump_now()`（＝`ap_run()`）の直後に、**同じアクタへ直接 `dispatch()`**

  ```c
  enqueue(w, serve); cc_pump_now();      /* アクタ・プロセスが serve を走らせる */
  v_print(dispatch(w, job, v_int(1)));   /* アプリ・ワーカーが job を直接走らせる */
  ```

  Pi 4 のアクタは**実プロセス**なので、同じアクタのメソッドを
  **二つのプロセスが順に走らせる**形になる。`g_obj[self].f[]` も
  出力捕捉の `g_cap`/`g_caplen` も共有。Pi 5（協調ポンプ・単一スレッド）には無い形。

### 次に試すこと（この順で）

1. g4 の直後に計器を読む（**板は生きているので安全**）:
   `/api/actors` `/api/aprun` `/api/mem` `/ticks` `/jitstats`。
   g1 の直後と比べれば、g4 が残すものが見える
2. g4 から `select` を外した版（`send w.serve()` と `now w.job(1)` だけ）を投げ、
   そのあと g1 を投げる。落ちれば原因は select ではなく
   「pump 直後の同一アクタへの直接 dispatch」
3. 逆に `now w.job(1)` を消して `select` だけ残した版でも同じことをする
4. Pi 4 では `now` を直接 dispatch にせず、`ap_call`（送って返信を待つ）に
   落とすことを検討する。アクタが実プロセスなのだから、そちらが素直
   ―― Pi 3 も実プロセスだが、あちらは `.avm` VM が継続分割で受けている

## 7. 次の一手（全体）

1. `6708b55e…` を焼いて、10 本の**連続投入**が通ることを確かめる
   （`/api/aprun` の「強制 kill」が 0 なら協調で畳めている）
2. 回帰: `/compile` に素の C、`/actor/load`、`/llm`、`/chat`、`/smp-bench`
3. `web_expose` した先へ `/api/x/echo?method=say&args=hi` が届くか
   （Pi 4 の `/cc` は既定で `cc_run_source`。常駐は `?resident=1`）
4. `ai_call` を Pi 4 で（`v_ai_call` は移植済み。モデルは元から焼いてある）
5. 通ったらコミットを push し、ガイド第39章に Pi 4 の節を足す

## 7. 三機の到達点

| | Pi 3 | Pi 4 | Pi 5 |
|---|---|---|---|
| カーネル | `820388da…` | `431da890…`（`6708b55e…` 未焼き） | `fc71e715…` |
| 正典ガイド | 9/10 一致（g9 は順序） | **10/10 一致**（ただし g4 の次が落ちる） | **10/10 一致** |
| 生の AIPL | Mac で `.avm` に | `POST /cc` | `POST /cc` |
| 出力を読む | `/api/console` | 応答に出る | 応答に出る |
| `b = true` | ○ | ○ | ○ |
