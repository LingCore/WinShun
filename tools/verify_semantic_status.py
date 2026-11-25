"""Checks what winshun.semantic says of its index while it builds it (GitHub #458).

Runs WinShunSemantic.exe by itself, as Win顺's plugin host would: JSON-RPC on
its stdio, its winshun/tool calls (list_documents, read_file_text,
index_status) answered from a made-up list of files, and the model played by a
slow fake /v1/embeddings server here. No Win顺, no index, nothing of the
user's is read; the plugin's data goes to a temporary folder.

    python tools/verify_semantic_status.py build/<dir>/library-plugins/winshun.semantic/WinShunSemantic.exe

Exit code 0 when every check held.
"""
import json, os, queue, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FILES = 12
DELAY = 0.35  # seconds the model takes per file
failures = []


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what, flush=True)
    if not ok:
        failures.append(what)


class Model(BaseHTTPRequestHandler):
    requests = 0

    def log_message(self, *a):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
        inputs = req['input'] if isinstance(req['input'], list) else [req['input']]
        Model.requests += 1
        time.sleep(DELAY)
        body = json.dumps({'object': 'list', 'model': 'fake', 'data': [
            {'object': 'embedding', 'index': i, 'embedding': [1.0, 0.0, 0.0, 0.0]} for i in range(len(inputs))]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Plugin:
    """The plugin's process, with Win顺's side of the wire."""

    def __init__(self, exe, data, endpoint):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 1
        self.replies = {}
        self.reports = []  # every winshun/status, in order
        self.cv = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        self.settings = {'provider': 'custom', 'endpoint': endpoint, 'model': 'fake', 'apiKey': 'k', 'folders': '', 'types': 'documents'}
        self.call('initialize', {'protocolVersion': '1', 'winshun': '0.46.0', 'locale': 'zh', 'dataDir': data, 'pluginDir': os.path.dirname(exe),
                                 'settings': self.settings})

    def _send(self, message):
        message['jsonrpc'] = '2.0'
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8'))
        self.proc.stdin.flush()

    def _read(self):
        for line in self.proc.stdout:
            m = json.loads(line.decode('utf-8'))
            method = m.get('method')
            if method == 'winshun/tool':
                name = m['params']['name']
                if name == 'list_documents':
                    result = {'items': [{'path': 'C:\\docs\\file%02d.docx' % i, 'stamp': 100 + i} for i in range(FILES)], 'reading': False}
                elif name == 'read_file_text':
                    result = {'text': '报销流程与差旅费用申请的规定。' * 20}
                else:
                    result = {'user_idle_seconds': 3600}
                self._send({'id': m['id'], 'result': result})
            elif method == 'winshun/status':
                with self.cv:
                    self.reports.append(m['params'])
                    self.cv.notify_all()
            elif 'id' in m and method is None:
                with self.cv:
                    self.replies[m['id']] = m
                    self.cv.notify_all()

    def call(self, method, params):
        call_id = self.next_id
        self.next_id += 1
        self._send({'id': call_id, 'method': method, 'params': params})
        with self.cv:
            if not self.cv.wait_for(lambda: call_id in self.replies, 15):
                raise RuntimeError('no answer to ' + method)
            return self.replies.pop(call_id).get('result', {})

    def query(self):
        return self.call('query', {'queryId': self.next_id, 'command': 'find', 'trigger': 'keyword', 'keyword': '?', 'text': '', 'raw': '? ', 'limit': 30})

    def status_action(self, action):
        return self.call('action', {'command': '', 'action': action, 'trigger': 'status', 'item': {}, 'text': ''})

    def wait_report(self, fits, seconds=20):
        """The newest report once it fits; None when none did in time."""
        with self.cv:
            ok = self.cv.wait_for(lambda: self.reports and fits(self.reports[-1]), seconds)
            return self.reports[-1] if ok else None

    def last(self):
        with self.cv:
            return self.reports[-1] if self.reports else {}

    def stop(self):
        try:
            self.call('shutdown', {})
        except Exception:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def actions(report):
    return [a['id'] for a in report.get('actions', [])]


def main():
    exe = os.path.abspath(sys.argv[1])
    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    endpoint = 'http://127.0.0.1:%d/v1' % server.server_address[1]
    with tempfile.TemporaryDirectory(prefix='ws-semantic-status-') as data:
        p = Plugin(exe, data, endpoint)
        try:
            # Counted, then waiting for the user's yes: the card says what would go, with 开始.
            r = p.wait_report(lambda r: actions(r) == ['start'])
            check(r is not None and '12 个文件' in r['text'] and r.get('tone') == 'warn' and 'progress' not in r,
                  'waiting for the yes: the card says what would be sent, with 开始: %s' % (r and r['text']))
            check(Model.requests == 0, 'nothing sent before the yes')
            row = p.query()
            check(row['items'][0]['id'] == 'start' and 'rerun' not in row, 'the search box asks the same, and does not rerun while it waits')

            # 开始 on the card: the bar moves, 暂停 is offered.
            p.status_action('start')
            r = p.wait_report(lambda r: actions(r) == ['pause'] and r.get('progress', 0) > 0)
            check(r is not None and r.get('tone') == 'busy' and '/ 12 个文件' in r['text'], 'indexing: files done of the total, a bar, 暂停: %s' % (r and r['text']))
            row = p.query()
            check(row.get('rerun') == 1 and row['items'][0]['id'] == 'pause' and '/ 12 个文件' in row['items'][0]['title'],
                  'the search box row moves by itself (rerun) and Enter pauses: %s' % row['items'][0]['title'])

            # 暂停: said at once, and no file goes through while it holds.
            p.status_action('pause')
            r = p.wait_report(lambda r: actions(r) == ['resume'], 5)
            check(r is not None and r.get('tone') == 'idle' and '已暂停' in r['text'] and 'progress' in r, 'paused: said so, the bar kept, 继续: %s' % (r and r['text']))
            time.sleep(DELAY * 2)  # the file under way finishes
            held = (Model.requests, p.last().get('progress'))
            time.sleep(DELAY * 5)
            check((Model.requests, p.last().get('progress')) == held and held[1] < 1, 'paused: nothing more is sent (%d requests, %.2f)' % (held[0], held[1] or 0))
            check(os.path.exists(os.path.join(data, 'paused')), 'paused: remembered in the data folder')
            row = p.query()
            check('rerun' not in row and row['items'][0]['id'] == 'resume', 'paused: the search box row offers 继续 and stays still')

            # Still paused after a restart.
            p.stop()
            p = Plugin(exe, data, endpoint)
            r = p.wait_report(lambda r: actions(r) == ['resume'])
            time.sleep(DELAY * 4)
            check(r is not None and Model.requests == held[0] and '/ 12 个文件' in r['text'] and 0 < r.get('progress', 0) < 1,
                  'restarted: still paused where it was, nothing sent: %s' % (r and r['text']))

            # 继续, from the search box's row this time: to the end.
            p.call('action', {'command': 'find', 'action': 'default', 'item': {'id': 'resume'}, 'text': ''})
            r = p.wait_report(lambda r: r.get('tone') == 'ok', 30)
            check(r is not None and '已就绪：12 个文件' in r['text'] and not actions(r) and 'progress' not in r, 'done: ready, no bar, no button: %s' % (r and r['text']))
            check(not os.path.exists(os.path.join(data, 'paused')), 'resumed: the pause is forgotten')
            texts = [x['text'] for x in p.reports]
            check(len(texts) == len(set(texts)) or all(texts[i] != texts[i + 1] for i in range(len(texts) - 1)), 'no report is sent twice in a row')
            row = p.query()
            check('rerun' not in row and '已就绪' in row['items'][0]['title'], 'ready: the search box row rests')
        finally:
            p.stop()
            server.shutdown()
    print('\n%s' % ('ALL OK' if not failures else '%d FAILED' % len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
