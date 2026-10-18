#!/usr/bin/env python3
"""Checks that the Windows (MSBuild) and Linux (CMake) builds compile the same
shared sources: a file added to one build and forgotten in the other fails
here rather than on the other platform.  The files only one build compiles
are listed below, each with why.  Run by CTest (SourceListsAgree):

    python3 tools/check_sources.py [repository root]
"""
import pathlib
import re
import sys

PAIRS = [
    {
        'msbuild': 'src/Core/Core.vcxproj',
        'cmake': 'src/Core/CMakeLists.txt',
        'windows': {
            'pch.cpp',             # MSVC's precompiled header (CMake makes its own)
            'FolderWatch.cpp',     # ReadDirectoryChangesW
            'IoCancellation.cpp',  # CancelSynchronousIo
            'Parallel.cpp',        # the Windows thread pool
        },
        'linux': {
            'FolderTreePosix.cpp',      # getdents64 and statx
            'FolderWatchPosix.cpp',     # inotify
            'IoCancellationPosix.cpp',  # a signal that breaks off blocked system calls
            'ParallelPosix.cpp',        # std::thread
            'Posix.cpp',                # names as bytes, mount points, inode numbers
            'TrashPosix.cpp',           # the freedesktop.org trash
        },
    },
    {
        'msbuild': 'tests/CoreTests/CoreTests.vcxproj',
        'cmake': 'tests/CoreTests/CMakeLists.txt',
        'windows': {
            'pch.cpp',
            'Bench.cpp',        # reads NTFS drives directly
            'Compare.cpp',      # ditto
            'VolumeTests.cpp',  # ditto
        },
        'linux': {
            'BenchPosix.cpp',      # the benchmark here
            'PresenterTests.cpp',  # src/Presenter, which only the Linux build has for now
        },
    },
]


def msbuild_sources(text):
    return set(re.findall(r'<ClCompile Include="([^"]+\.cpp)"', text))


def cmake_sources(text):
    sources = set()
    for line in text.splitlines():
        sources.update(re.findall(r'([\w./-]+\.cpp)\b', line.split('#', 1)[0]))
    return sources


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent)
    problems = []
    for pair in PAIRS:
        windows = msbuild_sources((root / pair['msbuild']).read_text(encoding='utf-8'))
        linux = cmake_sources((root / pair['cmake']).read_text(encoding='utf-8'))
        for name in sorted(windows - linux - pair['windows']):
            problems.append(f"{pair['cmake']}: {name} is missing (or list it as Windows-only in tools/check_sources.py)")
        for name in sorted(linux - windows - pair['linux']):
            problems.append(f"{pair['msbuild']}: {name} is missing (or list it as Linux-only in tools/check_sources.py)")
        for name in sorted(pair['windows'] & linux):
            problems.append(f"{pair['cmake']}: {name} is listed as Windows-only, but CMake builds it")
        for name in sorted(pair['linux'] & windows):
            problems.append(f"{pair['msbuild']}: {name} is listed as Linux-only, but MSBuild builds it")
        for name in sorted((pair['windows'] - windows) | (pair['linux'] - linux)):
            problems.append(f'tools/check_sources.py: {name} is listed, but no build has it')
    for problem in problems:
        print(problem)
    if not problems:
        print('The Windows and Linux builds compile the same shared sources')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
