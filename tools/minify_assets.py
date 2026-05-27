"""
PlatformIO pre-build script and CLI tool to minify and gzip web assets.

Usage (CLI):
    python3 tools/minify_assets.py

PlatformIO integration: added as `extra_scripts = pre:tools/minify_assets.py`
The script will read from `data-src/` if present, otherwise falls back to `data/`.
It writes minified files and `.gz` compressed variants into `data/` for LittleFS upload.
"""
from __future__ import print_function
import os
import re
import gzip
import shutil
from pathlib import Path

TEXT_EXTS = {'.html', '.htm', '.js', '.css', '.txt'}
BINARY_EXTS = {'.png', '.jpg', '.jpeg', '.gif', '.svg', '.ico', '.woff', '.woff2', '.ttf'}

def minify_html(s: str) -> str:
    s = re.sub(r'<!--([\s\S]*?)-->', '', s)
    s = re.sub(r'>\s+<', '><', s)
    s = re.sub(r'\s{2,}', ' ', s)
    return s.strip()

def minify_css(s: str) -> str:
    s = re.sub(r'/\*([\s\S]*?)\*/', '', s)
    s = re.sub(r'\s{2,}', ' ', s)
    s = re.sub(r'\s*([{};:,])\s*', r'\1', s)
    return s.strip()

def minify_js(s: str) -> str:
    s = re.sub(r'/\*([\s\S]*?)\*/', '', s)
    # remove // comments but avoid http(s)://
    s = re.sub(r'(^|[^:\\])//[^\n\r]*', r'\1', s)
    s = re.sub(r'\s{2,}', ' ', s)
    return s.strip()

def process_file(src_path: Path, out_dir: Path):
    ext = src_path.suffix.lower()
    out_path = out_dir / src_path.name
    # Ensure out_dir exists
    out_dir.mkdir(parents=True, exist_ok=True)

    if ext in TEXT_EXTS or ext in {'.css', '.js', '.html'}:
        text = src_path.read_text(encoding='utf-8')
        if ext == '.css':
            text_min = minify_css(text)
        else:
            # JS and HTML minification via regex is unsafe (breaks ASI, inline scripts)
            # Just gzip the original — that provides the real size savings.
            text_min = text
        out_path.write_text(text_min, encoding='utf-8')
        # Write gzipped version
        gz_path = out_path.with_suffix(out_path.suffix + '.gz')
        # Use gzip.compress for compatibility and deterministic output
        gzbytes = gzip.compress(text_min.encode('utf-8'))
        with open(gz_path, 'wb') as gzf:
            gzf.write(gzbytes)
        print(f'Wrote {out_path.name} (+ {gz_path.name})')
    elif ext in BINARY_EXTS or src_path.is_file():
        # copy binaries unchanged
        shutil.copy2(src_path, out_path)
        print(f'Copied binary {out_path.name}')

def minify_project(project_dir: str):
    p = Path(project_dir)
    src_dir = p / 'data-src'
    fallback_dir = p / 'data'
    out_dir = p / 'data'

    if src_dir.exists() and src_dir.is_dir():
        source = src_dir
        print('Using data-src/ as source')
    else:
        source = fallback_dir
        print('data-src/ not found; using data/ as source')

    if not source.exists():
        print('No source data directory found; nothing to do.')
        return

    for entry in sorted(source.iterdir()):
        if entry.is_file():
            process_file(entry, out_dir)

    print('Minify complete.')


# --- PlatformIO integration ---
try:
    from SCons.Script import AddPreAction
    ImportError  # silence linter
except Exception:
    AddPreAction = None

def pre_build_action(source=None, target=None, env=None):
    project_dir = env['PROJECT_DIR'] if env and 'PROJECT_DIR' in env else os.getcwd()
    minify_project(project_dir)

if 'Import' in globals():
    try:
        Import('env')
        env.AddPreAction('buildprog', pre_build_action)
    except Exception:
        pass

if __name__ == '__main__':
    minify_project(os.getcwd())
