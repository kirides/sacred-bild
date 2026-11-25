// Test of gear/vfs.cpp outside the game. From the repository root (a ZIP with a stored and a deflated file in a temp folder):
//   python -c "import zipfile,os;d=os.environ['TEMP']+'/vfstest/mod';os.makedirs(d,exist_ok=True);z=zipfile.ZipFile(d+'/a.gpak','w');z.writestr('ui/a.txt','stored',zipfile.ZIP_STORED);z.writestr('ui/B.txt','deflated '*500,zipfile.ZIP_DEFLATED);z.close()"
//   clang++ -std=c++20 gear/tests/vfs_test.cpp gear/vfs.cpp -I<zlib include> -L<zlib lib> -llibzs -o vfs_test.exe   (or any build with zlib)
//   vfs_test.exe %TEMP%/vfstest/mod
#include "../vfs.h"

#include <cstdio>
#include <string>

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
    const std::filesystem::path root = argc > 1 ? argv[1] : ".";
    std::string out;
    check(Gear::Vfs::read(root, root / "ui" / "a.txt", out) && out == "stored", "stored file");
    check(Gear::Vfs::read(root, root / "ui" / "b.txt", out) && out.size() == 4500 && out.compare(0, 9, "deflated ") == 0, "deflated file, any case");
    check(Gear::Vfs::contains(root, root / "ui" / "a.txt"), "contains");
    check(!Gear::Vfs::read(root, root / "ui" / "missing.txt", out), "missing file");
    check(!Gear::Vfs::read(root, root / ".." / "x.txt", out), "outside the mod folder");
    check(!Gear::Vfs::read(root / "nopack", root / "nopack" / "ui" / "a.txt", out), "a folder without packs");
    std::printf(failures ? "vfs_test: %d failures\n" : "vfs_test: all passed\n", failures);
    return failures ? 1 : 0;
}
