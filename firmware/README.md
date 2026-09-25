# ファームウェア開発ガイド

ESP32 Matter Turntableのビルド・書き込み・更新・検証手順。
デバイスの使い方は[プロジェクトREADME](../README.md)を参照してください。

## 開発環境

- ESP-IDF: 5.5.5（[dependencies.lock](dependencies.lock)に記録されたバージョン）
- ESP-Matter: 1.5.x（同ロックファイルに記録されたバージョン）
- 対応ターゲット: ESP32-C6
- WebUIビルド用のPythonパッケージ: [requirements.txt](tools/web/requirements.txt)

依存コンポーネントは[main/idf_component.yml](main/idf_component.yml)で指定し、
ESP-IDFのComponent Managerで取得します。arduino-esp32には依存しません。

## ビルド・USB書き込み

[ESP-IDFのドキュメント](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c6/get-started/index.html)に従って
開発環境を用意し、以下を実行します。以降、このREADMEのコマンドは `firmware/` ディレクトリで実行します。

```sh
# ESP-IDFの環境を有効化
source "$IDF_PATH/export.sh"
cd firmware

# 初回またはターゲット変更時
idf.py set-target esp32c6

# 初回のみ、ESP-IDFのPython環境にWebUIビルドの依存を追加
python -m pip install -r tools/web/requirements.txt

# HTML/CSS/JSの最小化・gzip圧縮も自動実行
idf.py build
```

生成されるファームウェアは `build/esp32-matter-turntable.bin` です。

ESP32のUSBポートをPCに接続して書き込みます。`/dev/ttyACM0` は接続先に合わせて変更してください。

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

シリアルモニタは `Ctrl+]` で終了します。

## OTA（ファームウェア更新）

curlコマンドでファームウェアを書き換えられる。書き込み後は自動で再起動する。

```sh
curl --data-binary @build/esp32-matter-turntable.bin "http://<ホスト名>.local/update"
```

デバイスのプロジェクト名（CMakeの`PROJECT_NAME`）とイメージのプロジェクト名が一致しない場合は拒否される
（LAN内の別機種へ誤って書き込まないためのガード）。意図的に上書きしたい場合は `?skip_check=1` を付ける。
現在のバージョン情報は `GET /version` で確認できる。

```sh
curl "http://<ホスト名>.local/version"
```

新しいファームウェアの初回起動がクラッシュループした場合は、ブートローダーが自動的に直前のファームウェアに
ロールバックする（USBで書き込んだブートローダーのみ対応）。

### 旧ファームウェア（arduino-esp32版）からの更新

パーティションテーブル（アプリ領域の拡張）とブートローダー（ロールバック対応）が変わったため、
初回だけUSBで `idf.py -p /dev/ttyACM0 flash` により書き込む（OTAでは更新できない）。
NVS領域の位置・サイズは旧版と同じで `flash` はNVSを消去しないため、Matterの登録情報・Wi-Fi設定・
Web画面の設定（デバイス名・ホスト名・角度・速度・ON/OFF状態）はそのまま引き継がれる。
以降の更新はOTA（`/update`）で行える。

## WebUIの開発・動作確認

WebUIのソースは [main/web/index.html](main/web/index.html) です。実機なしのプレビュー:

```sh
python tools/web/preview_server.py
```

<http://localhost:8000> を開いて確認します。プレビューの操作は実機に送信されません。

### 自動テスト

```sh
python -m unittest discover -s tools/tests -v
```

### 実機検証（サーボが実際に動きます）

```sh
python tools/tests/verify_device.py --host esp32-matter-turntable.local
```

検証範囲は[テストツール](tools/tests/README.md)を参照してください。

## HTTP API

| メソッド・パス | 内容 |
| --- | --- |
| `GET /` | WebUI（gzip/ETag対応） |
| `GET /state` | ON/OFF状態・現在角度・移動中か・設定値（JSON） |
| `GET /device-info` | IPアドレス・Wi-Fi・ファームウェア・メモリ・Fabric一覧・ペアリングコード（JSON） |
| `POST /action` `state=on\|off` | ON/OFFを切り替え、サーボを設定角度へ移動 |
| `POST /move` `angle=0..180` | 試し動作（ON/OFF状態は変えない） |
| `POST /settings` | デバイス名・ホスト名・角度・最大速度を保存 |
| `POST /matter` `action=commission\|remove` | ペアリング受付開始・Fabric削除 |
| `POST /reboot` | 再起動 |
| `POST /update` / `GET /version` | OTA更新・バージョン情報 |

POSTは `Accept: application/json` を付けると更新後の `/state` を返し、付けない場合は `/` へリダイレクトします。

## 関連ドキュメント

- [開発ツール一覧](tools/README.md)
- [コーディングルール](CODING_RULE.md)
- [ピンアサイン](main/app_config.h)
- [OTA実装](main/ota_service.cpp)
- [ライセンス](LICENSE.md)
