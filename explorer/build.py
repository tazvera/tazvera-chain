#!/usr/bin/env python3
"""Assemble explorer/index.html (standalone) from page.src.html + a chain JSON.
Usage: python3 build.py [chain.json]   (default: demo_chain.json)"""
import json, sys, pathlib
here = pathlib.Path(__file__).parent
chain = json.loads((here / (sys.argv[1] if len(sys.argv) > 1 else "demo_chain.json")).read_text())
data = json.dumps(chain, separators=(",", ":")).replace("</", "<\\/")
page = (here / "page.src.html").read_text().replace("__DEMO_JSON__", data)
head = '<!doctype html>\n<html lang="hi">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n</head>\n<body>\n'
(here / "index.html").write_text(head + page + "\n</body>\n</html>\n")
print("wrote", here / "index.html")
