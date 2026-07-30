import os
import sys

DOCS_SPHINX_DIR  = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT     = os.path.abspath(os.path.join(DOCS_SPHINX_DIR, '..', '..'))
PGIPC_DEV_DIR    = os.path.join(PROJECT_ROOT, 'pgipc_dev')
KDOC_VENDOR_DIR  = os.path.join(PROJECT_ROOT, 'docs', 'kerneldoc-src')

# kerneldoc.py / automarkup.py live right next to this conf.py, but
# Sphinx does NOT put the confdir on sys.path automatically — we have
# to do it ourselves so `extensions = ['kerneldoc', 'automarkup']` below
# can actually be imported.
sys.path.insert(0, DOCS_SPHINX_DIR)

# kerneldoc.py reads this env var at import time to locate
# tools/lib/python/kdoc. Set it here too, in case sphinx-build is
# invoked directly without a wrapper script setting it first.
os.environ.setdefault('srctree', KDOC_VENDOR_DIR)

project   = 'PGIPC'
author    = 'Simon Gibson'
copyright = '2026, Simon Gibson'
release   = '0.0.1'

extensions = [
    'kerneldoc',    # pulls kernel-doc comments into Sphinx C-domain objects
    'automarkup',   # auto cross-links foo(), struct foo, &struct foo, etc.
]

# Base dir every `.. kernel-doc:: <path>` argument resolves against.
# This is what lets rst files write clean paths like
# `display/display_core.c` instead of long absolute paths.
kerneldoc_srctree = PGIPC_DEV_DIR

# Debug-only: silences/enables kernel-doc's own stderr chatter
kerneldoc_verbosity = 0

# Add compiler attributes/macros used in your headers here if you hit
# "Invalid C declaration" warnings during the build.
c_id_attributes = [
    '__packed',
    'PGIPC_DEF',
    'PGIPC_ATOMIC'
]

c_id_paren_attributes = [
    'PGIPC_ALIGNAS'
]

primary_domain = 'c'
highlight_language = 'c'
exclude_patterns = ['_build']
html_theme = 'alabaster'