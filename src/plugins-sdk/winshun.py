# winshun.py — the Win顺 (WinShun) plugin SDK for Python 3. No dependencies.
#
# Copy this file next to your main.py.
#
# A once plugin (runtime.mode = "once"; a process per query) is a few lines:
#
#     import winshun
#     q = winshun.query()                       # from the WS_* environment
#     winshun.reply([winshun.item("hi", "你好，" + q.text, copy=q.text)])
#
# A resident plugin (runtime.mode = "resident"; one process, JSON-RPC lines
# on stdin and stdout) answers a function per request:
#
#     import winshun
#     def on_query(q, plugin):
#         return [winshun.item("hi", "你好，" + q.text, copy=q.text)]
#     winshun.serve(on_query)
#
# Protocol: docs/plugins.md. Only JSON-RPC may go to stdout; print() to
# stderr (winshun.debug) goes to plugins.log and the developer mode's pane.

import json
import os
import sys
import threading

__all__ = ["Query", "ActionRequest", "item", "action", "query", "settings", "reply", "effects",
           "is_action", "action_request", "serve", "Plugin", "debug",
           "hide", "keep", "notify", "copy", "set_query", "open_path", "open_url", "replace"]

_out = sys.stdout.buffer


def debug(text):
    """A line to stderr: plugins.log and, in developer mode, the log pane."""
    sys.stderr.write(str(text) + "\n")
    sys.stderr.flush()


# ---- items and effects ---------------------------------------------------------

def item(id, title, subtitle=None, icon=None, score=None, copy=None, autocomplete=None, path=None, url=None,
         preview=None, data=None, actions=None, valid=True, match=None, **more):
    """One result row (docs/plugins.md, "item"). `path` or `url` is the
    declarative default action (Win顺 opens it without asking back);
    otherwise Enter sends an `action` request (once plugins: WS_ACTION)."""
    row = {"id": str(id), "title": str(title)}
    if subtitle:
        row["subtitle"] = str(subtitle)
    if icon:
        row["icon"] = icon
    if score is not None:
        row["score"] = int(score)
    if copy is not None:
        row["copy"] = str(copy)
    if autocomplete:
        row["autocomplete"] = autocomplete
    if path:
        row["open"] = {"path": path}
    elif url:
        row["open"] = {"url": url}
    if preview:
        row["preview"] = {"text": preview}
    if data is not None:
        row["data"] = data
    if actions:
        row["actions"] = actions
    if not valid:
        row["valid"] = False
    if match:
        row["match"] = match
    row.update(more)
    return row


def action(id, title, key="Enter"):
    """An action of an item: {"id", "title", "key"}; key is Enter, Ctrl+Enter or Shift+Enter."""
    return {"id": id, "title": title, "key": key}


def hide():
    return {"type": "hide"}


def keep():
    return {"type": "keep"}


def notify(text):
    return {"type": "notify", "text": str(text)}


def copy(text):
    return {"type": "copy", "text": str(text)}


def set_query(text):
    return {"type": "setQuery", "text": str(text)}


# open_path / open_url (and an item's open=...): Win顺 carries these out. A
# plugin without the "processes" permission may only open a folder or an
# ordinary (non-program, non-script) file, and only http/https/mailto URLs,
# with no args/cwd; anything else is refused with a footer notice. Declare
# "processes" to open a program or a non-web link.
def open_path(path):
    return {"type": "open", "path": path}


def open_url(url):
    return {"type": "open", "url": url}


def replace(items):
    return {"type": "replace", "items": list(items)}


# ---- requests --------------------------------------------------------------------

class Query:
    """A query: `text` is what came after the keyword (the whole input for
    a regex or push trigger), `raw` the whole input, `match` a regex
    trigger's groups, `command` the command's id, `trigger` keyword |
    regex | push, `query_id` the number Win顺 gave it."""

    def __init__(self, text="", raw="", command="", trigger="keyword", keyword="", match=None, query_id=0, limit=200, filters=None):
        self.text = text
        self.raw = raw
        self.command = command
        self.trigger = trigger
        self.keyword = keyword
        self.match = list(match or [])
        self.query_id = query_id
        self.limit = limit
        self.filters = dict(filters or {})

    def __repr__(self):
        return "Query(%r, command=%r, trigger=%r)" % (self.text, self.command, self.trigger)


class ActionRequest:
    """An action on an item: `action` is the action's id ("default" when
    the command declares none), `item` the item as the plugin gave it
    (its `data` included), `text` the words in the box. A `files` trigger
    chosen on Win顺's result menu: `trigger` is "files", `action` "files",
    `files` the chosen files and folders (full paths), no item."""

    def __init__(self, action="default", item=None, command="", text="", trigger="", files=None):
        self.action = action
        self.item = item or {}
        self.command = command
        self.text = text
        self.trigger = trigger
        self.files = list(files or [])


