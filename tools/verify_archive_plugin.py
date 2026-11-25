"""Checks the archive search plugin (winshun.archive) over its own wire.

Runs WinShunArchive.exe by itself, as Win顺's plugin host would: JSON-RPC on
its stdio, its winshun/tool call (list_files) answered from the small
archives of tests/fixtures/archives copied to a temporary folder. No Win顺,
no index, nothing of the user's is read; the plugin's data goes to a
temporary folder too.

    python tools/verify_archive_plugin.py build/<dir>/library-plugins/winshun.archive/WinShunArchive.exe

Exit code 0 when every check held.
"""
import json, os, shutil, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, '..', 'tests', 'fixtures', 'archives')
failures = []


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what, flush=True)
    if not ok:
        failures.append(what)


class Plugin:
    """The plugin's process, with Win顺's side of the wire."""

    def __init__(self, exe, data, folder, settings=None, page=1000):
        self.folder = folder
        self.page = page  # of list_files: small, to see it ask for the next
        self.calls = []  # the arguments of every list_files
        self.fail_list = None  # an error to answer list_files with
        self.building = False  # Win顺's index says it is still being built
        self.hidden = set()  # file names left out of the list (deleted, as the index sees it)
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 1
        self.replies = {}
        self.reports = []  # every winshun/status, in order
        self.listed_at = 0
        self.logs = []
        self.cv = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        self.settings = {'more': '', 'folders': '', 'system': False}
        self.settings.update(settings or {})
        self.call('initialize', {'protocolVersion': '1', 'winshun': '0.47.0', 'locale': 'zh', 'dataDir': data,
                                 'pluginDir': os.path.dirname(exe), 'capabilities': {'details': True}, 'settings': self.settings})

    def _send(self, message):
        message['jsonrpc'] = '2.0'
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8'))
        self.proc.stdin.flush()

    def _files(self, extensions):
        out = []
        for name in sorted(os.listdir(self.folder)):
            path = os.path.join(self.folder, name)
            if name in self.hidden or not os.path.isfile(path) or name.rsplit('.', 1)[-1].lower() not in extensions:
                continue
            stat = os.stat(path)
            out.append({'path': path, 'size': stat.st_size, 'modified': int(stat.st_mtime)})
        return out

    def _read(self):
        for line in self.proc.stdout:
            m = json.loads(line.decode('utf-8'))
            method = m.get('method')
            if method == 'winshun/tool':
                args = m['params']['arguments']
                if m['params']['name'] != 'list_files':
                    self._send({'id': m['id'], 'error': {'code': -32602, 'message': 'Unknown tool: ' + m['params']['name']}})
                    continue
                self.calls.append(args)
                if not args.get('cursor'):
                    self.listed_at = len(self.reports)  # a pass begins: what it says comes after this
                if self.fail_list:
                    self._send({'id': m['id'], 'error': {'code': -32000, 'message': self.fail_list}})
                    continue
                files = self._files(args['extensions'])
                start = args.get('cursor', 0)
                result = {'items': files[start:start + self.page], 'returned': len(files[start:start + self.page])}
                if start + self.page < len(files):
                    result['next_cursor'] = start + self.page
                if self.building:
                    result['index_state'] = 'scanning'
                self._send({'id': m['id'], 'result': result})
            elif method == 'winshun/status':
                with self.cv:
                    self.reports.append(m['params'])
                    self.cv.notify_all()
            elif method == 'winshun/log':
                self.logs.append(m['params']['text'])
            elif 'id' in m and method is None:
                with self.cv:
                    self.replies[m['id']] = m
                    self.cv.notify_all()

    def call(self, method, params, timeout=15):
        id_ = self.next_id
        self.next_id += 1
        self._send({'id': id_, 'method': method, 'params': params})
        end = time.time() + timeout
        with self.cv:
            while id_ not in self.replies:
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)
            m = self.replies.pop(id_)
        return m.get('result') if 'error' not in m else {'error': m['error']}

    def notify(self, method, params):
        self._send({'method': method, 'params': params})

    def wait_report(self, test, timeout=20, since=0):
        """The first report from `since` on that `test` holds for."""
        end = time.time() + timeout
        with self.cv:
            while True:
                for report in self.reports[since:]:
                    if test(report):
                        return report
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def query(self, text, trigger='keyword', filters=None):
        params = {'queryId': self.next_id, 'command': 'find', 'trigger': trigger, 'keyword': 'zip' if trigger == 'keyword' else '',
                  'text': text, 'raw': ('zip ' if trigger == 'keyword' else '') + text, 'limit': 200}
        if filters is not None:
            params['filters'] = filters
        return self.call('query', params) or {}

    def action(self, action, item):
        result = self.call('action', {'command': 'find', 'action': action, 'item': item, 'text': ''}) or {}
        return result.get('effects', [])

    def status(self, action):
        return self.call('action', {'command': '', 'action': action, 'trigger': 'status', 'item': {}, 'text': ''})

    def after_pass(self, start, test=None, timeout=20):
        """Does `start` (a click on 立即更新, new settings), waits for the pass it
        sets off to ask for the list, and for the report that ends that pass."""
        asked = len(self.calls)
        start()
        end = time.time() + timeout
        while len(self.calls) == asked and time.time() < end:
            time.sleep(0.02)
        return self.wait_report(test or settled, since=self.listed_at, timeout=timeout) if len(self.calls) > asked else None

    def refresh(self, test=None):
        return self.after_pass(lambda: self.status('refresh'), test)

    def stop(self):
        self.call('shutdown', {}, timeout=5)
        try:
            return self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None


