"""Writes the packs gear/tests/vfs_test.cpp reads into the folder given as the argument."""
import os
import sys
import zipfile

root = sys.argv[1]
os.makedirs(root, exist_ok=True)


def pack(name, files):
    with zipfile.ZipFile(os.path.join(root, name), 'w') as z:
        for path, (text, method) in files.items():
            z.writestr(path, text, method)


pack('a.gpak', {'ui/a.txt': ('stored', zipfile.ZIP_STORED), 'ui/B.txt': ('deflated ' * 500, zipfile.ZIP_DEFLATED)})
pack('data01.pak', {'data.txt': ('original', zipfile.ZIP_STORED), 'only1.txt': ('one', zipfile.ZIP_STORED)})
pack('patch.pak', {'data.txt': ('patched', zipfile.ZIP_DEFLATED), 'only2.txt': ('two', zipfile.ZIP_STORED)})
pack('maps.pak', {'m.txt': ('map', zipfile.ZIP_STORED)})
