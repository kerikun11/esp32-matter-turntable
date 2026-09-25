import gzip
import http.client
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from urllib.parse import urlencode

TOOLS = Path(__file__).resolve().parents[1]
MAIN = TOOLS.parent / 'main'
sys.path.insert(0, str(TOOLS / "web"))
from build_web import build, generate
import preview_server as server

JSON = {'Accept': 'application/json'}
SETTINGS = {'device_name': 'Turntable', 'hostname': 'turntable',
            'on_angle': '170', 'off_angle': '10', 'max_speed': '360'}

# Minimal ESP-IDF stand-ins so servo_settings.cpp compiles on the host.
STUB_HEADERS = {
    'esp_timer.h': '#pragma once\n#include <cstdint>\ninline int64_t esp_timer_get_time() { return 0; }\n',
    'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n'
                 'inline const char* esp_err_to_name(esp_err_t) { return ""; }\n',
    'nvs_flash.h': '#pragma once\n',
    'nvs.h': '''#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
inline esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { return 1; }
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_commit(nvs_handle_t) { return 1; }
inline esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*) { return 1; }
inline esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return 1; }
inline esp_err_t nvs_get_i32(nvs_handle_t, const char*, int32_t*) { return 1; }
inline esp_err_t nvs_set_i32(nvs_handle_t, const char*, int32_t) { return 1; }
inline esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*) { return 1; }
inline esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t) { return 1; }
inline esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { return 1; }
inline esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { return 1; }
''',
}

HOSTNAME_CASES = {
    'esp32-matter-turntable': True, 'Turntable2': True, 'a': True, 'a' * 63: True,
    '': False, 'a' * 64: False, '-turntable': False, 'turntable-': False,
    'turn table': False, 'turntable.local': False, 'ターンテーブル': False,
    'under_score': False,
}


def compile_and_run(tmp: Path, source: str, *sources: Path, includes=()):
    test = tmp / 'test.cpp'
    test.write_text(source)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    *('-I' + str(path) for path in includes),
                    str(test), *map(str, sources), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)


class WebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = server.ThreadingHTTPServer(('127.0.0.1', 0), server.PreviewHandler)
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()
        cls.thread.join()

    def setUp(self):
        with server.STATE_LOCK:
            server.STATE = server.PreviewState()

    def request(self, path='/', headers=None, fields=None):
        conn = http.client.HTTPConnection(*self.httpd.server_address, timeout=5)
        try:
            body = None if fields is None else urlencode(fields)
            conn.request('GET' if fields is None else 'POST', path, body, headers or {})
            response = conn.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            conn.close()

    def post(self, path, fields):
        status, _, body = self.request(path, JSON, fields)
        self.assertEqual(status, 200)
        return json.loads(body)

    def test_compression_is_deterministic_and_round_trips(self):
        plain, compressed = build(server.TEMPLATE)
        self.assertEqual(gzip.decompress(compressed), plain)
        self.assertEqual(build(server.TEMPLATE), (plain, compressed))
        self.assertLess(len(plain), server.TEMPLATE.stat().st_size)
        self.assertLess(len(compressed), len(plain) // 2)
        self.assertNotIn(b'{{', plain)
        self.assertIn('読込中'.encode(), plain)

    def test_no_js_comments_survive_minification(self):
        # minify_html doesn't reliably strip every `//` line comment -- catch
        # any future regression generically. URLs ("https://") are allowed.
        plain, _ = build(server.TEMPLATE)
        script = plain[plain.find(b'<script>'):plain.find(b'</script>')]
        comments = re.findall(rb'(?<!:)//[^\n]*', script)
        self.assertEqual(comments, [], f'JS comment(s) survived minification: {comments}')

    def test_hostname_pattern_matches_firmware_rule(self):
        # The HTML pattern attribute is anchored implicitly by the browser.
        html = server.TEMPLATE.read_text(encoding='utf-8')
        pattern = re.search(r'name="hostname"[^>]*pattern="([^"]+)"', html).group(1)
        js_like = re.compile('^(?:' + pattern.replace('\\-', '-') + ')$')
        for hostname, valid in HOSTNAME_CASES.items():
            with self.subTest(hostname=hostname):
                accepted = bool(js_like.match(hostname)) and len(hostname) <= 63
                self.assertEqual(accepted, valid)
                self.assertEqual(bool(server.HOSTNAME_RE.match(hostname)), valid)

    def test_negotiation_and_cache_variants(self):
        status, headers, plain = self.request()
        self.assertEqual(status, 200)
        self.assertNotIn('Content-Encoding', headers)
        identity_tag = headers['ETag']
        status, headers, compressed = self.request(headers={'Accept-Encoding': 'br, gzip'})
        self.assertEqual(headers['Content-Encoding'], 'gzip')
        self.assertEqual(headers['Vary'], 'Accept-Encoding')
        self.assertEqual(gzip.decompress(compressed), plain)
        self.assertNotEqual(headers['ETag'], identity_tag)
        gzip_tag = headers['ETag']
        status, headers, body = self.request(headers={
            'Accept-Encoding': 'gzip', 'If-None-Match': 'W/' + gzip_tag})
        self.assertEqual((status, body), (304, b''))
        status, _, _ = self.request(headers={
            'Accept-Encoding': 'identity', 'If-None-Match': gzip_tag})
        self.assertEqual(status, 200)
        for header in ['gzip;q=0', 'br', '*;q=1,gzip;q=0']:
            status, headers, body = self.request(headers={'Accept-Encoding': header})
            self.assertEqual((status, body), (200, plain))
            self.assertNotIn('Content-Encoding', headers)
        status, _, _ = self.request(headers={'Accept-Encoding': 'gzip;q=0,identity;q=0'})
        self.assertEqual(status, 406)

    def test_state_not_cached_and_action_returns_json(self):
        status, headers, body = self.request('/state')
        self.assertEqual(headers['Cache-Control'], 'no-store')
        self.assertTrue(json.loads(body)['switch'])
        state = self.post('/action', {'state': 'off'})
        self.assertFalse(state['switch'])
        self.assertTrue(state['moving'])
        self.assertEqual(state['target_angle'], state['off_angle'])
        self.assertTrue(state['message'])
        self.assertEqual(json.loads(self.request('/state')[2])['message'], '')

    def test_move_does_not_change_switch_and_finishes(self):
        with server.STATE_LOCK:
            server.STATE.max_speed = 720
        state = self.post('/move', {'angle': '120'})
        self.assertTrue(state['switch'])
        self.assertEqual(state['target_angle'], 120)
        time.sleep(0.2)
        state = json.loads(self.request('/state')[2])
        self.assertFalse(state['moving'])
        self.assertEqual(state['angle'], 120)
        for angle in ('-1', '181', '', '90.5', '1e2'):
            with self.subTest(angle=angle):
                self.assertTrue(self.post('/move', {'angle': angle})['error'])

    def test_settings_json_preserves_special_characters(self):
        name = 'テーブル "<&>\\テスト'
        state = self.post('/settings', {**SETTINGS, 'device_name': name})
        self.assertFalse(state['error'])
        self.assertEqual(state['device_name'], name)
        self.assertEqual((state['on_angle'], state['off_angle'], state['max_speed']), (170, 10, 360))
        for override in ({'device_name': ''}, {'device_name': 'あ' * 22},
                         {'hostname': 'bad host'}, {'hostname': '-bad'},
                         {'on_angle': '181'}, {'off_angle': '-1'},
                         {'max_speed': '0'}, {'max_speed': '721'}, {'on_angle': 'x'}):
            with self.subTest(override=override):
                state = self.post('/settings', {**SETTINGS, **override})
                self.assertTrue(state['error'])
                self.assertEqual(state['device_name'], name)
                self.assertEqual(state['on_angle'], 170)

    def test_legacy_post_redirect(self):
        status, headers, _ = self.request('/action', fields={'state': 'off'})
        self.assertEqual(status, 303)
        self.assertEqual(headers['Location'], '/')
        self.assertFalse(json.loads(self.request('/state')[2])['switch'])

    def test_remove_last_fabric_preserves_wifi_without_opening_commissioning(self):
        info = json.loads(self.request('/device-info')[2])
        fabric = info['fabrics'][0]
        self.assertFalse(self.post('/matter', dict(fabric, action='remove'))['error'])
        info = json.loads(self.request('/device-info')[2])
        self.assertEqual(info['fabrics'], [])
        self.assertTrue(info['connected'])
        self.assertFalse(info['commissioning_open'])
        self.assertFalse(json.loads(self.request('/state')[2])['commissioned'])

    def test_stale_or_invalid_fabric_removal_is_rejected(self):
        original = json.loads(self.request('/device-info')[2])['fabrics']
        for override in ({'index': '1x'}, {'index': '0'}, {'index': '255'},
                         {'fabric_id': '0x0000000000000000'},
                         {'node_id': ''}, {'vendor_id': ''}):
            with self.subTest(override=override):
                state = self.post('/matter', {**original[0], 'action': 'remove', **override})
                self.assertTrue(state['error'])
                self.assertEqual(json.loads(self.request('/device-info')[2])['fabrics'], original)

    def test_commissioning_open_duplicate_and_expiry(self):
        self.assertFalse(self.post('/matter', {'action': 'commission'})['error'])
        info = json.loads(self.request('/device-info')[2])
        self.assertTrue(info['commissioning_open'])
        self.assertEqual(info['manual_code'], '34970112332')
        with server.STATE_LOCK:
            deadline = server.STATE.commissioning_until
        self.post('/matter', {'action': 'commission'})
        with server.STATE_LOCK:
            self.assertEqual(server.STATE.commissioning_until, deadline)
            server.STATE.commissioning_until = 0
        self.assertFalse(json.loads(self.request('/device-info')[2])['commissioning_open'])

    def test_actual_cpp_negotiation_and_generated_header(self):
        # Compile the same negotiation code and generated bytes used by ESP-IDF.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            generate(server.TEMPLATE, tmp / 'web_assets.h')
            compile_and_run(tmp, r'''
#include <cassert>
#include <string>
#include "web_asset_http.h"
#include "web_assets.h"
int main() {
  using namespace web_asset;
  assert(quality("", "gzip") == 0);
  assert(quality("", "identity") == 1);
  assert(quality("br, GZIP; q=0.5", "gzip") == .5f);
  assert(quality("*;q=1, gzip;q=0", "gzip") == 0);
  assert(quality("gzip;q=0, *;q=1", "gzip") == 0);
  assert(quality("*;q=0", "identity") == 0);
  assert(quality("*;q=0,identity;q=1", "identity") == 1);
  assert(quality("gzip;q=oops", "gzip") == 0);
  assert(quality("gzip;q=2", "gzip") == 0);
  assert(quality("x-gzip", "gzip") == 0);
  assert(etagMatches("W/\"tag\", \"other\"", "\"tag\""));
  assert(etagMatches("*", "\"tag\""));
  assert(!etagMatches("\"other\"", "\"tag\""));
  assert(kWebGzip[0] == 0x1f && kWebGzip[1] == 0x8b);
  assert(kWebIdentityEtag[0] == '"');
  assert(std::string(kWebIdentityEtag) != kWebGzipEtag);
}
''', includes=(MAIN, tmp))

    def test_actual_cpp_hostname_validation(self):
        # Same cases as the web form and preview server, against firmware code.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            stubs = tmp / 'stubs'
            stubs.mkdir()
            for name, content in STUB_HEADERS.items():
                (stubs / name).write_text(content)
            cases = '\n'.join(f'  assert(ServoSettings::isValidHostname("{h}") == {str(v).lower()});'
                              for h, v in HOSTNAME_CASES.items())
            compile_and_run(tmp, '#include <cassert>\n#include "servo_settings.h"\n'
                            f'int main() {{\n{cases}\n}}\n',
                            MAIN / 'servo_settings.cpp', includes=(stubs, MAIN))


if __name__ == '__main__':
    unittest.main()
