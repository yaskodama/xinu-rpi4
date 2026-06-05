# Pi 5 実機ブリングアップ — ハンドオフ (2026-06-04)

## ゴール
`make pi5` (BCM2712 / Cortex-A76 / kernel_2712.img) を**実機 Raspberry Pi 5** で動かす。
これまで実機作業は全て Pi 4 で、Pi 5 ターゲットは一度も実機検証されていなかった。

## このセッションで作ったもの (xinu-rpi4 リポ、未コミット)
- `firstlight/firstlight.c` — 最小 first-light カーネル本体（自己完結、video/net/scheduler 不使用）
- `compile/Makefile` — `make pi5-min` ターゲット追加（`firstlight.o + boot.o + mbox.o + font8x8.o`
  → `kernel_xinu.img` 約8200B）。COMPONENTS 外の `firstlight/` に置くことでフルカーネルの
  `kernel_main` 二重定義を回避。
- `sdcard/config_pi5min.txt`  … 専用デバッグ UART (0x107D001000) + HDMI ヒント
- `sdcard/config_pi5gpio.txt`  … GPIO14/15 = RP1 UART0 (0x1F00030000) + pciex4_reset=0 + HDMI
- `sdcard/config_linux_serial.txt` … Linux + シリアル検証用（kernel= 無し→Linux 起動）

⚠️ **現在 firstlight.c はディスク上「GPIO/RP1 UART 版(0x1F00030000)」になっている。**
次セッションで専用 UART 路線に進むなら、SERIAL_BASE を **0x107D001000** に戻すこと
(最初のバージョン＝専用デバッグ UART、PCIe 不要)。

## firstlight.c の構造（2 チャネル）
1. HDMI（mailbox FB, 0x107C00B880）で画面全体を **赤→緑→青** と塗り替え→banner。
   静的 firmware 画面は色が変わらないので「色が流れたら=カーネル稼働+FB OK」の生存証拠。
2. シリアル banner + エコー。

## 重要な技術メモ
- **Pi 5 のシリアルは Pi 4 と全く違う**:
  - GPIO14/15 (pin8/10) = RP1 UART0 @ **0x1F00030000**。PCIe 背後。`pciex4_reset=0` を
    config に入れないとアクセスで data abort。`dtparam=uart0=on` 必要。
  - 専用 3ピン JST-SH デバッグコネクタ(micro-HDMI の間) = BCM2712 内蔵デバッグ UART
    @ **0x107D001000**。PCIe 不要・firmware 起動時から常時 115200 出力。
    Linux の `earlycon=pl011,0x107d001000,115200n8` がこれ。**ブリングアップの本命**。
- 専用コネクタ配線(公式 RP-003139): **①RX ②GND ③TX**(GND が真ん中)。3.3V 失敗安全。
- HDMI: Pi 5 firmware FB は depth バグあり→`framebuffer_depth=32` 必須。KMS(vc4-kms-v3d)を
  読むと mailbox FB が走査されない(rainbow→black)ので **no-KMS** にする。mask は `&0x3FFFFFFF`(Pi4 と同)。
- ★イメージヘッダ(boot.S の arm64 Image header)は Pi 4 で動いた物と同一で正常。

## 実機での結果（全て「何も出ない」）
- 専用 UART 版・GPIO 版とも、HDMI・シリアルどちらにも一度も出力が出なかった。
- ただし **シリアルは観測手段自体が壊れていた**ため、カーネルが動いたかは未確定。

## ★★ シリアル沼の真因（重要・時間を溶かした）
- 手元の USB-シリアル変換器(cu.usbserial-1130/1120)は **macOS 上で baud を変更できない**。
  証拠: python の IOSSIOSPEED で 115200 と 921600 を設定しても**取得バイト数がきっかり同じ
  ~2930B**(8倍違えば8倍変わるはず)。stty も IOSSIOSPEED も無視されている。
  → どう設定しても固定 baud X に張り付き、Pi の baud と一致せず永遠に化ける。
- GPIO14/15 配線の向きは**正しい**(白=pin8/緑=pin10/黒=pin6。緑↔白 入替で受信が 1B に激減
  =元が正しいと確定)。Pi の GPIO14 は起動時に **~0.26秒の短いバースト**を出すが、
  これは**フルの Linux コンソールではない**(短すぎる)。`console=ttyAMA0` にしても変わらず
  =GPIO14/15 に Linux コンソールが乗っていない疑い。
