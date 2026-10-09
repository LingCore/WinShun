#pragma once

#include "Source.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ws::extract {

// A compound file ([MS-CFB], "OLE2"): the container of Word, Excel and
// PowerPoint 97–2003 files (and of WPS's own, and of password-protected
// Office Open XML). Only the streams directly in the root storage are
// looked at: those inside sub-storages belong to embedded objects.
class Cfb {
public:
    explicit Cfb(Source& source);

    bool ok() const noexcept { return m_ok; }
    bool has(std::u16string_view name) const; // ASCII case-insensitive
    // The stream, up to `limit` bytes. False if there is none or it cannot be read.
    bool read(std::u16string_view name, std::string& out, std::size_t limit);

    static bool isCfb(std::string_view head);

private:
    struct Entry {
        std::u16string name;
        std::uint8_t type = 0; // 1 storage, 2 stream, 5 root
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        std::uint32_t child = 0;
        std::uint32_t start = 0;
        std::uint64_t size = 0;
    };

    bool load();
    bool chain(const std::vector<std::uint32_t>& table, std::uint32_t start, std::vector<std::uint32_t>& out) const;
    bool readSectors(const std::vector<std::uint32_t>& sectors, std::uint64_t size, std::string& out);
    const Entry* findRootChild(std::u16string_view name) const;

    Source& m_source;
    bool m_ok = false;
    unsigned m_shift = 9;
    unsigned m_miniShift = 6;
    std::uint32_t m_miniCutoff = 4096;
    std::uint32_t m_firstMiniFat = 0;
    std::vector<std::uint32_t> m_fat;
    std::vector<std::uint32_t> m_miniFat;
    std::vector<Entry> m_entries;
    std::vector<std::uint32_t> m_rootChildren; // entry numbers
    std::string m_mini; // the mini stream, read when first needed
    bool m_miniLoaded = false;
};

} // namespace ws::extract
