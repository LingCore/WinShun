#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <windows.h>

namespace ws::extract {

// A file read at any offset: documents are zip archives, compound files and
// PDFs, read from wherever their directories point.
class Source {
public:
    virtual ~Source() = default;
    virtual std::uint64_t size() const = 0;
    // Up to n bytes at `offset`: fewer at the end of the file, 0 past it or on failure.
    virtual std::size_t read(std::uint64_t offset, void* buffer, std::size_t n) = 0;

    bool readExact(std::uint64_t offset, void* buffer, std::size_t n) { return n == 0 || read(offset, buffer, n) == n; }
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::string_view data)
        : m_data(data)
    {
    }
    std::uint64_t size() const override { return m_data.size(); }
    std::size_t read(std::uint64_t offset, void* buffer, std::size_t n) override
    {
        if (offset >= m_data.size())
            return 0;
        n = std::min<std::size_t>(n, m_data.size() - static_cast<std::size_t>(offset));
        std::memcpy(buffer, m_data.data() + offset, n);
        return n;
    }

private:
    std::string_view m_data;
};

// A file handle open for reading (synchronous I/O). Not owned.
class HandleSource final : public Source {
public:
    HandleSource(HANDLE file, std::uint64_t size)
        : m_file(file)
        , m_size(size)
    {
    }
    std::uint64_t size() const override { return m_size; }
    std::size_t read(std::uint64_t offset, void* buffer, std::size_t n) override
    {
        if (offset >= m_size)
            return 0;
        n = static_cast<std::size_t>(std::min<std::uint64_t>(n, m_size - offset));
        std::size_t total = 0;
        while (total < n) {
            OVERLAPPED at {};
            const std::uint64_t pos = offset + total;
            at.Offset = static_cast<DWORD>(pos);
            at.OffsetHigh = static_cast<DWORD>(pos >> 32);
            DWORD got = 0;
            const auto ask = static_cast<DWORD>(std::min<std::size_t>(n - total, 1u << 30));
            if (!::ReadFile(m_file, static_cast<char*>(buffer) + total, ask, &got, &at) || got == 0)
                break;
            total += got;
        }
        return total;
    }

private:
    HANDLE m_file;
    std::uint64_t m_size;
};

} // namespace ws::extract