- こちらの TX→Pi RX は応答なし(CR 送出に getty 無反応)。

## 結論と次アクション
**この USB アダプタ + Mac ではシリアル不可(baud 設定不能)。** ユーザは Amazon で
**完成品 USB-UART デバッグアダプタ**を購入予定:
- Waveshare USB to UART Debugger Module for Pi 5 (Amazon.co.jp **B0CW12C4XX** / B0CYLPJBQQ、
  SH1.0 3PIN ケーブル同梱、Mac 対応) — または **Raspberry Pi 公式 Debug Probe**(RP2040、
  macOS で usbmodem・baud 確実)。
- ❌「3ピンケーブル単品」を今のアダプタに繋ぐのは NG(baud 問題残る)。

**アダプタ到着後の手順:**
1. Pi 5 の 3ピン専用デバッグコネクタに接続。
2. まず **Linux 起動**(config を `config_linux_serial.txt` 系に戻す or kernel= を消す)で
   115200 のブートログが読めるか確認 → 配線・baud・screen の検証。
3. `firstlight.c` の SERIAL_BASE を **0x107D001000** に戻して `make pi5-min` → `kernel_xinu.img`。
   config を `config_pi5min.txt`(専用 UART)にして焼く。
4. Xinu の banner / CurrentEL / エコーが読めれば first-light 成功。以降フル機能へ。

## SD カードの状態
- 61.9GB カード: `/Volumes/bootfs` (FAT32) + Linux (ext4)。Linux 起動は確認済(=HW 正常)。
- バックアップ: `config.txt.linux.bak`, `cmdline.txt.bak` (bootfs 上)。
- `kernel_2712.img` = Linux 本体(温存)。`kernel_xinu.img` = 我々の最小カーネル。
- 現在 cmdline.txt は `console=ttyAMA0,115200 console=serial0,115200 console=tty1 ...`、
  config.txt は Linux+uart0+init_uart_clock(検証用に書き換え済)。
  → Xinu に戻すには config を config_pi5min/gpio に差し替え。

## 観測ツール (Mac, /tmp/)
- `/tmp/serread.py <dev> <baud> <secs> <out>` — ポートを開いたまま baud 適用(IOSSIOSPEED)して録音。
- `/tmp/sersweep.py <dev> <baud...>` — CR 送出して baud スイープ。
- ※ これらは「アダプタが baud を変えられる」前提。今のアダプタでは無力(上記の通り)。

## デバッグ哲学(再確認)
観測手段(シリアル or HDMI)を確実に立てるまでブラインド・フラッシュは収束しない。
今回シリアルが壊れていたため、まともなアダプタが届くまで実機判定は保留。
HDMI 路線(色サイクル)も並行で有効(Mac 不要・画面直視)。

---

## 2026-06-04 続き — HDMI 路線で再開 (firstlight.c 改修, 全て未コミット)

### firstlight.c の変更 (HDMI を主観測チャネルに)
1. **SERIAL_BASE を 0x1F00030000(RP1 UART/PCIe依存) → 0x107D001000(専用デバッグ UART/
   PCIe 非依存) に戻した**。HDMI 路線では config_pi5min(PCIe 起こさない)で焼くため、
   RP1 UART を触ると data abort → カーネル停止 → HDMI も道連れになる。debug UART なら
   ケーブル無しでも触って fault しない。
2. **継続ハートビート追加**: banner 後に無限ループで「点滅する四角 + 増え続けるフレーム
   カウンタ(fb_decimal)」を描画。一発 banner だと「描画後ハング」と「生存」を区別不能
   だが、カウンタが増え続ければ生存ループ確定。シリアル echo も同ループに統合。
