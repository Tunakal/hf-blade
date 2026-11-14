"""Builds the release zip from build/ (run build.bat first): dist/HF-Blade-<version>.zip.

The zip holds the files that go next to METAL GEAR RISING REVENGEANCE.exe, plus the README and the
license. The version is read from src/config.h (cfg::kVersion).

Usage: py -3 tools/make_release.py
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    cfg = open(os.path.join(ROOT, 'src', 'config.h'), encoding='utf-8').read()
    version = re.search(r'kVersion\[\]\s*=\s*"([^"]+)"', cfg).group(1)
    files = [
        ('build/dinput8.dll', 'dinput8.dll'),
        ('build/hfblade-launcher.exe', 'hfblade-launcher.exe'),
        ('hfblade.ini', 'hfblade.ini'),
        ('README.md', 'README.md'),
        ('LICENSE', 'LICENSE.txt'),
    ]
    missing = [src for src, _ in files if not os.path.isfile(os.path.join(ROOT, src))]
    if missing:
        sys.exit('missing: ' + ', '.join(missing) + ' (run build.bat first)')
    os.makedirs(os.path.join(ROOT, 'dist'), exist_ok=True)
    out = os.path.join(ROOT, 'dist', f'HF-Blade-{version}.zip')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for src, name in files:
            z.write(os.path.join(ROOT, src), name)
    print(f'{out} ({os.path.getsize(out) // 1024} KB)')


if __name__ == '__main__':
    main()
