# nRF52840 BLE Application

nRF52840 DK 向けの Bluetooth Low Energy (BLE) サンプルアプリケーションです。
**nRF Connect SDK (Zephyr RTOS)** をベースに構築されており、以下の4つの BLE 基本機能をボタン1つで操作できます。

| 機能 | 役割 |
|------|------|
| ADV送信 | Peripheral として Android からの接続を受け入れる |
| Scan | Central として周囲の BLE デバイスを探索する |
| BLE接続 | スキャンで見つけたデバイスへ接続を開始する |
| 切断 | 接続中のデバイスとの接続を終了する |

---

## 目次

- [動作環境](#動作環境)
- [プロジェクト構成](#プロジェクト構成)
- [ハードウェア仕様](#ハードウェア仕様)
- [ビルド & フラッシュ](#ビルド--フラッシュ)
- [動作詳細](#動作詳細)
  - [起動シーケンス](#起動シーケンス)
  - [ADV送信 (アドバタイジング)](#adv送信-アドバタイジング)
  - [Scan (スキャン)](#scan-スキャン)
  - [BLE接続](#ble接続)
  - [切断](#切断)
  - [状態遷移図](#状態遷移図)
- [GATTサービス仕様](#gattサービス仕様)
- [ログ出力例](#ログ出力例)
- [Android での動作確認](#android-での動作確認)
- [Kconfig 設定一覧](#kconfig-設定一覧)

---

## 動作環境

| 項目 | 内容 |
|------|------|
| ターゲットボード | nRF52840 DK (`nrf52840dk_nrf52840`) |
| SDK | nRF Connect SDK (Zephyr RTOS ベース) |
| CMake | 3.20.0 以上 |
| 接続対象 | Android (BLE 4.0 以上対応端末) |

---

## プロジェクト構成

```
nrf52840_ble_app/
├── CMakeLists.txt   # ビルド設定
├── prj.conf         # Kconfig 設定 (BLE・GPIO・ログ等)
└── src/
    └── main.c       # アプリケーション本体
```

---

## ハードウェア仕様

### GPIO ピンアサイン (nRF52840 DK)

| 論理名 | ピン番号 | 方向 | 機能 |
|--------|---------|------|------|
| SW0 (BTN1) | P0.11 | 入力 | ADV ON/OFF トグル |
| SW1 (BTN2) | P0.12 | 入力 | Scan ON/OFF トグル |
| SW2 (BTN3) | P0.24 | 入力 | スキャン結果のデバイスへ接続 |
| SW3 (BTN4) | P0.25 | 入力 | 接続中デバイスと切断 |
| LED0 | P0.13 | 出力 | ADV中に点灯 |
| LED1 | P0.14 | 出力 | BLE接続中に点灯 |

> すべてのボタンは PORT0 上に配置されているため、単一のコールバック関数で `pins` ビットマスクにより識別しています。
> ボタン割り込みは **`GPIO_INT_EDGE_TO_ACTIVE`** (押した瞬間) に設定されています。

### LED 状態一覧

| LED0 | LED1 | 状態 |
|------|------|------|
| 点灯 | 消灯 | アドバタイジング中 |
| 消灯 | 点灯 | BLE 接続中 |
| 消灯 | 消灯 | アイドル (ADV停止・未接続) |
| 点灯 | 点灯 | ※通常は発生しない (接続後ADVは自動停止) |

---

## ビルド & フラッシュ

```bash
# 1. nRF Connect SDK 環境をセットアップ済みであることを確認
# 2. プロジェクトディレクトリへ移動
cd nrf52840_ble_app

# 3. ビルド
west build -b nrf52840dk_nrf52840

# 4. nRF52840 DK を USB 接続してフラッシュ
west flash

# 5. シリアルログの確認 (115200 baud)
west espressif monitor   # または任意のシリアルターミナル
```

> ログは `CONFIG_LOG_DEFAULT_LEVEL=3` (INF) に設定されています。デバッグ時は `4` (DBG) に変更してください。

---

## 動作詳細

### 起動シーケンス

```
電源投入 / リセット
    │
    ├─ gpio_init()
    │     ├─ LED0, LED1 を出力・消灯に初期化
    │     └─ SW0〜SW3 を入力・エッジ割り込みに初期化
    │
    └─ bt_enable(bt_ready_cb)
          │
          └─ [コールバック] bt_ready_cb()
                └─ adv_start()  ← ADV自動開始
```

起動直後は **アドバタイジングが自動的に開始** され、Android から接続可能な状態になります。

---

### ADV送信 (アドバタイジング)

#### 概要

`bt_le_adv_start()` を使用した **Connectable Undirected Advertising** (`BT_LE_ADV_CONN`) を送信します。
Android を含む周囲の BLE セントラルデバイスから発見・接続が可能です。

#### ADVパケット構成

**Advertising Data (ADパケット)**

| フィールド | 値 | 説明 |
|-----------|-----|------|
| AD Type: Flags | `0x06` | General Discoverable + BR/EDR Not Supported |
| AD Type: Complete Local Name | `nRF52840_BLE` | デバイス名 |

**Scan Response Data (SRパケット)**

| フィールド | 値 | 説明 |
|-----------|-----|------|
| AD Type: 128-bit UUIDs | `12345678-1234-5678-1234-56789abcdef0` | カスタムサービス UUID |

> Scan Response は Android が Active Scan を実行した場合に返します。
> これによりサービス UUID を広告でき、Android 側でフィルタリングが可能になります。

#### 制御フロー

```
BTN1 押下
    ├─ is_advertising == true  →  adv_stop()
    │     ├─ bt_le_adv_stop()
    │     ├─ is_advertising = false
    │     └─ LED0 消灯
    │
    └─ is_advertising == false  →  adv_start()
          ├─ bt_le_adv_start(BT_LE_ADV_CONN, ...)
          ├─ is_advertising = true
          └─ LED0 点灯
```

#### 自動停止条件

- BLE 接続が確立されると `on_connected()` 内で `adv_stop()` が自動呼び出されます。
- 接続中は新たな接続受け入れが不要なためアドバタイジングを停止します。

#### 自動再開条件

- BLE 切断時 (`on_disconnected()`) に `adv_start()` が自動呼び出されます。
- 接続失敗時 (`on_connected()` でエラー) にも `adv_start()` が呼ばれます。

---

### Scan (スキャン)

#### 概要

`bt_le_scan_start()` を使用した **Active Scan** を実行します。
周囲の BLE デバイス (Android を含む) を検出し、ログに出力します。

#### スキャンパラメータ

| パラメータ | 値 | 説明 |
|-----------|-----|------|
| Type | `BT_LE_SCAN_TYPE_ACTIVE` | アクティブスキャン (Scan Request を送信) |
| Options | `BT_LE_SCAN_OPT_NONE` | 重複フィルタなし |
| Interval | `BT_GAP_SCAN_FAST_INTERVAL` | 高速スキャン間隔 (60ms) |
| Window | `BT_GAP_SCAN_FAST_WINDOW` | 高速スキャンウィンドウ (30ms) |

> **Active Scan** は対象デバイスへ Scan Request を送信し、Scan Response を受け取ります。
> これにより ADV パケットに含まれない追加情報 (サービス UUID等) も取得できます。

#### スキャン結果の処理 (`scan_recv_cb`)

スキャンで発見されたデバイスごとに `scan_recv_cb()` が呼ばれます。

```
デバイス発見
    ├─ アドレス文字列変換 (bt_addr_le_to_str)
    ├─ ログ出力: "[SCAN] Found: <アドレス>  RSSI:<値>  Type:<型>"
    │
    └─ target_found == false の場合
          ├─ target_addr にアドレスをコピー (bt_addr_le_copy)
          ├─ target_found = true
          └─ ログ出力: "[SCAN] >> Target saved: <アドレス>"
```

> **最初に発見されたデバイス**のみを接続候補 (`target_addr`) として保存します。
> BTN3 を押すとこの候補に接続します。

#### 制御フロー

```
BTN2 押下
    ├─ is_scanning == true  →  scan_stop()
    │     ├─ bt_le_scan_stop()
    │     └─ is_scanning = false
    │
    └─ is_scanning == false  →  scan_start()
          ├─ target_found = false  (候補リセット)
          ├─ bt_le_scan_start(&param, scan_recv_cb)
          └─ is_scanning = true
```

---

### BLE接続

#### 概要

`bt_conn_le_create()` を使用してスキャンで保存したデバイスへ接続要求を送信します。

#### 接続パラメータ

| パラメータ | 値 | 説明 |
|-----------|-----|------|
| Options | `BT_CONN_LE_OPT_NONE` | 標準オプション |
| Scan Interval | `BT_GAP_SCAN_FAST_INTERVAL` | 接続スキャン間隔 |
| Scan Window | `BT_GAP_SCAN_FAST_INTERVAL` | 接続スキャンウィンドウ |
| Connection Params | `BT_LE_CONN_PARAM_DEFAULT` | デフォルト接続パラメータ |

#### 制御フロー

```
BTN3 押下
    ├─ default_conn != NULL  →  "Already connected" (警告のみ)
    ├─ target_found == false →  "No target. BTN2でScanしてください" (警告のみ)
    │
    └─ 接続処理開始
          ├─ is_scanning == true の場合 → scan_stop() (スキャン自動停止)
          ├─ bt_conn_le_create(&target_addr, ...)
          └─ エラー時: default_conn = NULL, target_found = false
```

#### 接続確立コールバック (`on_connected`)

```
接続確立
    ├─ エラーあり
    │     ├─ bt_conn_unref(default_conn)
    │     ├─ default_conn = NULL
    │     └─ adv_start()  (ADV自動再開)
    │
    └─ 接続成功
          ├─ Peripheral経由 (default_conn == NULL): bt_conn_ref(conn) でハンドル保存
          ├─ Central経由 (default_conn 設定済み): そのまま使用
          ├─ LED1 点灯
          └─ adv_stop()  (ADV自動停止)
```

> **Peripheral 経由** (Android → nRF52840 方向) と **Central 経由** (nRF52840 → デバイス方向) の両パターンを自動判定して処理します。

---

### 切断

#### 制御フロー

```
BTN4 押下
    ├─ default_conn == NULL  →  "Not connected" (警告のみ)
    └─ bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN)
          └─ 切断理由コード: 0x13 (Remote User Terminated Connection)
```

#### 切断コールバック (`on_disconnected`)

```
切断検知
    ├─ ログ出力: "[CONN] Disconnected: <アドレス>  reason=0x<理由コード>"
    ├─ bt_conn_unref(default_conn)
    ├─ default_conn = NULL
    ├─ target_found = false  (接続候補リセット)
    ├─ LED1 消灯
    └─ adv_start()  (ADV自動再開)
```

---

### 状態遷移図

```
                         ┌─────────────────────────────────────────┐
                         │              起動                        │
                         └──────────────────┬──────────────────────┘
                                            │ adv_start() 自動
                                            ▼
                         ┌─────────────────────────────────────────┐
              BTN1 OFF ──┤         ADV中 (LED0点灯)                 ├── BTN2 ON
                         │   is_advertising = true                  │
                         └──────────────────┬──────────────────────┘
                                            │ Android or BTN3 で接続
                                            ▼
              BTN2 ON ──────────────────────────────────────────────
                         ┌─────────────────────────────────────────┐
                         │         スキャン中                       │
                         │   is_scanning = true                     │
                         │   デバイス発見 → target_addr 保存        │
                         └──────────────────┬──────────────────────┘
                                            │ BTN3
                                            ▼
                         ┌─────────────────────────────────────────┐
              BTN4 ─────┤     BLE接続中 (LED1点灯)                  │
                         │   default_conn != NULL                   │
                         │   GATT Read/Write/Notify 利用可能        │
                         └──────────────────┬──────────────────────┘
                                            │ 切断 (BTN4 or Android側)
                                            ▼
                                     adv_start() 自動再開
```

---

## GATTサービス仕様

接続後に Android から以下のカスタム GATT サービスを通じてデータ送受信が可能です。

### サービス

| 項目 | 値 |
|------|-----|
| Service UUID | `12345678-1234-5678-1234-56789abcdef0` |
| タイプ | Primary Service |

### キャラクタリスティック

| 項目 | 値 |
|------|-----|
| Characteristic UUID | `12345678-1234-5678-1234-56789abcdef1` |
| Properties | Read / Write / Notify |
| Permissions | Read / Write |
| 初期値 | `Hello from nRF52840!` (20バイト) |

#### Read (Android → nRF52840 から読み取り)

- `on_char_read()` が呼ばれ、`char_value` の現在の内容を返します。
- ログ: `[GATT] Read request from Android`

#### Write (Android → nRF52840 への書き込み)

- `on_char_write()` が呼ばれ、受信データを `char_value` に上書きします。
- 最大書き込みサイズ: **20バイト** (`sizeof(char_value)`)
- 超過した場合は `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` を返します。
- ログ: `[GATT] Received from Android: <受信文字列>`

#### Notify (nRF52840 → Android への通知)

- CCC (Client Characteristic Configuration) ディスクリプタにより通知の有効/無効を制御します。
- Android が Notify を有効化すると `ccc_cfg_changed()` が呼ばれます。
- ログ: `[GATT] Notifications enabled` / `disabled`

---

## ログ出力例

シリアルターミナル (115200 baud) で以下のようなログが確認できます。

```
======================================
  nRF52840 BLE App
  BTN1: ADV ON/OFF
  BTN2: Scan ON/OFF
  BTN3: Connect to found device
  BTN4: Disconnect
======================================
[00:00:00.012] Bluetooth initialized
[00:00:00.013] [ADV] Started  name="nRF52840_BLE"

>>> BTN2: Toggle Scan
[00:00:05.100] [SCAN] Started (Active)
[00:00:05.120] [SCAN] Found: AA:BB:CC:DD:EE:FF (random)    RSSI: -55  Type:0
[00:00:05.121] [SCAN] >> Target saved: AA:BB:CC:DD:EE:FF (random) (BTN3で接続)
[00:00:05.135] [SCAN] Found: 11:22:33:44:55:66 (public)    RSSI: -72  Type:0

>>> BTN3: Connect
[00:00:08.200] [SCAN] Stopped
[00:00:08.201] [CONN] Connecting to AA:BB:CC:DD:EE:FF (random) ...
[00:00:08.450] [CONN] Connected: AA:BB:CC:DD:EE:FF (random)
[00:00:08.451] [ADV] Stopped

[00:00:10.000] [GATT] Notifications enabled
[00:00:11.000] [GATT] Read request from Android
[00:00:12.000] [GATT] Received from Android: Hello nRF!

>>> BTN4: Disconnect
[00:00:15.300] [CONN] Disconnecting...
[00:00:15.450] [CONN] Disconnected: AA:BB:CC:DD:EE:FF (random)  reason=0x16
[00:00:15.451] [ADV] Started  name="nRF52840_BLE"
```

---

## Android での動作確認

**nRF Connect for Mobile** (Nordic Semiconductor 製・無料) を使用して動作確認できます。

### ADV確認手順

1. nRF Connect アプリを起動
2. 「SCANNER」タブを開く
3. デバイス一覧に **`nRF52840_BLE`** が表示されることを確認
4. タップして Scan Response の UUID `12345678-...` が見えることを確認

### 接続・GATT 操作手順

1. `nRF52840_BLE` の **CONNECT** ボタンをタップ
2. 「CLIENT」タブで Custom Service を展開
3. Characteristic `12345678-...-def1` に対して以下を操作：
   - 📖 **Read**: 現在値 (`Hello from nRF52840!`) を取得
   - ✏️ **Write**: 任意のテキストを送信 → nRF52840 のログに表示される
   - 🔔 **Notify**: ベルアイコンをタップして通知を有効化

### Central として Android へ接続する手順

1. Android 側で Peripheral として ADV を送信するアプリを用意
   (例: nRF Connect の ADVERTISER 機能)
2. nRF52840 の **BTN2** を押してスキャン開始
3. ログで Android のアドレスが表示されたら **BTN3** で接続

---

## Kconfig 設定一覧

`prj.conf` の設定項目と意味の一覧です。

| 設定キー | 値 | 説明 |
|---------|-----|------|
| `CONFIG_BT` | y | Bluetooth スタック有効化 |
| `CONFIG_BT_PERIPHERAL` | y | Peripheral 役割 (ADV・接続受け入れ) |
| `CONFIG_BT_CENTRAL` | y | Central 役割 (Scan・接続開始) |
| `CONFIG_BT_DEVICE_NAME` | `"nRF52840_BLE"` | アドバタイジング時のデバイス名 |
| `CONFIG_BT_DEVICE_APPEARANCE` | 0 | GAP アピアランス値 (0=Unknown) |
| `CONFIG_BT_MAX_CONN` | 2 | 最大同時接続数 |
| `CONFIG_BT_MAX_PAIRED` | 2 | 最大ペアリング保存数 |
| `CONFIG_BT_SMP` | y | セキュリティマネージャプロトコル (ペアリング) |
| `CONFIG_BT_GATT_CLIENT` | y | GATT クライアント機能 |
| `CONFIG_GPIO` | y | GPIO ドライバ |
| `CONFIG_LOG` | y | ログシステム有効化 |
| `CONFIG_LOG_DEFAULT_LEVEL` | 3 | ログレベル INF (1=ERR, 2=WRN, 3=INF, 4=DBG) |
| `CONFIG_MAIN_STACK_SIZE` | 2048 | メインスレッドスタックサイズ (バイト) |

---

## Program Summary

- nRF52840 DK 上で BLE の Peripheral/Central 役割を切り替えて操作できるサンプルアプリ。
- BTN1〜BTN4 で ADV/Scan/接続/切断を切り替え、LED とログで状態を確認。
- カスタム GATT サービス (Read/Write/Notify) を公開して Android からデータ送受信が可能。

## How to Use

- 前提: nRF Connect SDK (Zephyr) の環境構築済み。
- ビルド/フラッシュ手順 (Not verified): `west build -b nrf52840dk_nrf52840` → `west flash` → シリアルログは `west espressif monitor` (115200 baud)。
- 動作: BTN1=ADV ON/OFF、BTN2=Scan ON/OFF、BTN3=スキャンで見つけたデバイスへ接続、BTN4=接続中デバイスと切断。

## Completion Status

- Usable (demo)  
  - ADV/Scan/接続/切断と GATT Read/Write/Notify の一連動作が実装済み。
  - ただし製品向けの堅牢性・テスト・例外処理は限定的なため production ではない。

## Program Summary

- nRF52840 DK 上で BLE Peripheral/Central の基本操作 (ADV/Scan/接続/切断) をボタンで切り替えるサンプル。
- カスタム GATT サービス (Read/Write/Notify) を公開し、Android などからデータ送受信可能。

## How to Use

- Not verified: `west build -b nrf52840dk_nrf52840` → `west flash`。
- Not verified: 115200 baud のシリアルターミナルでログ確認 (具体的なコマンドは環境依存)。
- BTN1=ADV、BTN2=Scan、BTN3=接続、BTN4=切断。

## Completion Status

- Usable (demo)
- ADV/Scan/接続/切断と GATT Read/Write/Notify の基本動作は実装済みだが、テストや製品向けの堅牢化は未確認。
