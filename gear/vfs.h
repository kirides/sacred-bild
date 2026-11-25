// Packs: a mod's resources in one file instead of thousands.
//
// Every `*.gpak` in a mod's folder is a ZIP archive (files stored or deflated, no encryption, no ZIP64) whose paths are relative to the mod's folder:
// `ui/megamap/s2/005_003.jpg` inside the pack is the file `mods\<mod>\ui\megamap\s2\005_003.jpg`. A file that is not on disk is looked up in the
// mod's packs (a file on disk wins, so a mod can still be patched by dropping a file in). The pack is opened once and mapped into memory: reading a
// file is a copy (stored) or an inflate, with no file open, no permission check and no virus scan per file; copying or downloading the mod is one file.
//
// Documents, their style sheets, templates and images read through this (gear/rml.cpp). tests/vfs_test.cpp builds vfs.cpp alone (Windows, for the file mapping).
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gear::Vfs
{
    class Pack
    {
    public:
        // The pack file, mapped; null with `error` if it is not a readable ZIP.
        static std::unique_ptr<Pack> open(const std::filesystem::path& file, std::string& error);
        ~Pack();
        Pack(const Pack&) = delete;
        Pack& operator=(const Pack&) = delete;

        // The file at `rel` (forward slashes, case-insensitive) into `out`.
        bool read(const std::string& rel, std::string& out) const;
        bool contains(const std::string& rel) const;
        size_t count() const { return m_entries.size(); }

    private:
        Pack() = default;
        struct Entry
        {
            uint32_t headerOffset = 0, compressed = 0, size = 0;
            uint16_t method = 0;
        };
        std::unordered_map<std::string, Entry> m_entries;
        const uint8_t* m_data = nullptr;
        size_t m_size = 0;
        void* m_file = nullptr;
        void* m_mapping = nullptr;
    };

    // The packs of the mod folder `root`, mounted on first use (the folder's `*.gpak`, in name order; the first pack that has a file wins).
    // `file` is a path inside `root`; false if no pack has it. Not for files that are on disk: the caller tries the disk first.
    bool read(const std::filesystem::path& root, const std::filesystem::path& file, std::string& out);
    bool contains(const std::filesystem::path& root, const std::filesystem::path& file);

    // Forget the mounted packs of `root` (a mod was updated), or all of them with an empty path.
    void unmount(const std::filesystem::path& root = {});
}
