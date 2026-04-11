#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius
#
# Strip YAML front-matter from a Markdown file and write the result to stdout.
# Usage: strip-frontmatter.py <input.md>

import re
import sys

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <input.md>", file=sys.stderr)
    sys.exit(1)

text = open(sys.argv[1]).read()
text = re.sub(r"^---\n.*?\n---\n*", "", text, count=1, flags=re.DOTALL)
sys.stdout.write(text)
