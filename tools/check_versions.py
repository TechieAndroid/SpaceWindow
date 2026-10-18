#!/usr/bin/env python3
"""Checks that every file stating SpaceWindow's version agrees with VERSION.

CMake and build.ps1 read VERSION themselves; the Windows resources, the app
manifest, the WinUI window's About text and the AppStream metainfo state it
in their own formats.  Run by CTest (VersionsAgree):

    python3 tools/check_versions.py [repository root]
"""
import pathlib
import re
import sys


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent)
    version = (root / 'VERSION').read_text(encoding='utf-8').strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        print(f'VERSION: "{version}" is not major.minor.patch')
        return 1
    four = version + '.0'
    commas = four.replace('.', ',')

    # (file, pattern, what the pattern must find)
    checks = [
        ('src/App/SpaceWindow.rc', r'FILEVERSION\s+([\d,]+)', commas),
        ('src/App/SpaceWindow.rc', r'PRODUCTVERSION\s+([\d,]+)', commas),
        ('src/App/SpaceWindow.rc', r'VALUE "FileVersion", "([^"]+)"', four),
        ('src/App/SpaceWindow.rc', r'VALUE "ProductVersion", "([^"]+)"', four),
        ('src/App/app.manifest', r'<assemblyIdentity version="([^"]+)"', four),
        ('src/App/MainWindowShared.h', r'kVersion\[\] = L"([^"]+)"', version),
        ('packaging/linux/io.github.techieandroid.SpaceWindow.metainfo.xml', r'<release version="([^"]+)"', version),
    ]
    problems = []
    for name, pattern, wanted in checks:
        path = root / name
        if not path.exists():
            problems.append(f'{name}: missing')
            continue
        found = re.search(pattern, path.read_text(encoding='utf-8'))
        if not found:
            problems.append(f'{name}: no version found ({pattern})')
        elif found.group(1) != wanted:
            problems.append(f'{name}: {found.group(1)}, VERSION says {wanted}')
    for problem in problems:
        print(problem)
    if not problems:
        print(f'{len(checks)} versions agree with VERSION ({version})')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