# ---- once plugins ---------------------------------------------------------------

def _env(name, default=""):
    return os.environ.get(name, default)


def query():
    """The query a once plugin was started for, from the WS_* environment."""
    try:
        match = json.loads(_env("WS_MATCH", "[]") or "[]")
    except ValueError:
        match = []
    try:
        filters = json.loads(_env("WS_FILTERS", "{}") or "{}")
    except ValueError:
        filters = {}
    return Query(text=_env("WS_QUERY"), raw=_env("WS_TEXT"), command=_env("WS_COMMAND"),
                 trigger=_env("WS_TRIGGER", "keyword"), keyword=_env("WS_KEYWORD"), match=match, filters=filters)


def settings():
    """The plugin's settings (manifest `settings`, with the user's values)."""
    try:
        return json.loads(_env("WS_SETTINGS", "{}") or "{}")
    except ValueError:
        return {}


def plugin_dir():
    return _env("WS_PLUGIN_DIR") or os.path.dirname(os.path.abspath(sys.argv[0]))


def data_dir():
    """The folder the plugin may write to (<id>.data); made when missing."""
    folder = _env("WS_DATA_DIR") or os.path.join(plugin_dir(), "data")
    os.makedirs(folder, exist_ok=True)
    return folder


def locale():
    return _env("WS_LOCALE", "zh")


def is_action():
    """True when the process was started for an action, not a query."""
    return bool(_env("WS_ACTION"))


def action_request():
    try:
        item_value = json.loads(_env("WS_ITEM", "{}") or "{}")
    except ValueError:
        item_value = {}
    try:
        files = json.loads(_env("WS_FILES", "[]") or "[]")
    except ValueError:
        files = []
    trigger = _env("WS_TRIGGER")
    return ActionRequest(action=_env("WS_ACTION", "default"), item=item_value, command=_env("WS_COMMAND"), text=_env("WS_QUERY"),
                         trigger=trigger if trigger == "files" else "", files=files if isinstance(files, list) else [])


_write_lock = threading.Lock()


def _write(obj):
    # One whole line at a time: a helper (refresh(), progress()) may be
    # called from another thread while the handler answers.
    line = json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n"
    with _write_lock:
        _out.write(line)
        _out.flush()


def reply(items, rerun=0, cache=0):
    """A once plugin's whole answer: the items (dicts from item()), then
    the process ends. `rerun`: seconds until Win顺 asks again while the
    list is still shown (a changing list; at least 1 for a once plugin);
    `cache`: seconds the answer may be reused for the same text."""
    result = {"items": list(items)}
    if rerun:
        result["rerun"] = rerun
    if cache:
        result["cache"] = cache
    _write(result)


def effects(*effect_list):
    """A once plugin's answer to an action: effects in order (hide() is
    the default when nothing is printed)."""
    _write({"effects": list(effect_list)})


# ---- resident plugins -----------------------------------------------------------

