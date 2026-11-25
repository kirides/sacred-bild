// Test of gear/dds.cpp outside the game. Makes the DDS files with texconv from one picture (src.png) and compares what the reader decodes:
//   clang++ -std=c++20 -O2 -I<stb include> gear/tests/dds_test.cpp gear/dds.cpp -o dds_test.exe
//   for f in BC1_UNORM BC2_UNORM BC3_UNORM BC4_UNORM BC5_UNORM BC6H_UF16 BC7_UNORM R8G8B8A8_UNORM B5G6R5_UNORM; do
//       texconv -nologo -y -f $f -m 1 -o dir src.png; mv dir/src.DDS dir/$f.dds; done
//   dds_test.exe dir          (dir holds src.png and the .dds files)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "../dds.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

// Mean absolute error over R, G, B (0..255) between the decoded image and the source (rgba).
static double error(const Gear::Dds::Image& img, const uint8_t* src, bool gray, bool rgOnly)
{
    double sum = 0;
    for (size_t i = 0; i < img.argb.size(); ++i)
    {
        const uint32_t p = img.argb[i];
        const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        const int sr = src[i * 4], sg = src[i * 4 + 1], sb = src[i * 4 + 2];
        if (gray)
        {
            sum += std::abs(r - sr);      // BC4 holds the red channel
        }
        else if (rgOnly)
        {
            sum += (std::abs(r - sr) + std::abs(g - sg)) / 2.0;
        }
        else
        {
            sum += (std::abs(r - sr) + std::abs(g - sg) + std::abs(b - sb)) / 3.0;
        }
    }
    return sum / img.argb.size();
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    int w = 0, h = 0, n = 0;
    uint8_t* src = stbi_load((dir + "/src.png").c_str(), &w, &h, &n, 4);
    check(src != nullptr, "src.png");
    if (!src)
    {
        return 1;
    }
    struct Case
    {
        const char* name;
        double maxError;
        bool block;        // stays compressed with allowBlock
        bool gray, rg;
    };
    const Case cases[] = {
        {"BC1_UNORM", 8, true, false, false},   {"BC2_UNORM", 8, true, false, false},  {"BC3_UNORM", 8, true, false, false},
        {"BC4_UNORM", 8, false, true, false},   {"BC5_UNORM", 8, false, false, true},  {"BC6H_UF16", 12, false, false, false},
        {"BC7_UNORM", 4, false, false, false},  {"R8G8B8A8_UNORM", 0.5, false, false, false}, {"B5G6R5_UNORM", 4, false, false, false},
    };
    for (const Case& c : cases)
    {
        const auto bytes = readFile(dir + "/" + c.name + ".dds");
        check(Gear::Dds::isDds(bytes.data(), bytes.size()), std::string(c.name) + " is DDS");
        for (const bool allowBlock : {true, false})
        {
            Gear::Dds::Image img;
            std::string err;
            const bool ok = Gear::Dds::load(bytes.data(), bytes.size(), allowBlock, img, err);
            check(ok, std::string(c.name) + " loads: " + err);
            if (!ok)
            {
                continue;
            }
            check(img.width == w && img.height == h, std::string(c.name) + " size");
            const bool wantBlock = allowBlock && c.block;
            check((img.kind == Gear::Dds::Kind::Block) == wantBlock, std::string(c.name) + " block kind");
            if (wantBlock)
            {
                const size_t blockBytes = c.name[2] == '1' ? 8 : 16;
                check(img.blocks.size() == size_t(w / 4) * (h / 4) * blockBytes, std::string(c.name) + " block bytes");
                continue;
            }
            const double e = error(img, src, c.gray, c.rg);
            std::printf("%-16s %-12s mean error %.2f\n", c.name, img.format.c_str(), e);
            check(e <= c.maxError, std::string(c.name) + " error " + std::to_string(e));
        }
    }
    // garbage and truncation do not crash
    Gear::Dds::Image img;
    std::string err;
    check(!Gear::Dds::load("DDS junk", 8, true, img, err), "short file");
    auto bytes = readFile(dir + "/BC7_UNORM.dds");
    bytes.resize(bytes.size() / 2);
    check(!Gear::Dds::load(bytes.data(), bytes.size(), true, img, err), "truncated BC7");
    stbi_image_free(src);
    std::printf(failures ? "dds_test: %d failures\n" : "dds_test: all passed\n", failures);
    return failures ? 1 : 0;
}
