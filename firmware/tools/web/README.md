# firmware/tools/web

`main/web/index.html`（WebUI）のビルドツールと、実機なしのブラウザプレビュー。

## build_web.py

`main/CMakeLists.txt`から`idf.py build`時に自動実行され、`index.html`を最小化・gzip圧縮して
C++ヘッダ（`kWebIdentity`/`kWebGzip`とそれぞれのETag）を生成します。手動で呼ぶ必要は
普段ありません。初回ビルド時のみ依存パッケージのインストールが必要です。

```sh
python -m pip install -r firmware/tools/web/requirements.txt
```

## preview_server.py

実機なしで画面・状態更新を確認するプレビューサーバーです。同じ`index.html`を
最小化・gzip圧縮して配信し、`/state`・`/device-info`とPOSTのJSON応答を模擬します
（サーボの移動も時間経過で模擬します）。操作は実機へ送信されません。

```sh
python firmware/tools/web/preview_server.py
```

<http://localhost:8000> を開いてください。元のHTMLを保存すると、次のアクセスで再生成します。

## テスト

[../tests/](../tests/)を参照してください。

生成ファイルは`firmware/build/esp-idf/main/generated/`にあり、Gitには含めません。