3. **★mailbox バスアドレスを 1 起動で複数規約トライ**: device/mbox/mbox.c は生 phys を
   渡すが、u-boot リファレンス(references/uboot-mbox.c)は phys_to_bus 変換(VC4 は SDRAM
   を 0xC0000000 uncached alias にマップ)。Pi 4 は生 phys で HDMI 動作実績ありだが Pi 5
   (BCM2712)の VideoCore ビューは不明。そこで firstlight.c 内に自前 `fl_mbox(buf, bus_or)`
   を置き、fb_init が **0xC0000000 → 0x40000000 → 0x00000000(生)** の順で試し、最初に
   成功した規約で FB 確保。成功した規約は banner に `mbox bus = 0xC/0x4/0x0` と表示
   (= Pi 5 の正解判明)。`fl_mbox` は MBOX_BASE(0x107C00B880, Makefile -D / フォールバック
   定義あり)を直接 poke。mbox.o は PI5MIN_OBJS に残るが firstlight からは未使用。
- ビルド: `cd compile && make pi5-min` → kernel_xinu.img 8200B、md5 **27813a02...**。
  clean build OK(clang LSP の MBOX_BASE 未定義警告は -D を見ないだけの誤検出)。

### config (SD bootfs)
- `config.txt` = `sdcard/config_pi5min.txt`(専用 debug UART 0x107D001000 + HDMI, **PCIe無し**)。
- `config.txt.linux.bak` = 元の Linux+serial 検証用 config(kernel= 無し→Linux 起動)を退避。
  → Linux に戻すには config_pi5min を config.txt.linux.bak で上書き。
- Linux 本体 kernel_2712.img は温存。

### 実機結果 (HDMI)
- **Linux はこの同じモニタ+ケーブル+HDMI0(USB-C 寄り)ポートで映る = HDMI HW/firmware は正常**
  (ユーザ確認)。よって**バグは我々のカーネル側で確定**。
- 我々のカーネル = **完全に No Signal**(最初から最後まで、虹色スプラッシュも出ない)。
- ★論理: Pi で HDMI を出すには VideoCore mailbox で firmware に FB を確保させるしかない
  (bare-metal が直接 HDMI を叩く余地なし)。「No Signal」= FB 確保が成立していない =
  「カーネルが実行されていない」か「mailbox が 3 規約とも失敗」のどちらか。mbox 成功で
  描画先だけ誤りなら画面は黒/ノイズ(=信号あり)になるはずで、No Signal はそれより手前。
- **blind の限界**: HDMI は mailbox 成功が全てなので、No Signal が続く = 観測点ゼロで
  これ以上切り分け不能。今回の 3 規約トライ(md5 27813a02)が外れた場合、HDMI 単独では
  収束しない。

### カードリーダーの落とし穴 (今セッションで時間を溶かした)
- macOS のカードリーダーが**接触不良で頻繁に認識されない/書き込み中に外れる**
  (`diskutil list external` 空、cp が Permission denied 直後にボリューム消失)。
- 対策 = **堅牢版フラッシュ**: `/Volumes/bootfs` が `-w`(書込可)でマウントされるまで待ち、
  cp 後に **md5 検証**(成果物と一致するまで再試行)、一致で eject。最終的に
  17:12:28 に md5 27813a02 で焼き込み成功・検証 OK・eject 済。
- 教訓: マウント直後は不安定。sleep で settle + 書込可チェック + md5 検証が必須。

### 次アクション (どちらか)
1. **観測点の確保が本筋**: 手配済みの完成品 USB-UART デバッグアダプタ(Waveshare
   B0CW12C4XX / 公式 Debug Probe)到着後、専用 debug UART 0x107D001000 で firmware ログ
   →our banner を読む。HDMI の No Signal の真因(カーネル未実行 vs mbox 全滅)もこれで判明。
2. HDMI で粘るなら: (a) Linux 起動に一旦戻して HDMI 健全性を再確認、(b) boot.S が Pi 5 で
   実際に kernel_main に到達しているかを別手段(GPIO/LED 等)で確認、(c) mbox の FB 返却
   アドレスのマスク(現 &0x3FFFFFFF)が Pi 5 で正しいか再検討。ただし全て blind。

### 焼いた最終状態
- SD = kernel_xinu.img(md5 27813a02, 3規約mbox+heartbeat版) + config_pi5min(HDMI/debug UART)。
- 全変更**未コミット**(firstlight/firstlight.c, firstlight/NEXT_SESSION_PI5.md, compile/Makefile
  の pi5-min, sdcard/config_pi5*.txt)。
