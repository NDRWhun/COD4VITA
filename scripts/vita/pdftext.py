#!/usr/bin/env python3
"""Extract a vendor SDK PDF to text so the docs are greppable.

  python pdftext.py "<sdk>/Graphics/libgxm-Overview_e.pdf" gxm-overview.txt
"""

import sys

from pypdf import PdfReader


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    src, out = sys.argv[1], sys.argv[2]
    reader = PdfReader(src)
    with open(out, "w", encoding="utf-8") as f:
        for i, page in enumerate(reader.pages):
            f.write("\n\n=== PAGE %d ===\n" % (i + 1))
            f.write(page.extract_text() or "")
    print("%d pages -> %s" % (len(reader.pages), out))


if __name__ == "__main__":
    main()
