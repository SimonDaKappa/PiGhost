# docs/generate_rst.py
#
# Walks pg_display/ and (re)writes docs/sphinx/*.rst with one
# `.. kernel-doc::` per .c/.h file found. Run this after adding a new
# client app or source file, then re-run sphinx-build.

import os

ROOT       = os.path.dirname(os.path.abspath(__file__))
pg_display  = os.path.abspath(os.path.join(ROOT, '..', 'pg_display'))
SPHINX_DIR = os.path.join(ROOT, 'sphinx')
DOCUMENT_CLIENTS = False
IGNORED_DIRECTORIES = ["build", "build-tsan"]

def find_sources(subdir):
    out = []
    for dirpath, dirs, files in os.walk(subdir):
        dirs[:] = [d for d in dirs if d not in IGNORED_DIRECTORIES]
        for f in sorted(files):
            if f.endswith(('.c', '.h')):
                rel = os.path.relpath(os.path.join(dirpath, f), pg_display)
                out.append(rel.replace(os.sep, '/'))
    return sorted(out)


def write_page(path, title, sources):
    with open(path, 'w') as fh:
        fh.write(f"{title}\n{'=' * len(title)}\n\n")
        fh.writelines(f".. kernel-doc:: {src}\n" for src in sources)

def main():
    write_page(os.path.join(SPHINX_DIR, 'libpgdp.rst'), 'libpgdp',
               find_sources(os.path.join(pg_display, 'libpgdp')))

    write_page(os.path.join(SPHINX_DIR, 'server.rst'), 'server',
               find_sources(os.path.join(pg_display, 'server')))

    if DOCUMENT_CLIENTS:
        clients_dir = os.path.join(pg_display, 'clients')
        clients_sphinx_dir = os.path.join(SPHINX_DIR, 'clients')
        os.makedirs(clients_sphinx_dir, exist_ok=True)

        names = sorted(d for d in os.listdir(clients_dir)
                    if os.path.isdir(os.path.join(clients_dir, d)))

        for name in names:
            write_page(os.path.join(clients_sphinx_dir, f'{name}.rst'), name,
                    find_sources(os.path.join(clients_dir, name)))

        with open(os.path.join(clients_sphinx_dir, 'index.rst'), 'w') as fh:
            fh.write("Clients\n=======\n\n.. toctree::\n   :maxdepth: 1\n\n")
            for name in names:
                fh.write(f"   {name}\n")


if __name__ == '__main__':
    main()