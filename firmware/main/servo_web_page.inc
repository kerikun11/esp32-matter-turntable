R"HTML(<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>{{DEVICE_NAME}}</title>
  <style>
    :root{--panel:#fff;--line:#dbe3ef;--ink:#17212b;--muted:#64748b;--accent:#2563eb;--good:#166534;--bad:#991b1b;--shadow:0 10px 30px rgba(15,23,42,.08)}
    *{box-sizing:border-box}
    body{margin:0;background:linear-gradient(180deg,#f8fbff,#eef3f9);color:var(--ink);font:15px/1.6 system-ui,-apple-system,"Segoe UI",sans-serif}
    main{max-width:720px;margin:0 auto;padding:24px 16px 40px}
    .hero,.fold{overflow:hidden;border:1px solid var(--line);border-radius:20px;background:var(--panel);box-shadow:var(--shadow)}
    .hero{display:flex;align-items:center;justify-content:space-between;gap:20px;padding:24px}
    h1{margin:0 0 4px;font-size:28px}p{margin:0;color:var(--muted)}
    form{display:grid;gap:16px}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}
    label{display:grid;gap:6px;padding:16px;border:1px solid var(--line);border-radius:14px;background:#fbfdff;font-weight:650}
    label.wide{grid-column:1/-1}
    .mini{color:var(--muted);font-size:13px;font-weight:400}
    input{width:100%;padding:12px 13px;border:1px solid var(--line);border-radius:12px;background:#fff;color:var(--ink);font:inherit}
    button{border:0;border-radius:12px;padding:14px 18px;background:var(--accent);color:#fff;font:700 15px system-ui;cursor:pointer}
    .toggle-btn{display:flex;align-items:center;justify-content:center;gap:8px;min-width:120px;border-radius:999px}
    .toggle-btn:before{content:"";width:8px;height:8px;border-radius:50%;background:currentColor}
    .toggle-btn.on{background:#e8f7ee;color:var(--good)}.toggle-btn.off{background:#fdecec;color:var(--bad)}
    .fold{margin-top:14px}.fold summary{display:flex;align-items:center;justify-content:space-between;list-style:none;cursor:pointer;padding:18px 20px;font-size:17px;font-weight:700}
    .fold summary::-webkit-details-marker{display:none}.fold summary span{color:var(--muted);font-size:12px;font-weight:500}.fold .body{padding:0 20px 20px}.fold form{gap:16px}
    .status{margin:14px 0 0;padding:14px 18px;border:1px solid;border-radius:12px;font-weight:650}
    .success{border-color:#86efac;background:#f0fdf4;color:var(--good)}
    .error{border-color:#fca5a5;background:#fef2f2;color:var(--bad)}
    @media(max-width:560px){main{padding:14px 10px 28px}.hero{align-items:stretch;flex-direction:column;padding:18px}.toggle-btn{width:100%}.grid{grid-template-columns:1fr}label.wide{grid-column:auto}.fold summary{padding:16px}.fold .body{padding:0 16px 16px}}
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <div>
        <h1><a href="/" style="color:inherit;text-decoration:none">{{DEVICE_NAME}}</a></h1>
        <p>サーボの位置と動作速度を設定します。</p>
      </div>
      <form method="post" action="/action">
        <input type="hidden" name="state" value="{{SWITCH_ACTION}}">
        <button class="toggle-btn {{SWITCH_CLASS}}">{{SWITCH_STATE}}</button>
      </form>
    </section>
    {{STATUS_NOTICE}}
    <details class="fold">
      <summary>設定<span>タップして開閉</span></summary>
      <div class="body">
        <form method="post" action="/settings">
          <div class="grid">
            <label class="wide">
              <span>デバイス名</span>
              <input name="device_name" type="text" maxlength="64" required value="{{DEVICE_NAME}}">
              <span class="mini">タイトルとブラウザのタブに表示されます。</span>
            </label>
            <label class="wide">
              <span>ホスト名</span>
              <input name="hostname" type="text" maxlength="63" required value="{{HOSTNAME}}">
              <span class="mini">この画面は<strong>http://&lt;ホスト名&gt;.local</strong>でアクセスできます。</span>
            </label>
            <label>
              <span>OFFの角度</span>
              <input name="off_angle" type="number" min="0" max="180" required value="{{OFF_ANGLE}}">
              <span class="mini">0〜180度</span>
            </label>
            <label>
              <span>ONの角度</span>
              <input name="on_angle" type="number" min="0" max="180" required value="{{ON_ANGLE}}">
              <span class="mini">0〜180度</span>
            </label>
            <label class="wide">
              <span>最大速度</span>
              <input name="max_speed" type="number" min="1" max="720" required value="{{MAX_SPEED}}">
              <span class="mini">1〜720度/秒。次回のON/OFF操作から反映されます。</span>
            </label>
          </div>
          <button>設定を保存</button>
        </form>
      </div>
    </details>
  </main>
</body>
</html>)HTML"
