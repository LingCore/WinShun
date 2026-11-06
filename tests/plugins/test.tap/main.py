"""Local test fixture: records host notifications only in its private data dir."""
import json
import sys
from pathlib import Path


def send(message):
    print(json.dumps({"jsonrpc": "2.0", **message}), flush=True)


data = None
received = []
for line in sys.stdin:
    message = json.loads(line)
    method = message.get("method")
    params = message.get("params", {})
    if method == "initialize":
        data = Path(params["dataDir"])
        data.mkdir(parents=True, exist_ok=True)
        send({"id": message["id"], "result": {"protocolVersion": params["protocolVersion"], "name": "tap fixture"}})
        send({"id": 215001, "method": "winshun/tool", "params": {
            "name": "register_hotkeys", "arguments": {"keys": ["Ctrl+Alt+Shift+F24"]}}})
    elif method == "hotkey":
        received.append(params["key"])
        target = data / "received.json"
        temporary = data / "received.tmp"
        temporary.write_text(json.dumps(received), encoding="utf-8")
        temporary.replace(target)
    elif method == "query":
        send({"id": message["id"], "result": {"items": []}})
    elif method == "shutdown":
        send({"id": message["id"], "result": None})
        break
