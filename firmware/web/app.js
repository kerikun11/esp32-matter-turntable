(() => {
  const qrBase = 'https://project-chip.github.io/connectedhomeip/qrcode.html?data=';
  let submitting = true;
  let loaded = false;
  let infoGeneration = 0;
  let infoLoading = false;
  let infoKnown = false;
  let commissioningOpen = false;
  let fabricSignature = null;
  let pollTimer = 0;
  let stateLoading = false;
  let formDirty = false;
  const get = (id) => document.getElementById(id);
  const buttons = () => document.querySelectorAll('button');
  const labels = new Map(Array.from(buttons(), (button) => [button, button.textContent]));
  const settingsForm = get('settings-form');
  const notice = (message, error = false) => {
    const status = get('status');
    status.textContent = message;
    status.className = 'status ' + (error ? 'error' : 'success');
    status.hidden = !message;
  };

  function validate(state) {
    for (const key of ['switch', 'moving', 'powered', 'commissioned', 'commissioning_open', 'error', 'reboot']) {
      if (typeof state[key] !== 'boolean') throw new Error('Invalid state');
    }
    for (const key of ['device_name', 'hostname', 'message']) {
      if (typeof state[key] !== 'string') throw new Error('Invalid state');
    }
    for (const key of ['angle', 'target_angle', 'on_angle', 'off_angle', 'max_speed']) {
      if (!Number.isFinite(state[key])) throw new Error('Invalid state');
    }
  }

  function renderLive(state) {
    get('angle-value').textContent = state.angle;
    get('motion').textContent = state.moving ? (state.target_angle + '°へ移動中') : '停止中';
    if (!submitting) {
      const form = document.querySelector('.toggle-form');
      const button = form.querySelector('button');
      form.elements.state.value = state.switch ? 'off' : 'on';
      button.classList.toggle('on', state.switch);
      button.classList.toggle('off', !state.switch);
      button.textContent = state.switch ? 'ON' : 'OFF';
    }
    document.title = state.device_name;
    get('device-name').textContent = state.device_name;
  }

  function fillSettings(state) {
    for (const key of ['device_name', 'hostname', 'on_angle', 'off_angle']) {
      settingsForm.elements[key].value = state[key];
    }
    settingsForm.elements.max_speed.value = state.max_speed;
    syncRanges(settingsForm);
    formDirty = false;
  }

  function render(state) {
    validate(state);
    loaded = true;
    buttons().forEach((button) => {
      button.classList.remove('pending');
      button.removeAttribute('aria-busy');
      button.textContent = labels.get(button) || button.dataset.label;
      button.disabled = !!state.reboot;
    });
    submitting = !!state.reboot;
    renderLive(state);
    if (!formDirty || !state.error) fillSettings(state);
    get('preview').hidden = !state.preview;
    get('reboot').hidden = !state.reboot;
    notice(state.message, state.error);
    document.querySelector('main').removeAttribute('aria-busy');
    updateMatterButtons();
    schedulePoll(state.moving ? 300 : 3000);
  }

  const request = DeviceUI.request;

  function syncRanges(root) {
    root.querySelectorAll('input[data-sync]').forEach((range) => {
      const number = root.querySelector('input[name="' + range.dataset.sync + '"]');
      if (number && number.value !== '') range.value = number.value;
    });
  }
  document.querySelectorAll('input[data-sync]').forEach((range) => {
    const number = range.form.elements[range.dataset.sync];
    range.addEventListener('input', () => { number.value = range.value; number.dispatchEvent(new Event('input', {bubbles: true})); });
    number.addEventListener('input', () => { if (number.value !== '') range.value = number.value; });
  });
  settingsForm.addEventListener('input', () => { formDirty = true; });

  async function submit(action, data, pending, isMatter) {
    submitting = true;
    clearTimeout(pollTimer);
    if (isMatter) {
      ++infoGeneration;
      infoKnown = false;
      get('matter-result').textContent = '';
    }
    buttons().forEach((item) => { item.disabled = true; });
    pending.forEach((item) => {
      item.classList.add('pending');
      item.textContent = '送信中…';
      item.setAttribute('aria-busy', 'true');
    });
    notice('');
    try {
      const state = await request(action, {method: 'POST', body: new URLSearchParams(data)});
      if (action === '/settings' && !state.error) formDirty = false;
      render(state);
      if (isMatter) {
        get('matter-result').textContent = state.message;
        await loadDeviceInfo();
      }
    } catch (error) {
      /* A lost response does not mean the operation failed. Never resend it
         automatically or restore the pre-operation state as if it were current. */
      pending.forEach((item) => {
        item.textContent = '結果未確認';
        item.removeAttribute('aria-busy');
      });
      const message = '操作結果を確認できませんでした。ページを再読み込みして状態を確認してください。';
      notice(message, true);
      if (isMatter) get('matter-result').textContent = message;
    }
  }

  document.addEventListener('submit', (event) => {
    const form = event.target;
    if (form.method.toLowerCase() !== 'post') return;
    event.preventDefault();
    if (submitting) return;
    const action = form.getAttribute('action');
    const isMatter = action === '/matter';
    if (isMatter && !infoKnown) return;
    if (form.dataset.confirm && !window.confirm(form.dataset.confirm)) return;
    const data = new FormData(form);
    const button = event.submitter;
    if (button && button.name) data.append(button.name, button.value);
    submit(action, data, [button].filter(Boolean), isMatter);
  });

  document.querySelectorAll('button[data-try]').forEach((button) => {
    button.addEventListener('click', () => {
      if (submitting) return;
      const input = settingsForm.elements[button.dataset.try];
      if (!input.reportValidity()) return;
      submit('/move', new URLSearchParams({angle: input.value}), [button], false);
    });
  });

  function schedulePoll(delay) {
    clearTimeout(pollTimer);
    pollTimer = setTimeout(poll, delay);
  }
  async function poll() {
    if (document.hidden || submitting || stateLoading) return schedulePoll(3000);
    stateLoading = true;
    try {
      const state = await request('/state');
      if (!loaded) return render(state);
      validate(state);
      if (!submitting) {
        renderLive(state);
        if (!formDirty) fillSettings(state);
        if (state.message) notice(state.message, state.error);
      }
      schedulePoll(state.moving ? 300 : 3000);
    } catch (error) {
      schedulePoll(5000);
    } finally {
      stateLoading = false;
    }
  }

  async function load() {
    submitting = true;
    buttons().forEach((button) => { button.disabled = true; });
    try { render(await request('/state')); }
    catch (error) {
      notice('状態を取得できませんでした。自動で再取得します。', true);
      submitting = false;
      schedulePoll(5000);
    }
  }

  const infoRow = DeviceUI.infoRow;
  function formatDuration(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor(seconds % 86400 / 3600);
    const minutes = Math.floor(seconds % 3600 / 60);
    return (days ? days + '日 ' : '') + hours + '時間 ' + minutes + '分';
  }
  const kib = (bytes) => (bytes / 1024).toFixed(1) + ' KiB';
  function updateMatterButtons() {
    document.querySelectorAll('form[action="/matter"] button').forEach((button) => {
      button.disabled = submitting || !infoKnown ||
        (button.id === 'commissioning-start' && commissioningOpen);
    });
  }
  function renderFabrics(fabrics) {
    const signature = JSON.stringify(fabrics);
    if (signature === fabricSignature) return;
    const list = get('fabric-list');
    list.replaceChildren();
    for (const fabric of fabrics) {
      const entry = document.createElement('div');
      entry.className = 'fabric-entry';
      const table = document.createElement('table');
      table.className = 'info-table';
      table.setAttribute('aria-label', 'Fabric #' + fabric.index);
      const card = document.createElement('tbody');
      table.append(card);
      infoRow(card, 'Fabric #' + fabric.index, fabric.label || 'ラベルなし');
      infoRow(card, 'Fabric ID', fabric.fabric_id);
      infoRow(card, 'Node ID', fabric.node_id);
      infoRow(card, 'Vendor ID', fabric.vendor_id);
      const form = document.createElement('form');
      form.method = 'post';
      form.action = '/matter';
      form.dataset.confirm = 'Fabric #' + fabric.index + '（' +
        (fabric.label || 'ラベルなし') + '）を削除しますか？\n' +
        'このFabricに属するコントローラーから操作できなくなります。Wi-Fi接続は維持されます。' +
        (fabrics.length === 1 ? '\n最後のFabricです。削除後、再登録にはペアリング受付の開始が必要です。' : '');
      for (const [name, value] of Object.entries({
        action: 'remove', index: fabric.index, fabric_id: fabric.fabric_id,
        node_id: fabric.node_id, vendor_id: fabric.vendor_id
      })) {
        const input = document.createElement('input');
        input.type = 'hidden';
        input.name = name;
        input.value = value;
        form.append(input);
      }
      const button = document.createElement('button');
      button.type = 'submit';
      button.className = 'danger';
      button.dataset.label = 'このFabricを削除';
      button.textContent = button.dataset.label;
      button.setAttribute('aria-label', 'Fabric #' + fabric.index + 'を削除');
      form.append(button);
      entry.append(table, form);
      list.append(entry);
    }
    if (!fabrics.length) list.textContent = '登録済みのFabricはありません。';
    fabricSignature = signature;
  }
  async function loadDeviceInfo() {
    const generation = ++infoGeneration;
    infoLoading = true;
    infoKnown = false;
    updateMatterButtons();
    try {
      const info = await request('/device-info');
      if (generation !== infoGeneration) return;
      if (typeof info.commissioning_open !== 'boolean' || !Array.isArray(info.fabrics)) {
        throw new Error('Invalid device info');
      }
      const list = get('device-info');
      list.replaceChildren();
      infoRow(list, 'IPv4アドレス', info.ipv4 || '未取得');
      infoRow(list, 'IPv6アドレス', info.ipv6.join('\n') || '未取得');
      infoRow(list, 'MACアドレス', info.mac || '未取得');
      infoRow(list, 'Wi-Fi', info.connected
        ? info.ssid + '（' + info.rssi + ' dBm, ch ' + info.channel + '）' : '未接続');
      infoRow(list, 'ファームウェア', info.project_name + ' ' + info.version + '\n' + info.build_date);
      infoRow(list, 'ESP-IDF', info.idf_version);
      infoRow(list, '稼働時間', formatDuration(info.uptime_seconds));
      infoRow(list, '前回の起動要因', info.reset_reason);
      infoRow(list, '空きメモリ', kib(info.free_heap) + '（最小 ' + kib(info.min_free_heap) +
        '、最大ブロック ' + kib(info.largest_free_block) + '）');
      get('fabric-count').textContent = '（' + info.fabrics.length + '件）';
      renderFabrics(info.fabrics);
      commissioningOpen = info.commissioning_open;
      infoKnown = true;
      get('commissioning-status').textContent = commissioningOpen
        ? '状態：受付中（最大5分間）' : '状態：受付停止中';
      get('pairing').hidden = !commissioningOpen;
      get('manual-code').textContent = info.manual_code.replace(/^(\d{4})(\d{3})(\d{4})$/, '$1-$2-$3');
      get('qr-link').href = qrBase + encodeURIComponent(info.qr_payload);
      get('info-status').textContent = '設定を開いている間、5秒ごとに更新します。';
    } catch (error) {
      if (generation !== infoGeneration) return;
      get('commissioning-status').textContent = '状態：取得できませんでした';
      get('info-status').textContent = '最新情報を取得できませんでした。自動で再取得します。';
    } finally {
      if (generation === infoGeneration) {
        infoLoading = false;
        updateMatterButtons();
      }
    }
  }
  setInterval(() => {
    if (get('settings-pane').open && !document.hidden && !submitting && !infoLoading) loadDeviceInfo();
  }, 5000);
  get('settings-pane').addEventListener('toggle', () => {
    if (get('settings-pane').open) loadDeviceInfo();
  });
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !submitting) schedulePoll(0);
  });
  window.addEventListener('pageshow', (event) => {
    if (!event.persisted) return;
    load();
    if (get('settings-pane').open) loadDeviceInfo();
  });
  load();
})();
