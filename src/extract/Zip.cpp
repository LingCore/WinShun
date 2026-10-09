#include "Zip.h"

#include "miniz.h"

#include <algorithm>

namespace ws::extract {

namespace {

constexpr std::size_t kMaxEntries = 100000;

std::string folded(std::string_view s)
{
    if (s.starts_with('/'))
        s.remove_prefix(1);
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        else if (c == '\\')
            c = '/';
    }
    return out;
}

} // namespace

struct Zip::Archive {
    mz_zip_archive zip;
    Source* source;
};

Zip::Zip(Source& source, std::uint64_t budget)
    : m_archive(std::make_unique<Archive>())
    , m_budget(budget)
{
    mz_zip_zero_struct(&m_archive->zip);
    m_archive->source = &source;
    m_archive->zip.m_pRead = [](void* opaque, mz_uint64 offset, void* buffer, size_t n) -> size_t {
        return static_cast<Archive*>(opaque)->source->read(offset, buffer, n);
    };
    m_archive->zip.m_pIO_opaque = m_archive.get();
    if (!mz_zip_reader_init(&m_archive->zip, source.size(), 0))
        return;
    m_ok = true;
    const mz_uint n = std::min<mz_uint>(mz_zip_reader_get_num_files(&m_archive->zip), kMaxEntries);
    m_names.reserve(n);
    char name[1024];
    for (mz_uint i = 0; i < n; ++i) {
        const mz_uint len = mz_zip_reader_get_filename(&m_archive->zip, i, name, sizeof name);
        m_names.emplace_back(name, len > 0 ? len - 1 : 0);
        m_byName.emplace(folded(m_names.back()), i);
    }
}

Zip::~Zip()
{
    if (m_ok)
        mz_zip_reader_end(&m_archive->zip);
}

std::optional<std::size_t> Zip::find(std::string_view name) const
{
    const auto it = m_byName.find(folded(name));
    if (it == m_byName.end())
        return std::nullopt;
    return it->second;
}

bool Zip::encrypted(std::size_t i) const
{
    return mz_zip_reader_is_file_encrypted(&m_archive->zip, static_cast<mz_uint>(i));
}

Zip::Entry::~Entry()
{
    mz_zip_reader_extract_iter_free(static_cast<mz_zip_reader_extract_iter_state*>(m_state));
}

std::size_t Zip::Entry::read(char* buffer, std::size_t capacity)
{
    if (m_zip.m_budget == 0)
        return 0;
    capacity = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, m_zip.m_budget));
    const std::size_t got
        = mz_zip_reader_extract_iter_read(static_cast<mz_zip_reader_extract_iter_state*>(m_state), buffer, capacity);
    m_zip.m_budget -= std::min<std::uint64_t>(got, m_zip.m_budget);
    return got;
}

std::unique_ptr<Zip::Entry> Zip::open(std::size_t i)
{
    if (!m_ok || i >= m_names.size() || encrypted(i)
        || !mz_zip_reader_is_file_supported(&m_archive->zip, static_cast<mz_uint>(i)))
        return nullptr;
    mz_zip_reader_extract_iter_state* state
        = mz_zip_reader_extract_iter_new(&m_archive->zip, static_cast<mz_uint>(i), 0);
    if (!state)
        return nullptr;
    return std::unique_ptr<Entry>(new Entry(*this, state));
}

bool Zip::readAll(std::size_t i, std::string& out, std::size_t limit)
{
    out.clear();
    const auto entry = open(i);
    if (!entry)
        return false;
    constexpr std::size_t kChunk = 64 * 1024;
    while (out.size() < limit) {
        const std::size_t old = out.size();
        const std::size_t ask = std::min(kChunk, limit - old);
        out.resize(old + ask);
        const std::size_t got = entry->read(out.data() + old, ask);
        out.resize(old + got);
        if (got == 0)
            break;
    }
    return true;
}

std::string resolvePartPath(std::string_view base, std::string_view target)
{
    std::vector<std::string_view> parts;
    const auto split = [&](std::string_view path) {
        std::size_t start = 0;
        while (start <= path.size()) {
            std::size_t end = path.find_first_of("/\\", start);
            if (end == std::string_view::npos)
                end = path.size();
            const std::string_view part = path.substr(start, end - start);
            if (part == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            start = end + 1;
        }
    };
    if (target.starts_with('/')) {
        split(target);
    } else {
        const std::size_t slash = base.find_last_of('/');
        if (slash != std::string_view::npos)
            split(base.substr(0, slash));
        split(target);
    }
    std::string out;
    for (const std::string_view p : parts) {
        if (!out.empty())
            out.push_back('/');
        out.append(p);
    }
    return out;
}

} // namespace ws::extract
