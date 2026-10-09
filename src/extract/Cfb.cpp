#include "Cfb.h"

#include <algorithm>
#include <cstring>

namespace ws::extract {

namespace {

constexpr unsigned char kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr std::uint32_t kNoStream = 0xFFFFFFFF;
constexpr std::size_t kMaxEntries = 1u << 20;

std::uint32_t u32(const char* p)
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::uint16_t u16(const char* p)
{
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

char16_t fold(char16_t c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char16_t>(c - 'a' + 'A') : c;
}

bool sameName(std::u16string_view a, std::u16string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char16_t x, char16_t y) {
        return fold(x) == fold(y);
    });
}

} // namespace

bool Cfb::isCfb(std::string_view head)
{
    return head.size() >= 8 && std::memcmp(head.data(), kSignature, 8) == 0;
}

Cfb::Cfb(Source& source)
    : m_source(source)
{
    m_ok = load();
}

bool Cfb::chain(const std::vector<std::uint32_t>& table, std::uint32_t start, std::vector<std::uint32_t>& out) const
{
    out.clear();
    for (std::uint32_t s = start; s != kEndOfChain;) {
        if (s >= table.size() || out.size() >= table.size())
            return false; // out of the table, or a loop
        out.push_back(s);
        s = table[s];
    }
    return true;
}

bool Cfb::readSectors(const std::vector<std::uint32_t>& sectors, std::uint64_t size, std::string& out)
{
    const std::uint64_t sectorSize = std::uint64_t {1} << m_shift;
    size = std::min<std::uint64_t>(size, sectors.size() * sectorSize);
    out.resize(static_cast<std::size_t>(size));
    std::uint64_t done = 0;
    for (std::size_t i = 0; i < sectors.size() && done < size;) {
        // Sectors that follow on from each other are read at once.
        std::size_t run = 1;
        while (i + run < sectors.size() && sectors[i + run] == sectors[i] + run && run < 4096)
            ++run;
        const std::uint64_t bytes = std::min<std::uint64_t>(run * sectorSize, size - done);
        const std::uint64_t offset = (std::uint64_t {sectors[i]} + 1) << m_shift;
        if (!m_source.readExact(offset, out.data() + done, static_cast<std::size_t>(bytes)))
            return false;
        done += bytes;
        i += run;
    }
    return true;
}

