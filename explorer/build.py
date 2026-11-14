#!/usr/bin/env python3
"""Assemble explorer/index.html (standalone) from page.src.html and script.src.js's chains.
Usage: python3 build.py            -> uses demo_chain.json and demo_safety_chain.json
       python3 build.py a.json b.json   -> a.json is the plain demo, b.json the safety demo"""
import json, sys, pathlib
here = pathlib.Path(__file__).parent
def load(name):
    data = json.dumps(json.loads((here / name).read_text()), separators=(",", ":"))
    return data.replace("</", "<\\/")
a = sys.argv[1] if len(sys.argv) > 1 else "demo_chain.json"
b = sys.argv[2] if len(sys.argv) > 2 else "demo_safety_chain.json"
page = (here / "page.src.html").read_text().replace("__DEMO_JSON__", load(a)).replace("__DEMO_SAFETY_JSON__", load(b))
head = '<!doctype html>\n<html lang="hi">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n</head>\n<body>\n'
(here / "index.html").write_text(head + page + "\n</body>\n</html>\n")
print("wrote", here / "index.html")
