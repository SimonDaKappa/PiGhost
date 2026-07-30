# docs/generate_rst.py
#
# Walks pgipc_dev/ and (re)writes docs/sphinx/*.rst with one
# `.. kernel-doc::` per .c/.h file found. Run this after adding a new
# writer app or source file, then re-run sphinx-build.

import os

ROOT       = os.path.dirname(os.path.abspath(__file__))
PGIPC_DEV  = os.path.abspath(os.path.join(ROOT, '..', 'pgipc_dev'))
SPHINX_DIR = os.path.join(ROOT, 'sphinx')
DOCUMENT_WRITERS = False
IGNORED_DIRECTORIES = ["build", "build-tsan"]

def find_sources(subdir):
    out = []
    for dirpath, dirs, files in os.walk(subdir):
        dirs[:] = [d for d in dirs if d not in IGNORED_DIRECTORIES]
        for f in sorted(files):
            if f.endswith(('.c', '.h')):
                rel = os.path.relpath(os.path.join(dirpath, f), PGIPC_DEV)
                out.append(rel.replace(os.sep, '/'))
    return sorted(out)


def write_page(path, title, sources):
    with open(path, 'w') as fh:
        fh.write(f"{title}\n{'=' * len(title)}\n\n")
        fh.writelines(f".. kernel-doc:: {src}\n" for src in sources)

def main():
    write_page(os.path.join(SPHINX_DIR, 'libpgipc.rst'), 'libpgipc',
               find_sources(os.path.join(PGIPC_DEV, 'libpgipc')))

    write_page(os.path.join(SPHINX_DIR, 'display.rst'), 'display',
               find_sources(os.path.join(PGIPC_DEV, 'display')))

    if DOCUMENT_WRITERS:
        writers_dir = os.path.join(PGIPC_DEV, 'writers')
        writers_sphinx_dir = os.path.join(SPHINX_DIR, 'writers')
        os.makedirs(writers_sphinx_dir, exist_ok=True)

        names = sorted(d for d in os.listdir(writers_dir)
                    if os.path.isdir(os.path.join(writers_dir, d)))

        for name in names:
            write_page(os.path.join(writers_sphinx_dir, f'{name}.rst'), name,
                    find_sources(os.path.join(writers_dir, name)))

        with open(os.path.join(writers_sphinx_dir, 'index.rst'), 'w') as fh:
            fh.write("Writers\n=======\n\n.. toctree::\n   :maxdepth: 1\n\n")
            for name in names:
                fh.write(f"   {name}\n")


if __name__ == '__main__':
    main()