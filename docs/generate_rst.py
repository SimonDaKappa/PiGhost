# docs/generate_rst.py
#
# Walks pg_display/ and (re)writes docs/sphinx/*.rst with one
# `.. kernel-doc::` per .c/.h file found. Run this after adding a new
# client app or source file, then re-run sphinx-build.

import os

ROOT       = os.path.dirname(os.path.abspath(__file__))
WISP_PATH  = os.path.abspath(os.path.join(ROOT, '..', 'wisp'))
SPHINX_DIR = os.path.join(ROOT, 'sphinx')
DOCUMENT_CLIENTS = False
IGNORED_DIRECTORIES = ["build", "build-tsan"]

def find_sources(subdir):
    out = []
    for dirpath, dirs, files in os.walk(subdir):
        dirs[:] = [d for d in dirs if d not in IGNORED_DIRECTORIES]
        for f in sorted(files):
            if f.endswith(('.c', '.h')):
                rel = os.path.relpath(os.path.join(dirpath, f), WISP_PATH)
                out.append(rel.replace(os.sep, '/'))
    return sorted(out)


def write_page(path, title, sources):
    with open(path, 'w') as fh:
        fh.write(f"{title}\n{'=' * len(title)}\n\n")
        fh.writelines(f".. kernel-doc:: {src}\n" for src in sources)

def main():
    write_page(os.path.join(SPHINX_DIR, 'wisp_protocol.rst'), 'Wisp Protocol',
               find_sources(os.path.join(WISP_PATH, 'protocol')))

    write_page(os.path.join(SPHINX_DIR, 'wisp_server.rst'), 'Wisp Server',
               find_sources(os.path.join(WISP_PATH, 'server')))

    write_page(os.path.join(SPHINX_DIR, 'wisp_client_sdk.rst'), 'Wisp Client SDK',
               find_sources(os.path.join(WISP_PATH, 'client_sdk')))

if __name__ == '__main__':
    main()