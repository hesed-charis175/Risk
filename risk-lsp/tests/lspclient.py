import json, subprocess, sys, os, time, select

class Client:
    def __init__(self, binary, root, env=None):
        e = dict(os.environ)
        if env: e.update(env)
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=e)
        self.id = 0
        self.notes = []
        self.root = root
        r = self.req("initialize", {"rootUri": "file://" + root, "capabilities": {}})
        self.caps = r["capabilities"]
        self.note("initialized", {})

    def send(self, obj):
        b = json.dumps(obj).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
        self.p.stdin.flush()

    def read_msg(self, timeout=15):
        fd = self.p.stdout
        end = time.time() + timeout
        hdr = b""
        while b"\r\n\r\n" not in hdr:
            if time.time() > end: return None
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                ch = os.read(fd.fileno(), 1)
                if not ch: return None
                hdr += ch
        n = int([l for l in hdr.decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
        body = b""
        while len(body) < n:
            body += os.read(fd.fileno(), n - len(body))
        return json.loads(body)

    def req(self, method, params, timeout=15):
        self.id += 1
        rid = self.id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            m = self.read_msg(timeout)
            if m is None: raise TimeoutError(method)
            if m.get("id") == rid and "method" not in m:
                if "error" in m: raise RuntimeError(m["error"])
                return m["result"]
            self.notes.append(m)

    def note(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def open(self, path, text=None, version=1):
        if text is None: text = open(path).read()
        uri = "file://" + path
        self.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "risk", "version": version, "text": text}})
        return uri

    def change(self, uri, text, version):
        self.note("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                                              "contentChanges": [{"text": text}]})

    def diags(self, uri, timeout=10):
        end = time.time() + timeout
        got = None
        while True:
            for m in list(self.notes):
                if m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri:
                    got = m["params"]["diagnostics"]
                    self.notes.remove(m)
            if got is not None: return got
            m = self.read_msg(max(0.1, end - time.time()))
            if m is None:
                if time.time() > end: return None
                continue
            self.notes.append(m)

    def pos(self, uri, line, ch):
        return {"textDocument": {"uri": uri}, "position": {"line": line, "character": ch}}

    def close(self):
        try:
            self.req("shutdown", None, 5)
            self.note("exit", None)
        except Exception:
            pass
        try: self.p.wait(3)
        except Exception: self.p.kill()
