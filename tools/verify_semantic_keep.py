"""Checks that winshun.semantic keeps its index while Win顺 is still reading or
checking files (GitHub #458: after an update the index looked lost, "没有要建索引
的文件", and was built again from the start).

Runs WinShunSemantic.exe by itself, as Win顺's plugin host would: JSON-RPC on
its stdio, its winshun/tool calls answered by a made-up Win顺 whose content
index can be told to be "still reading" (index_status pending / reading, and a
listing that lacks the files not done yet). The files are real ones in a
temporary folder, so the plugin can tell a deleted file from one not listed
yet; the model is a fake /v1/embeddings server here. Nothing of the user's is
read.

    python tools/verify_semantic_keep.py build/<dir>/WinShunSemantic.exe

Exit code 0 when every check held.
"""
import json, os, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

failures = []


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what, flush=True)
    if not ok:
        failures.append(what)


class Model(BaseHTTPRequestHandler):
    texts = 0  # inputs embedded

    def log_message(self, *a):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
        inputs = req['input'] if isinstance(req['input'], list) else [req['input']]
        Model.texts += len(inputs)
        body = json.dumps({'object': 'list', 'model': 'fake', 'data': [
            {'object': 'embedding', 'index': i, 'embedding': [1.0, 0.0, 0.0, 0.0]} for i in range(len(inputs))]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class WinShun:
    """What the made-up Win顺 says: the files its content index lists, and whether it is at rest."""

    def __init__(self):
        self.files = []  # paths listed (the rest are "not done yet")
        self.settled = True
        self.lists = 0  # list_documents calls

    def answer(self, name, args):
        if name == 'list_documents':
            self.lists += 1
            folders = [f.lower().rstrip('\\') + '\\' for f in args.get('folders', [])]
            items = [{'path': f, 'stamp': 7} for f in self.files
                     if not folders or any(f.lower().startswith(x) for x in folders)]
            return {'items': items, 'reading': not self.settled}
        if name == 'read_file_text':
            return {'text': '报销流程与差旅费用申请的规定。' * 20}
        return {'user_idle_seconds': 3600, 'index': {'state': 'ready'},
                'content_index': {'enabled': True, 'files': len(self.files), 'pending': 0 if self.settled else 40, 'reading': not self.settled}}


class Plugin:
    def __init__(self, exe, data, endpoint, winshun, folders=''):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.winshun = winshun
        self.next_id = 1
        self.replies = {}
        self.cv = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        self.settings = {'provider': 'custom', 'endpoint': endpoint, 'model': 'fake', 'apiKey': 'k', 'folders': folders, 'types': 'documents'}
        self.call('initialize', {'protocolVersion': '1', 'winshun': '0.49.0', 'locale': 'zh', 'dataDir': data, 'pluginDir': os.path.dirname(exe),
                                 'settings': self.settings})

    def _send(self, message):
        message['jsonrpc'] = '2.0'
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8'))
        self.proc.stdin.flush()

    def _read(self):
        for line in self.proc.stdout:
            m = json.loads(line.decode('utf-8'))
            if m.get('method') == 'winshun/tool':
                self._send({'id': m['id'], 'result': self.winshun.answer(m['params']['name'], m['params'].get('arguments', {}))})
            elif 'id' in m and m.get('method') is None:
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

    def rows(self, text=''):
        r = self.call('query', {'queryId': self.next_id, 'command': 'find', 'trigger': 'keyword', 'keyword': '?', 'text': text,
                                'raw': '? ' + text, 'limit': 30})
        return r.get('items', [])

    def status(self):
        return self.rows()[0]['title']

    def pass_again(self, folders=None):
        """The same settings again (or other folders): a new pass, at once; back when it has listed and settled down."""
        if folders is not None:
            self.settings['folders'] = folders
        before = self.winshun.lists
        self._send({'method': 'settingsChanged', 'params': {'settings': self.settings}})
        end = time.time() + 10
        while self.winshun.lists == before and time.time() < end:
            time.sleep(0.05)
        time.sleep(0.6)

    def wait_status(self, fits, seconds=15):
        end = time.time() + seconds
        while time.time() < end:
            title = self.status()
            if fits(title):
                return title
            time.sleep(0.1)
        return self.status()

    def stop(self):
        try:
            self.call('shutdown', {})
        except Exception:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    exe = os.path.abspath(sys.argv[1])
    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    endpoint = 'http://127.0.0.1:%d/v1' % server.server_address[1]
    with tempfile.TemporaryDirectory(prefix='ws-semantic-keep-') as root:
        data = os.path.join(root, 'data')
        docs = os.path.join(root, 'docs')
        os.makedirs(data)
        os.makedirs(os.path.join(docs, 'a'))
        paths = [os.path.normpath(os.path.join(docs, 'a' if i < 3 else '', 'file%02d.docx' % i)) for i in range(8)]
        for p in paths:
            open(p, 'wb').close()
        winshun = WinShun()
        winshun.files = list(paths)
        p = Plugin(exe, data, endpoint, winshun)
        try:
            # Built once, with Win顺 at rest.
            first = p.wait_status(lambda t: t == '开始建立语义索引')
            check(first == '开始建立语义索引', 'asks for the yes first: ' + first)
            p.call('action', {'command': 'find', 'action': 'default', 'item': {'id': 'start'}, 'text': ''})
            ready = p.wait_status(lambda t: '已就绪：8 个文件' in t)
            check('已就绪：8 个文件' in ready, 'built: ' + ready)
            built = Model.texts
            p.stop()

            # Started again while Win顺 is still checking every file: it lists none of them.
            winshun.files = []
            winshun.settled = False
            p = Plugin(exe, data, endpoint, winshun)
            time.sleep(1.5)
            title = p.status()
            check('已就绪：8 个文件' in title, 'restarted with nothing listed yet: the index is kept and said ready: ' + title)
            found = p.rows('报销')
            check(len(found) > 0 and found[0]['id'] in paths, 'and it is searched: %s' % [r['title'] for r in found[:3]])
            check(not any('没有要建索引的文件' in r['title'] for r in found), 'no "没有要建索引的文件" over the results')

            # Some of them listed, the rest not checked yet: nothing leaves, nothing is sent again.
            winshun.files = paths[:4]
            p.pass_again()
            check('已就绪：8 个文件' in p.status(), 'half listed while still checking: all 8 kept: ' + p.status())
            check(Model.texts == built + 1, 'nothing sent again but the search (%d texts after %d)' % (Model.texts, built))

            # At rest again, all listed: nothing to do.
            winshun.files = list(paths)
            winshun.settled = True
            p.pass_again()
            check('已就绪：8 个文件' in p.status() and Model.texts == built + 1, 'at rest, all listed: 8, nothing sent: ' + p.status())

            # A file deleted: it leaves at once, even before Win顺 is at rest.
            os.remove(paths[7])
            winshun.files = paths[:7]
            winshun.settled = False
            p.pass_again()
            check('已就绪：7 个文件' in p.status(), 'a deleted file leaves at once: ' + p.status())

            # One still there but no longer listed with Win顺 at rest (excluded there): kept once, gone the second time.
            winshun.settled = True
            winshun.files = paths[:6]
            p.pass_again()
            check('已就绪：7 个文件' in p.status(), 'not listed once at rest: kept: ' + p.status())
            p.pass_again()
            check('已就绪：6 个文件' in p.status(), 'not listed twice at rest: it leaves: ' + p.status())

            # Fewer folders in the settings: the files outside leave at once, nothing to ask or send.
            p.pass_again(folders=os.path.join(docs, 'a'))
            title = p.wait_status(lambda t: '已就绪：3 个文件' in t, 5)
            check('已就绪：3 个文件' in title and Model.texts == built + 1, 'only folder a: 3 files, nothing sent: ' + title)
        finally:
            p.stop()
            server.shutdown()
    print('\n%s' % ('ALL OK' if not failures else '%d FAILED' % len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
