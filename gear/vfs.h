// Packs and layers: a mod's resources in a few files instead of thousands.
//
// A pack is a ZIP archive (files stored or deflated, no encryption, no ZIP64), by convention `*.pak` or `*.gpak`. Its paths are relative to a mount point
// inside the mod's folder: with `mount = "/"` the file `ui/a.jpg` of the pack is `mods\<mod>\ui\a.jpg`, with `mount = "ui/maps"` it is `mods\<mod>\ui\maps\ui\a.jpg`.
//
// The mod's mod.lua lists its layers in order, `vfs = {{file = 'resources/data01.pak', mount = '/'}, {file = 'resources/patch.pak', mount = '/'}}`; a later
// layer wins over an earlier one, and a file on disk in the mod's folder wins over every layer (so a mod can still be patched by dropping a file in).
// Without `vfs` every `*.gpak` in the mod's folder is mounted at "/" in name order. All layers are merged into one ordered index (virtual path -> the
// pack and the entry that win), built when the mod loads: a lookup is one search, a directory listing is a range of the index. The packs are opened once
// and only their directory is read: a file is a positioned read at its offset in the pack (and an inflate when deflated), with no file open, permission check
// or virus scan per file, and nothing of the pack held in the 32-bit address space.
//
// Documents, style sheets, templates and images (gear/rml.cpp) and `gear.vfs` (Lua) read through this. tests/vfs_test.cpp builds vfs.cpp alone (Windows,
// for ReadFile).
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Gear::Vfs
{
    class Pack
    {
    public:
        struct Entry
        {
            uint32_t headerOffset = 0, compressed = 0, size = 0;
            uint16_t method = 0;
        };

        // The pack file, opened (its directory is read, its data stays on disk); null with `error` if it is not a readable ZIP.
        static std::unique_ptr<Pack> open(const std::filesystem::path& file, std::string& error);
        ~Pack();
        Pack(const Pack&) = delete;
        Pack& operator=(const Pack&) = delete;

        const std::map<std::string, Entry>& entries() const { return m_entries; }     // lower-case paths with forward slashes
        bool read(const Entry& entry, std::string& out) const;
        // The file at `rel` (forward slashes, case-insensitive) into `out`.
        bool read(const std::string& rel, std::string& out) const;
        bool contains(const std::string& rel) const;

    private:
        Pack() = default;
        bool readAt(uint64_t offset, void* buffer, size_t n) const;
        std::map<std::string, Entry> m_entries;
        uint64_t m_size = 0;
        void* m_file = nullptr;
        mutable std::mutex m_readLock;
    };

    struct Layer
    {
        std::filesystem::path file;        // the pack, an absolute path
        std::string mount = "/";           // where its files appear inside the mod's folder
    };

    // The layers of the mod folder `root`, in order (a later one wins). Replaces an earlier setting and drops what was mounted; the packs open on first use.
    // An empty list: no packs at all. Without a call, the folder's `*.gpak` files are the layers.
    void configure(const std::filesystem::path& root, std::vector<Layer> layers);
    // Forget the layers and the mounted packs of `root` (the mod stopped, or was updated), or all of them with an empty path.
    void unmount(const std::filesystem::path& root = {});

    // `file` is a path inside `root`. False if no layer has it. Not for files that are on disk: the caller tries the disk first.
    bool read(const std::filesystem::path& root, const std::filesystem::path& file, std::string& out);
    bool contains(const std::filesystem::path& root, const std::filesystem::path& file);
    // The files below `dir` (a path relative to the mod's folder, "" for all) that the layers provide, as paths relative to the folder, in order.
    std::vector<std::string> list(const std::filesystem::path& root, const std::string& dir);
}
