# firmware/tests

## test_web.py

実機なしで以下を検証する自動テスト。g++が必要です。

- `../tools/web/`のビルドパイプライン（最小化・gzip圧縮・HTTPネゴシエーション・生成C++ヘッダとの整合性）
- プレビューサーバーのJSON API（ON/OFF・試し動作・設定の入力検証・Fabric操作）
- ホスト名の入力規則がWebフォーム・プレビュー・ファームウェア（`ServoSettings::isValidHostname`）で一致すること

```sh
python -m unittest discover -s firmware/tests -v
```

## verify_device.py

実機に対する黒箱動作検証スクリプト。`/version`・`/`・`/state`・`/device-info`・
`/action`・`/move`・`/settings`のHTTP APIをWi-Fi経由で叩き、ON/OFFでサーボが
設定角度まで動くこと・状態が勝手に変わらないこと・不正な入力への耐性・
連続操作後の整合性・設定の保存/復元などを確認します。
**実行中はサーボが実際に動きます。** 終了時にON/OFF状態と設定は元に戻します。
Matterコントローラ経由の操作やOTA書き込みは対象外です。

```sh
python3 firmware/tests/verify_device.py --host esp32-matter-turntable.local
python3 firmware/tests/verify_device.py --host 192.168.0.60 -v
```
