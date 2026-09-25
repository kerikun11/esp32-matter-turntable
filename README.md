# ESP32 Matter Turntable - KERI's Lab

ESP32で作るMatter対応サーボ回転台 (Amazon Echo Show向け)

## 機能

- Matter Endpoint（On/Off Plug-in Unit）
  - Google Home / Amazon AlexaアプリからON/OFFすると、サーボが設定した角度へ加減速付きで回転する。
- WebUI（`http://<ホスト名>.local`）
  - ON/OFF操作、現在角度の表示、ON/OFFの角度・最大速度の設定（保存前に「試す」で動作確認可）
  - 任意角度への試し動作
  - デバイス情報（IPアドレス、Wi-Fi、ファームウェア、稼働時間、空きメモリ）
  - Matter Fabric一覧・個別削除、ペアリング受付の開始とペアリングコード表示、再起動
- 物理ボタン（BOOTボタン）
  - 短押しでON/OFFをトグル。
  - 5秒長押しでMatterを初期化（未登録時はペアリング受付を開始）。
- RGB LED

| LED | 意味 |
| :-: | :-- |
| 白 | 通常動作 |
| 桃 | Matter未登録・ペアリング受付中 |
| 赤 | Wi-Fi（IPv4）未接続 |
| 青 (点滅) | Matterからの操作を受信 |
| 水色 (点滅) | ボタン操作 |

## 環境

- 開発環境
  - [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.5
  - [ESP-Matter](https://github.com/espressif/esp-matter) v1.5
- マイコンボード
  - Seeed Studio XIAO ESP32C6
- サーボモーター
  - SG90互換品
- ピンアサイン
  - ソースコード [app_config.h](firmware/main/app_config.h) を参照。

ビルド・書き込み・OTA更新は[ファームウェア開発ガイド](firmware/README.md)を参照。

## Matterペアリング

- QRコード: https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:Y.K9042C00KA0648G00
- ペアリングコード: 34970112332

---

## ライセンス

- LGPL v2.1
