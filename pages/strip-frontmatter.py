#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius
#
# Prepare a Markdown file for Pandoc HTML conversion:
#   - Strip YAML front-matter
#   - Optionally rewrite .md links to .html (for GitHub Pages deployment)
#
# Usage: strip-frontmatter.py [--rewrite-links] <input.md>

import re
import sys

rewrite_links = False
args = sys.argv[1:]

if args and args[0] == "--rewrite-links":
    rewrite_links = True
    args = args[1:]

if len(args) != 1:
    print(f"Usage: {sys.argv[0]} [--rewrite-links] <input.md>", file=sys.stderr)
    sys.exit(1)

text = open(args[0]).read()
text = re.sub(r"^---\n.*?\n---\n*", "", text, count=1, flags=re.DOTALL)

if rewrite_links:
    # Rewrite relative .md links to .html (e.g. BUILD.md -> BUILD.html).
    # Skip absolute URLs (http:// or https://).
    text = re.sub(
        r"(\[[^\]]*\]\((?!https?://)(?:[^)]*?))\.md([)#])", r"\1.html\2", text
    )

sys.stdout.write(text)
