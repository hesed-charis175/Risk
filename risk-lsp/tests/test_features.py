#!/usr/bin/env python3
"""End-to-end checks for risk-lsp.  Usage: test_features.py <path-to-risk-lsp> [repo-root]"""
import os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lspclient import Client

BIN = sys.argv[1]
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", ".."))
c = Client(BIN, ROOT)

SRC = '''import "@core";

block Vec { x: i32; y: i32 = 0; }
opts Color { Red, Green = 5, Blue }

func area(w: i32, h: i32 = 2) -> i32 {
    return w * h;
}

func main(args: Array<string>) -> i32 {
    v: Vec = { x = 1 };
    c: Color = Color.Green;
    names: Array<string> = strSplit("a,b", ",");
    names.push("z");
    n: i32 = area(3, );
    s: string = strUpper("hi");
    ioPrintln(v.x, c, n, s, length(names));
    return 0;
}
'''
LINES = SRC.split("\n")

def at(word, nth=0, off=0):
    k = 0
    for i, l in enumerate(LINES):
        j = -1
        while True:
            j = l.find(word, j + 1)
            if j < 0: break
            if k == nth: return i, j + off
            k += 1
    raise KeyError(word)

def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond: check.failed += 1
check.failed = 0

u = c.open(os.path.join(ROOT, "examples", "_virtual_test.risk"), SRC)
check("clean file has no diagnostics", c.diags(u) == [])

h = c.req("textDocument/hover", c.pos(u, *at("strUpper")))
check("hover on stdlib function shows signature", "func strUpper(s: string) -> string" in h["contents"]["value"])
h = c.req("textDocument/hover", c.pos(u, *at("push")))
check("hover on Array method", "built-in method of `Array`" in h["contents"]["value"])
h = c.req("textDocument/hover", c.pos(u, *at("i32", 3)))
check("hover on primitive type", "signed 32-bit" in h["contents"]["value"])

d = c.req("textDocument/definition", c.pos(u, *at("strUpper")))
check("go-to-definition crosses into stdlib", d["uri"].endswith("stdlib/core/string.risk"))
d = c.req("textDocument/definition", c.pos(u, 0, 10))
check("go-to-definition on import line opens the module", d["uri"].endswith("core.risk"))

sh = c.req("textDocument/signatureHelp", c.pos(u, *at("area(3, ", 0, 8)))
check("signature help tracks active parameter", sh["activeParameter"] == 1)

r = c.req("textDocument/completion", c.pos(u, *at("names.push", 0, 6)))
check("completion after Array<string>.", {"push", "pop", "clear"} <= {i["label"] for i in r["items"]})
r = c.req("textDocument/completion", c.pos(u, *at("Color.Green", 0, 6)))
check("completion after opts name", [i["label"] for i in r["items"]] == ["Red", "Green", "Blue"])
r = c.req("textDocument/completion", c.pos(u, 0, 9))
check("completion after import @", "core" in [i["label"] for i in r["items"]])

refs = c.req("textDocument/references", {**c.pos(u, *at("area", 1)), "context": {"includeDeclaration": True}})
check("find references", sorted(x["range"]["start"]["line"] for x in refs) == [5, 14])
ren = c.req("textDocument/rename", {**c.pos(u, *at("area", 1)), "newName": "areaOf", })
check("rename edits both sites", sum(len(v) for v in ren["changes"].values()) == 2)
syms = c.req("textDocument/documentSymbol", {"textDocument": {"uri": u}})
check("document symbols", {"Vec", "Color", "area", "main"} <= {s["name"] for s in syms})

bad = SRC.replace('area(3, )', 'area("x")')
u2 = c.open(os.path.join(ROOT, "examples", "_virtual_bad.risk"), bad)
dg = c.diags(u2)
check("type errors reported with source 'riskc'", bool(dg) and dg[0]["source"] == "riskc")

random.seed(7)
ver = 1
for _ in range(120):
    k = random.randrange(1, len(SRC))
    ver += 1
    c.change(u, SRC[:k], ver)
    c.req("textDocument/documentSymbol", {"textDocument": {"uri": u}}, 20)
check("survives 120 truncated edits", c.p.poll() is None)

c.close()
print("\n%d failure(s)" % check.failed)
sys.exit(1 if check.failed else 0)
