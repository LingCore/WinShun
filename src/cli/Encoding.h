#pragma once

// Text in and out of UTF-16 (docs/design-cli.md §4, "编码"). Inside ws every
// string is UTF-16; Win顺's answers are UTF-8 JSON; what ws writes where it
// is not a console is bytes in a code page:
// - UTF-8 without a BOM, as Git Bash, Claude Code, Codex and Python want;
// - but when the parent is Windows PowerShell or PowerShell 7, the console's
//   output code page, which PowerShell decodes with (65001 is UTF-8 again);
// - --encoding / WS_ENCODING force one.

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace ws::cli {

std::string utf8(std::wstring_view text);
std::wstring wide(std::string_view utf8);

inline constexpr UINT kUtf16CodePage = 1200; // UTF-16LE, as Windows numbers it
inline constexpr UINT kGb18030CodePage = 54936;

enum class EncodingChoice { Utf8, Utf16, Ansi, Oem, Console };

// utf8 (utf-8), utf16 (utf-16, utf16le), ansi, oem, console: what
// --encoding and WS_ENCODING take. Any case.
std::optional<EncodingChoice> encodingNamed(std::wstring_view name);

// The code page a choice stands for, on this computer. `consoleCp`:
// GetConsoleOutputCP(), 0 without a console (then UTF-8).
UINT codePageOf(EncodingChoice choice, UINT consoleCp);

// The code page for output that does not go to a console.
UINT redirectedCodePage(std::optional<EncodingChoice> chosen, bool parentIsPowerShell, UINT consoleCp);

// Whether the process that started ws is Windows PowerShell or PowerShell 7
// (powershell.exe, pwsh.exe), still running: what ws's output is
// decoded with depends on it.
bool parentIsPowerShell();
// The parent's file name ("pwsh.exe"); empty when it is gone or unknown.
std::wstring parentImageName();

// UTF-16 to a code page. What it cannot hold becomes one '?' per character,
// or with jsonEscapes \uXXXX (for JSON text, whose characters beyond ASCII
// are all inside strings: nothing is lost).
class Encoder {
public:
    explicit Encoder(UINT codePage = CP_UTF8, bool jsonEscapes = false);

    UINT codePage() const noexcept { return m_codePage; }
    void setJsonEscapes(bool on) noexcept { m_jsonEscapes = on; }

    // Appends `text` to `out`; returns how many characters could not be
    // written as they are ('?' put instead; with JSON escapes: 0).
    std::size_t append(std::string& out, std::wstring_view text) const;

    // Whether every character can be written (UTF-8, UTF-16, GB18030).
    bool lossless() const noexcept { return m_lossless; }

private:
    std::size_t appendSlowly(std::string& out, std::wstring_view text) const;
    bool fits(std::wstring_view character, std::string& bytes) const;

    UINT m_codePage;
    bool m_jsonEscapes;
    bool m_lossless;
    bool m_flagsAllowed; // WC_NO_BEST_FIT_CHARS and the default-character check
};

} // namespace ws::cli