bool Cfb::load()
{
    char header[512];
    if (!m_source.readExact(0, header, sizeof header) || !isCfb({header, 8}))
        return false;
    const std::uint16_t major = u16(header + 0x1A);
    m_shift = u16(header + 0x1E);
    m_miniShift = u16(header + 0x20);
    if ((major != 3 && major != 4) || (m_shift != 9 && m_shift != 12) || m_miniShift != 6)
        return false;
    const std::uint32_t fatSectors = u32(header + 0x2C);
    const std::uint32_t firstDirectory = u32(header + 0x30);
    m_miniCutoff = u32(header + 0x38);
    m_firstMiniFat = u32(header + 0x3C);
    std::uint32_t difat = u32(header + 0x44);
    const std::uint32_t difatSectors = u32(header + 0x48);

    const std::uint64_t sectorSize = std::uint64_t {1} << m_shift;
    const std::uint64_t fileSectors = m_source.size() / sectorSize + 1;
    if (fatSectors > fileSectors)
        return false;

    // Which sectors hold the FAT: 109 in the header, the rest in a chain of DIFAT sectors.
    std::vector<std::uint32_t> fatAt;
    for (std::uint32_t i = 0; i < 109 && fatAt.size() < fatSectors; ++i)
        fatAt.push_back(u32(header + 0x4C + 4 * i));
    std::string sector(static_cast<std::size_t>(sectorSize), '\0');
    const std::size_t perSector = static_cast<std::size_t>(sectorSize / 4);
    for (std::uint32_t k = 0; k < difatSectors && fatAt.size() < fatSectors; ++k) {
        if (difat >= fileSectors
            || !m_source.readExact((std::uint64_t {difat} + 1) << m_shift, sector.data(), sector.size()))
            return false;
        for (std::size_t i = 0; i + 1 < perSector && fatAt.size() < fatSectors; ++i)
            fatAt.push_back(u32(sector.data() + 4 * i));
        difat = u32(sector.data() + 4 * (perSector - 1));
    }
    m_fat.reserve(fatAt.size() * perSector);
    for (const std::uint32_t s : fatAt) {
        if (s >= fileSectors || !m_source.readExact((std::uint64_t {s} + 1) << m_shift, sector.data(), sector.size()))
            return false;
        for (std::size_t i = 0; i < perSector; ++i)
            m_fat.push_back(u32(sector.data() + 4 * i));
    }
    if (m_fat.size() > fileSectors)
        m_fat.resize(static_cast<std::size_t>(fileSectors)); // no sector lies past the end of the file

    // The directory.
    std::vector<std::uint32_t> sectors;
    if (!chain(m_fat, firstDirectory, sectors))
        return false;
    std::string directory;
    if (!readSectors(sectors, sectors.size() * sectorSize, directory))
        return false;
    const std::size_t count = std::min(directory.size() / 128, kMaxEntries);
    m_entries.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const char* e = directory.data() + i * 128;
        Entry& entry = m_entries[i];
        const std::size_t nameBytes = std::min<std::size_t>(u16(e + 0x40), 64);
        for (std::size_t k = 0; k + 1 < nameBytes; k += 2) {
            const char16_t c = u16(e + k);
            if (c == 0)
                break;
            entry.name.push_back(c);
        }
        entry.type = static_cast<std::uint8_t>(e[0x42]);
        entry.left = u32(e + 0x44);
        entry.right = u32(e + 0x48);
        entry.child = u32(e + 0x4C);
        entry.start = u32(e + 0x74);
        entry.size = major == 3 ? u32(e + 0x78) : (std::uint64_t {u32(e + 0x7C)} << 32 | u32(e + 0x78));
    }
    if (m_entries.empty() || m_entries[0].type != 5)
        return false;

    // The root's children: a red-black tree through left and right, from its child.
    std::vector<bool> seen(count, false);
    std::vector<std::uint32_t> stack {m_entries[0].child};
    while (!stack.empty()) {
        const std::uint32_t i = stack.back();
        stack.pop_back();
        if (i == kNoStream || i >= count || seen[i])
            continue;
        seen[i] = true;
        m_rootChildren.push_back(i);
        stack.push_back(m_entries[i].left);
        stack.push_back(m_entries[i].right);
    }
    return true;
}

const Cfb::Entry* Cfb::findRootChild(std::u16string_view name) const
{
    for (const std::uint32_t i : m_rootChildren) {
        const Entry& e = m_entries[i];
        if (e.type == 2 && sameName(e.name, name))
            return &e;
    }
    return nullptr;
}

bool Cfb::has(std::u16string_view name) const
{
    return findRootChild(name) != nullptr;
}

bool Cfb::read(std::u16string_view name, std::string& out, std::size_t limit)
{
    out.clear();
    if (!m_ok)
        return false;
    const Entry* e = findRootChild(name);
    if (!e)
        return false;
    const std::uint64_t size = std::min<std::uint64_t>(e->size, limit);
    std::vector<std::uint32_t> sectors;
    if (e->size >= m_miniCutoff)
        return chain(m_fat, e->start, sectors) && readSectors(sectors, size, out);

    // A small stream: in the mini stream, by the mini FAT.
    if (!m_miniLoaded) {
        m_miniLoaded = true;
        std::vector<std::uint32_t> at;
        std::string table;
        if (chain(m_fat, m_firstMiniFat, at) && readSectors(at, at.size() << m_shift, table)) {
            m_miniFat.resize(table.size() / 4);
            std::memcpy(m_miniFat.data(), table.data(), m_miniFat.size() * 4);
        }
        const Entry& root = m_entries[0];
        if (!chain(m_fat, root.start, at) || !readSectors(at, root.size, m_mini))
            m_mini.clear();
        m_miniFat.resize(std::min<std::size_t>(m_miniFat.size(), m_mini.size() >> m_miniShift));
    }
    if (!chain(m_miniFat, e->start, sectors))
        return false;
    const std::uint64_t miniSize = std::uint64_t {1} << m_miniShift;
    out.reserve(static_cast<std::size_t>(size));
    for (const std::uint32_t s : sectors) {
        if (out.size() >= size)
            break;
        const std::uint64_t offset = std::uint64_t {s} << m_miniShift;
        if (offset >= m_mini.size())
            return false;
        const std::uint64_t n = std::min({miniSize, size - out.size(), m_mini.size() - offset});
        out.append(m_mini, static_cast<std::size_t>(offset), static_cast<std::size_t>(n));
    }
    return true;
}

} // namespace ws::extract
