#pragma once

#include "Source.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ws::extract {

// A zip archive (Office Open XML, OpenDocument, EPUB), read through miniz.
// Everything taken out of it counts against one budget: an archive that
// unpacks to more (a zip bomb) yields nothing past it.
class Zip {
public:
    Zip(Source& source, std::uint64_t budget);
    ~Zip();
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    bool ok() const noexcept { return m_ok; }
    std::size_t count() const noexcept { return m_names.size(); }
    const std::string& name(std::size_t i) const { return m_names[i]; }
    std::optional<std::size_t> find(std::string_view name) const; // ASCII case-insensitive; a leading '/' is ignored
    bool encrypted(std::size_t i) const;

    // An entry, read in chunks.
    class Entry {
    public:
        ~Entry();
        std::size_t read(char* buffer, std::size_t capacity); // 0: the end (or the budget is spent)

    private:
        friend class Zip;
        Entry(Zip& zip, void* state)
            : m_zip(zip)
            , m_state(state)
        {
        }
        Zip& m_zip;
        void* m_state;
    };
    std::unique_ptr<Entry> open(std::size_t i);
    // The whole entry, up to `limit` bytes. False if it could not be read.
    bool readAll(std::size_t i, std::string& out, std::size_t limit);

private:
    struct Archive;
    std::unique_ptr<Archive> m_archive;
    std::vector<std::string> m_names;
    std::unordered_map<std::string, std::size_t> m_byName; // folded
    std::uint64_t m_budget;
    bool m_ok = false;
};

// "word/document.xml" + "../media/a.png" → "media/a.png": a relationship's
// target, relative to the folder of the part it belongs to.
std::string resolvePartPath(std::string_view base, std::string_view target);

} // namespace ws::extract