class Plugin:
    """The resident loop. Given to the handlers so they can send partial
    results, log, notify, show progress and call Win顺's tools."""

    def __init__(self, on_query, on_action=None, on_settings=None, on_cancel=None, name="plugin"):
        self._on_query = on_query
        self._on_action = on_action
        self._on_settings = on_settings
        self._on_cancel = on_cancel
        self._name = name
        self._next_id = 1
        self._queued = []
        self._in = sys.stdin.buffer
        self.settings = {}
        self.locale = "zh"
        self.theme = "light"
        self.plugin_dir = plugin_dir()
        self.data_dir = ""

    # -- to Win顺 --
    def _send(self, message):
        message["jsonrpc"] = "2.0"
        _write(message)

    def _notify(self, method, params):
        self._send({"method": method, "params": params})

    def partial(self, query_id, items, replace_all=False):
        """Rows shown before the final answer (keyword mode shows them at once)."""
        self._notify("query/partial", {"queryId": query_id, "items": list(items), "replace": replace_all})

    def log(self, text, level="info"):
        self._notify("winshun/log", {"level": level, "text": str(text)})

    def notify_user(self, text):
        self._notify("winshun/notify", {"text": str(text)})

    def toast(self, title, text="", click=None):
        """A system notification (needs the "notifications" permission;
        without it the text goes to the launcher's footer). At most one
        every 2 seconds is shown, the newest of those waiting; keep file
        names and other private words out: it stays in the Action Center.
        `click`: effects for a click on it, e.g. [set_query("kw ")]."""
        self._notify("winshun/notify", {"toast": True, "title": str(title), "text": str(text), "click": list(click or [])})

    def progress(self, query_id, text):
        self._notify("winshun/progress", {"queryId": query_id, "text": str(text)})

    def refresh(self):
        """What the launcher shows changed by itself (a request came, a job
        ended): when the last answer asked for `rerun` and the list is still
        shown, Win顺 asks again now instead of when `rerun` is up. Call it
        after the state the next query reads is written."""
        self._notify("winshun/refresh", {})

    def tool(self, name, **arguments):
        """One of Win顺's tools (search_files, search_content, recent_files,
        list_folder, get_file_info, open_item, show_in_folder,
        clipboard_history, clipboard_get, index_status), as the manifest's
        permissions allow. Returns the result, or {"error": {code, message}}."""
        call_id = self._next_id
        self._next_id += 1
        self._send({"id": call_id, "method": "winshun/tool", "params": {"name": name, "arguments": arguments}})
        while True:
            line = self._in.readline()
            if not line:
                return {"error": {"code": -32000, "message": "Win顺 went away"}}
            try:
                message = json.loads(line.decode("utf-8"))
            except ValueError:
                continue
            if isinstance(message, dict) and "method" not in message and message.get("id") == call_id:
                return {"error": message["error"]} if "error" in message else message.get("result")
            self._queued.append(line)

    # -- the loop --
    def serve(self):
        while True:
            line = self._queued.pop(0) if self._queued else self._in.readline()
            if not line:
                return 0
            try:
                message = json.loads(line.decode("utf-8"))
            except ValueError:
                debug("winshun.py: not JSON: " + line[:200].decode("utf-8", "replace"))
                continue
            if not isinstance(message, dict):
                continue
            if self._handle(message):
                return 0

    def _handle(self, message):
        method = message.get("method")
        msg_id = message.get("id")
        params = message.get("params") or {}
        if method == "initialize":
            self.settings = params.get("settings") or {}
            self.locale = params.get("locale") or "zh"
            self.theme = params.get("theme") or "light"
            self.plugin_dir = params.get("pluginDir") or self.plugin_dir
            self.data_dir = params.get("dataDir") or ""
            self._send({"id": msg_id, "result": {"protocolVersion": "1", "name": self._name}})
        elif method == "query":
            q = Query(text=params.get("text", ""), raw=params.get("raw", ""), command=params.get("command", ""),
                      trigger=params.get("trigger", "keyword"), keyword=params.get("keyword", ""),
                      match=params.get("match") or [], query_id=params.get("queryId", 0), limit=params.get("limit", 200),
                      filters=params.get("filters") or {})
            try:
                result = self._on_query(q, self)
            except Exception as e:  # the row says what went wrong; the process lives on
                debug("winshun.py: query failed: %r" % (e,))
                self._send({"id": msg_id, "error": {"code": -32000, "message": str(e)}})
                return False
            if isinstance(result, dict) and "items" in result:
                self._send({"id": msg_id, "result": result})
            else:
                self._send({"id": msg_id, "result": {"items": list(result or [])}})
        elif method == "action":
            a = ActionRequest(action=params.get("action", "default"), item=params.get("item") or {},
                              command=params.get("command", ""), text=params.get("text", ""),
                              trigger=params.get("trigger", ""), files=params.get("files") or [])
            if self._on_action is None:
                text = a.item.get("copy", a.item.get("title", ""))
                result = [copy(text), notify("已复制" if self.locale != "en" else "Copied"), hide()]
            else:
                try:
                    result = self._on_action(a, self)
                except Exception as e:
                    self._send({"id": msg_id, "error": {"code": -32000, "message": str(e)}})
                    return False
            self._send({"id": msg_id, "result": {"effects": list(result or [hide()])}})
        elif method == "cancel":
            if self._on_cancel:
                self._on_cancel(params.get("queryId", 0), self)
        elif method == "settingsChanged":
            self.settings = params.get("settings") or {}
            if self._on_settings:
                self._on_settings(self.settings, self)
        elif method == "shutdown":
            self._send({"id": msg_id, "result": {}})
            return True
        elif msg_id is not None:
            self._send({"id": msg_id, "error": {"code": -32601, "message": "Method not found: %s" % method}})
        return False


def serve(on_query, on_action=None, on_settings=None, on_cancel=None, name="plugin"):
    """Runs a resident plugin: on_query(query, plugin) returns a list of
    items (or {"items": [...], "rerun": s, "cache": s}); on_action(request,
    plugin) returns a list of effects (default: copy the item); on_settings
    (settings, plugin) hears the user's changes; on_cancel(query_id, plugin)
    hears a query Win顺 no longer wants. Returns when Win顺 says shutdown."""
    return Plugin(on_query, on_action, on_settings, on_cancel, name).serve()
