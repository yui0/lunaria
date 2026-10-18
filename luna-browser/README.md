# luna-browser

luna-ui がレイアウトと描画を、QuickJS がスクリプトを担当する小さなブラウザ。

```
make                 ウィンドウ版(GLFW があれば)+ ヘッドレス各モード
make LB_WINDOW=0     ヘッドレスのみ
make LB_NO_TLS=1     OpenSSL なし(https 不可)
make LB_NO_ZLIB=1    zlib なし(gzip/deflate 不可)
make test            ヘッドレス自己テスト
```

## 使い方

| | |
|---|---|
| `luna-browser URL` | ウィンドウで開く(タブ・アドレスバー・戻る/進む/再読み込み) |
| `--screenshot=FILE URL` | 読み込んで PNG を保存して終了 |
| `--dump-dom URL` | DOM を出力して終了 |
| `--remote-debugging-pipe` | CDP をパイプ(fd 3/4)で提供 |
| `--window-size=W,H` `--user-agent=UA` `--user-data-dir=DIR` `-v` | |

## ウィンドウ版の操作

- タブ: `+` / `×` / クリックで切替、Ctrl+T 新規、Ctrl+W 閉じる、Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+PgUp/PgDn 切替、Ctrl+1〜9
- **PC** ボタン: UA をデスクトップ版に切り替えて再読み込み(再度押すとモバイル版)
- 右クリックメニュー: リンク(新しいタブで開く / アドレスをコピー)、画像、選択範囲(コピー / 検索)、
  入力欄(切り取り / コピー / 貼り付け / すべて選択)、戻る / 進む / 再読み込み / PC版切替
- Ctrl+L / F6 アドレスバー、Ctrl+R / F5 再読み込み、Alt+←/→ 戻る/進む、Ctrl+F ページ内検索、Ctrl+±/0 ズーム
- 新しいタブはスタートページ(検索ボックスとよく行くサイト)。アドレスバーに入力した語句は Bing で検索(`LUNA_BROWSER_SEARCH` で変更可)、`localhost` や IP は http で開く
- 読み込み中はタブとアドレスバー横にスピナー、ページ中央に走る猫

## プログラムからのスクリーンショット(lunaria と同じ方式)

```
LUNA_BROWSER_DUMP_DIR=/tmp/shots LUNA_BROWSER_SCREENSHOT_EVERY=60 luna-browser URL
touch /tmp/luna-browser-shot      # 1 枚撮る(LUNA_BROWSER_SHOT_TRIGGER で変更可)
```

`luna-browser_0000.png` のように保存される(ページ部分、PNG)。F12 は `~/Pictures/Luna Browser 日時.png`。

## 環境変数

`LUNA_BROWSER_VERBOSE=1`(= `-v`): ネットワークと各スクリプトの所要時間、遅いタイマー(150ms 超)を stderr に出す。
`LUNA_BROWSER_SCRIPT_MS`: 1 回のスクリプト実行の上限(既定 10000)。

WebP画像は、インストール済みの`libwebp.so.7`または`libwebp.so`を優先して展開します。利用できない環境では内蔵デコーダーを使用します。`LUNA_BROWSER_SOFTWARE_WEBP=1`で内蔵デコーダーに固定できます。

## 通信と応答性

IPv6・IPv4の接続候補を250ms間隔で試し、先に接続できたアドレスを使用します。
配信元が`Cache-Control: public, max-age=...`で再利用を許可したリソースは、
プロセス内で最大32MiB・128件まで保持し、ページ遷移後も再利用します。
ドキュメントのナビゲーション、独自ヘッダー付きのリクエスト、`no-store`、
`no-cache`、`Set-Cookie`、`Accept-Encoding`以外の`Vary`がある応答は対象外です。
タイマーは8msを目安に処理を区切り、描画と次の入力処理へ制御を返します。
実行中のコールバック自体は、この区切りでは中断しません。