def rows(result):
    return [item for item in result.get('items', []) if item.get('valid', True)]


def settled(report):
    return 'progress' not in report and report.get('tone') == 'ok'


def main():
    exe = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix='ws-archive-') as temp:
        folder = os.path.join(temp, '压缩包')
        shutil.copytree(FIXTURES, folder)
        data = os.path.join(temp, 'data')
        os.makedirs(data)

        print('first pass')
        p = Plugin(exe, data, folder, page=4)
        done = p.wait_report(settled)
        check(done is not None, 'the card says it is done: ' + (done or {}).get('text', '(nothing)'))
        check(len(p.calls) >= 3 and p.calls[1].get('cursor') == 4, 'list_files was asked a page at a time (%d calls)' % len(p.calls))
        check({'zip', '7z', 'rar', 'tar', 'tgz', 'gz', 'xz'} <= set(p.calls[0]['extensions']), 'the types asked for: ' + ' '.join(p.calls[0]['extensions']))
        check(p.calls[0].get('include_system') is False and 'folders' not in p.calls[0], 'everywhere but system folders, by default')
        text = (done or {}).get('text', '')
        # 9 listed or partial (4 zip, 3 7z... see the fixtures), 2 with locked names, 1 not an archive.
        check('2 个加了密码看不到文件名' in text and '1 个不是压缩包或已损坏' in text and '1 个只读到一部分' in text, 'what could not be read is counted on the card')
        check(any(r.get('tone') == 'busy' and 'progress' in r and r.get('actions', [{}])[0].get('id') == 'pause' for r in p.reports),
              'while reading: a bar and 暂停')
        check((done or {}).get('actions', [{}])[0].get('id') == 'refresh', 'when done: 立即更新')
        check(os.path.exists(os.path.join(data, 'archives.bin')), 'what it read is kept in its data folder')

        print('searching')
        found = rows(p.query('报价单'))
        check(len(found) >= 6 and all(r['title'] == '报价单.xlsx' for r in found), '报价单: the member in every kind of archive (%d rows)' % len(found))
        one = next((r for r in found if r['subtitle'].startswith(os.path.join(folder, 'names-gbk.zip'))), None)
        check(one is not None and one['subtitle'].endswith(' › 合同\\附件'), 'a row says the archive and the folder inside it: ' + (one or {}).get('subtitle', ''))
        check(not any(r['subtitle'].endswith(' › \\') or r['subtitle'].endswith(' › ') for r in found),
              'a member at the archive\'s top says the archive alone')
        check(one is not None and one.get('details') is True and 'open' not in one and one.get('icon') == 'ext:xlsx' and one.get('size') == 10,
              'a row is the member: its type, its size, shown by details')
        check(one is not None and one.get('copy') == os.path.join(folder, 'names-gbk.zip') + '\\合同\\附件\\报价单.xlsx', 'Ctrl+C copies where it is')
        locked = [r for r in found if '有密码' in r['subtitle']]
        check(len(locked) == 1 and 'locked-files.7z' in locked[0]['subtitle'], 'a member whose content is locked says so')
        check(not rows(p.query('helloworld 没有这个')), 'every word has to be there')
        check(len(rows(p.query('rar5 hello'))) == 1, 'a word may be the archive\'s own name')
        nothing = p.query('zzz没有zzz')
        check(not rows(nothing) and len(nothing.get('items', [])) == 1 and '没有找到' in nothing['items'][0]['title'], 'nothing found: one line that says so')
        check(nothing['items'][0].get('valid') is False, '... on which Enter does nothing')
        content = p.query('报价单', trigger='content', filters={'contentEnabled': True, 'contentWords': '报价单'})
        check(len(rows(content)) == len(found) and len(content.get('items', [])) == len(found), 'as a content provider: the same rows, no line of its own')
        check(not p.query('zzz没有zzz', trigger='content').get('items'), '... and none at all when nothing is found')
        within = p.query('报价单', trigger='content', filters={'folder': os.path.join(temp, '别处')})
        check(not within.get('items'), 'a folder chosen in the panel: only archives in it')
        typed = p.query('会议', trigger='content', filters={'extensions': ['md']})
        check(len(rows(typed)) >= 6 and not rows(p.query('会议', trigger='content', filters={'extensions': ['pdf']})), 'types chosen in the panel: by the member\'s name')
        empty = p.query('')
        check(len(empty.get('items', [])) == 1 and empty['items'][0].get('valid') is False and '已收录' in empty['items'][0]['title'], 'the keyword alone: how the index stands')

        print('actions and details')
        effects = p.action('open', one)
        check([e.get('type') for e in effects] == ['open', 'hide'] and effects[0].get('path', '').endswith('names-gbk.zip'), 'Enter opens the archive')
        effects = p.action('folder', one)
        check(effects and effects[0].get('type') == 'reveal' and effects[0].get('path', '').endswith('names-gbk.zip'), 'Shift+Enter shows it in its folder')
        effects = p.action('extract', one)
        target = effects[0].get('path', '') if effects else ''
        check(effects and effects[0].get('type') == 'open' and target.startswith(data) and os.path.basename(target) == '报价单.xlsx',
              'Ctrl+Enter unpacks the one member into the data folder: ' + target[len(data):])
        check(os.path.exists(target) and open(target, encoding='utf-8').read() == '报价单\n', '... with its content')
        effects = p.action('extract', locked[0])
        check(effects and effects[0].get('type') == 'notify' and '密码' in effects[0].get('text', '') and effects[-1].get('type') == 'keep',
              'a locked member: a notice, the window stays')
        details = p.call('details', {'item': one, 'text': '报价单'}) or {}
        check(details.get('file', '').startswith(data) and os.path.exists(details.get('file', '')), 'details: the member as a file of the data folder')
        check((p.call('details', {'item': one, 'text': '报价单'}) or {}).get('file') == details.get('file'), '... the same copy when asked again')
        details = p.call('details', {'item': locked[0], 'text': ''}) or {}
        check('file' not in details and '密码' in details.get('text', ''), 'details of a locked member: words, no file')
        check(any(r.get('icon') == 'ph:folder' and 'names-gbk.zip' in r['subtitle'] for r in rows(p.query('合同'))), 'a folder inside an archive is a row too')
        folder_row = next((r for r in rows(p.query('合同')) if r.get('icon') == 'ph:folder' and 'names-utf8.zip' in r['subtitle']), None)
        details = p.call('details', {'item': folder_row, 'text': ''}) or {} if folder_row else {}
        check('2026 年度采购合同.docx' in details.get('text', '') and '附件\\报价单.xlsx' in details.get('text', ''), 'details of a folder inside: what is in it')
        effects = p.action('extract', folder_row) if folder_row else []
        check(effects and effects[0].get('type') == 'notify' and '文件夹' in effects[0].get('text', ''), 'Ctrl+Enter on a folder inside: a notice')
        script = dict(one, data=dict(one['data'], member='notes\\run.ps1'))
        effects = p.action('extract', script)
        check(effects and effects[0].get('type') == 'notify' and '脚本' in effects[0].get('text', ''), 'programs and scripts are not unpacked to be run')

        print('changes')
        asked = len(p.calls)
        p.hidden.add('rar5.rar')  # deleted
        shutil.copyfile(os.path.join(folder, 'files.7z'), os.path.join(folder, 'names-gbk.zip'))  # replaced by another archive
        os.utime(os.path.join(folder, 'names-gbk.zip'), (time.time() + 5, time.time() + 5))
        again = p.refresh()
        check(again is not None and len(p.calls) > asked, '立即更新 looks again')
        check(not rows(p.query('helloworld')), 'an archive that is gone is forgotten')
        # The 7z in its place has no entries for its folders, the zip had one.
        check(folder_row is not None and not any('names-gbk.zip' in r['subtitle'] and r['icon'] == 'ph:folder' for r in rows(p.query('合同'))),
              'an archive that changed was read again')
        effects = p.action('open', dict(one, data=dict(one['data'], archive=os.path.join(folder, 'rar5-gone.rar'))))
        check(effects and effects[0].get('type') == 'notify' and '不在' in effects[0].get('text', ''), 'Enter on an archive no longer there: a notice')
        # Win顺's index still being built: nothing is forgotten for what it does not list yet.
        p.building = True
        p.hidden.update(n for n in os.listdir(folder) if n.endswith('.7z'))
        p.refresh(lambda r: 'progress' not in r)
        check(len(rows(p.query('会议纪要'))) >= 6, 'while Win顺\'s index is being built nothing is forgotten')
        p.building = False
        p.hidden.clear()

        print('settings')
        asked = len(p.calls)
        os.rename(os.path.join(folder, 'files.tar.gz'), os.path.join(folder, 'bundle.apk'))
        p.settings.update({'more': 'apk, .JAR', 'folders': temp + '\\压缩包\\;D:\\没有', 'system': True})
        p.after_pass(lambda: p.notify('settingsChanged', {'settings': p.settings}))
        last = p.calls[-1]
        check(len(p.calls) > asked and {'apk', 'jar'} <= set(last['extensions']), 'more types are asked for: ' + ' '.join(last['extensions'][-3:]))
        check(last.get('folders') == [temp + '\\压缩包', 'D:\\没有'] and last.get('include_system') is True, 'the folders and system folders too, as set')
        check(any('bundle.apk' in r['subtitle'] for r in rows(p.query('报价单'))), 'an archive under another name is read by what it is')
        gz = os.path.join(folder, 'single.gz')
        shutil.copyfile(os.path.join(folder, 'bundle.apk'), gz)
        p.refresh()
        check(not any('single.gz' in r['subtitle'] for r in rows(p.query('报价单'))), 'a .gz that is not .tar.gz is left alone')

        print('trouble and the end')
        p.fail_list = 'Unknown tool: list_files'
        warned = p.refresh(lambda r: r.get('tone') == 'warn')
        check(warned is not None and 'list_files' in warned.get('text', ''), 'no list from Win顺: the card says why: ' + (warned or {}).get('text', ''))
        check(len(rows(p.query('会议纪要'))) >= 6, '... and what it has still answers')
        code = p.stop()
        check(code == 0, 'shutdown: the process ends by itself (exit code %s)' % code)

        print('started again')
        # As if the process had ended in the middle of reading one archive.
        fatal = os.path.join(folder, 'names-utf8.zip')
        stat = os.stat(fatal)
        with open(os.path.join(data, 'reading.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('%d\n%d\n%s\n' % (stat.st_size, int(stat.st_mtime), fatal))
        p = Plugin(exe, data, folder)
        first = rows(p.query('会议纪要'))
        check(len(first) >= 6, 'what was read is there at once (%d rows before any pass)' % len(first))
        done = p.wait_report(settled)
        check(done is not None and not any('progress' in r for r in p.reports), 'nothing changed: no archive is read again')
        check(not os.path.exists(os.path.join(data, 'opened')) and not os.path.exists(os.path.join(data, 'preview')), 'last time\'s unpacked copies are gone')
        check(not any('names-utf8.zip' in r['subtitle'] for r in first) and '1 个打不开' in (done or {}).get('text', ''),
              'the archive the process ended on is left out, and counted: ' + (done or {}).get('text', ''))
        check(not os.path.exists(os.path.join(data, 'reading.txt')) and any('names-utf8.zip' in line for line in p.logs), '... once, with a line in the log')
        os.utime(fatal, (time.time() + 30, time.time() + 30))
        p.refresh()
        check(any('names-utf8.zip' in r['subtitle'] for r in rows(p.query('会议纪要'))), '... and read again once it changed')

        print('pause')
        since = len(p.reports)
        for name in os.listdir(folder):
            os.utime(os.path.join(folder, name), (time.time() + 60, time.time() + 60))  # all of them changed
        p.status('refresh')
        busy = p.wait_report(lambda r: 'progress' in r, since=since)
        check(busy is not None, 'changed archives are read again')
        p.status('pause')
        paused = p.wait_report(lambda r: r.get('tone') == 'idle' and r.get('actions', [{}])[0].get('id') == 'resume', since=since + 1, timeout=5)
        # A dozen tiny archives: it may be done before the click lands.
        if paused is None:
            check(p.wait_report(settled, since=since + 1) is not None, '(read before 暂停 landed) done')
        else:
            check('已暂停' in paused.get('text', ''), '暂停: the card says so and offers 继续')
            p.status('resume')
            check(p.wait_report(settled, since=since + 1) is not None, '继续: it goes on to the end')
        check(p.stop() == 0, 'shutdown')

    print('\n%d failed' % len(failures) if failures else '\nall held')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
