// Test of src/gear/vfs.cpp outside the game. From the repository root:
//   python src/gear/tests/make_vfs_test_packs.py %TEMP%/vfstest/mod       (writes a.gpak, data01.pak, patch.pak, maps.pak)
//   clang++ -m32 -std=c++20 src/gear/tests/vfs_test.cpp src/gear/vfs.cpp -I<zlib include> -L<zlib lib> -llibzs -o vfs_test.exe
//   vfs_test.exe %TEMP%/vfstest/mod
#include "../vfs.h"

#include <cstdio>
#include <string>

namespace fs = std::filesystem;
static int failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

int main(int argc, char** argv)
{
    const fs::path root = argc > 1 ? argv[1] : ".";
    std::string out;

    // no configuration: every *.gpak of the folder at the root
    check(Gear::Vfs::read(root, root / "ui" / "a.txt", out) && out == "stored", "auto: stored file");
    check(Gear::Vfs::read(root, root / "ui" / "b.txt", out) && out.size() == 4500 && out.compare(0, 9, "deflated ") == 0, "auto: deflated file, any case");
    check(Gear::Vfs::contains(root, root / "ui" / "a.txt"), "auto: contains");
    check(!Gear::Vfs::read(root, root / "ui" / "missing.txt", out), "missing file");
    check(!Gear::Vfs::read(root, root / ".." / "x.txt", out), "outside the mod folder");
    check(!Gear::Vfs::read(root / "nopack", root / "nopack" / "ui" / "a.txt", out), "a folder without packs");

    // layers: a later one wins, a mount moves the files, an empty list mounts nothing
    Gear::Vfs::configure(root, {{root / "data01.pak", "/"}, {root / "patch.pak", "/"}, {root / "maps.pak", "ui/maps"}});
    check(Gear::Vfs::read(root, root / "data.txt", out) && out == "patched", "later layer wins");
    check(Gear::Vfs::read(root, root / "only1.txt", out) && out == "one", "file only in the first layer");
    check(Gear::Vfs::read(root, root / "only2.txt", out) && out == "two", "file only in the second layer");
    check(Gear::Vfs::read(root, root / "ui" / "maps" / "m.txt", out) && out == "map", "mounted below a folder");
    check(!Gear::Vfs::read(root, root / "m.txt", out), "mounted file is not at the root");
    check(!Gear::Vfs::read(root, root / "ui" / "a.txt", out), "configured layers replace the automatic ones");
    const auto files = Gear::Vfs::list(root, "ui/maps");
    check(files.size() == 1 && files[0] == "ui/maps/m.txt", "list below a folder");
    const auto all = Gear::Vfs::list(root, "");
    check(all.size() == 4 && all[0] == "data.txt", "list everything, in order");
    Gear::Vfs::configure(root, {});
    check(!Gear::Vfs::read(root, root / "data.txt", out), "empty layer list");
    Gear::Vfs::unmount(root);
    check(Gear::Vfs::read(root, root / "ui" / "a.txt", out), "after unmount the automatic layers are back");

    std::printf(failures ? "vfs_test: %d failures\n" : "vfs_test: all passed\n", failures);
    return failures ? 1 : 0;
}
