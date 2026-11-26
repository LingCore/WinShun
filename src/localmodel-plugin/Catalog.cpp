#include "Catalog.h"

#include "winshun.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <system_error>

namespace lm {

namespace {

// Pinned to the revision (the commit on ModelScope) the size and the SHA-256
// were read from: a model re-uploaded under the same name would fail the check
// instead of being taken. www.modelscope.cn answers with a redirect to its CDN,
// which serves ranges (a download goes on where it stopped).
constexpr std::array kRecommended {
    Recommended {
        "qwen3.5-2b",
        "Qwen3.5 2B",
        "Qwen3.5-2B-Q4_K_M.gguf",
        "https://www.modelscope.cn/models/unsloth/Qwen3.5-2B-GGUF/resolve/90057e31161eb95cc0bc1413c4f53b44de9b49c8/Qwen3.5-2B-Q4_K_M.gguf",
        1280835840,
        "aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223",
        false,
    },
    Recommended {
        "hy-mt1.5-1.8b",
        "HY-MT1.5 1.8B",
        "HY-MT1.5-1.8B-Q4_K_M.gguf",
        "https://www.modelscope.cn/models/Tencent-Hunyuan/HY-MT1.5-1.8B-GGUF/resolve/acac2122e32c8d7e6221fb135f918f6e6c87ce49/HY-MT1.5-1.8B-Q4_K_M.gguf",
        1133080512,
        "4383ac0c3c8e476de98ff979c2a3f069f8c4fb385e7860cf2d28da896cc477c7",
        true,
    },
    Recommended {
        "qwen3-embedding-0.6b",
        "Qwen3-Embedding 0.6B",
        "Qwen3-Embedding-0.6B-Q8_0.gguf",
        "https://www.modelscope.cn/models/Qwen/Qwen3-Embedding-0.6B-GGUF/resolve/a6804f0dece24ece939f52ffa3316fd650ca1839/Qwen3-Embedding-0.6B-Q8_0.gguf",
        639150592,
        "06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439",
        false,
        true,
    },
};

// GGUF (docs/gguf.md in ggml): "GGUF", a version, the tensor and metadata
// counts, then key / type / value triples. Only what is shown is kept.
enum ValueType : std::uint32_t {
    U8 = 0, I8 = 1, U16 = 2, I16 = 3, U32 = 4, I32 = 5, F32 = 6, Bool = 7, String = 8, Array = 9, U64 = 10, I64 = 11, F64 = 12,
};

std::optional<std::uint64_t> scalarSize(std::uint32_t type)
{
    switch (type) {
    case U8: case I8: case Bool: return 1;
    case U16: case I16: return 2;
    case U32: case I32: case F32: return 4;
    case U64: case I64: case F64: return 8;
    default: return std::nullopt;
    }
}

class GgufReader {
public:
    GgufReader(const std::filesystem::path& file, std::uint64_t size) : m_in(file, std::ios::binary), m_size(size) {}

    bool open() const { return m_in.is_open(); }
    bool good() const { return static_cast<bool>(m_in); }

    template <typename T>
    bool get(T& value)
    {
        m_in.read(reinterpret_cast<char*>(&value), sizeof value);
        return good();
    }

    bool skip(std::uint64_t bytes)
    {
        const auto at = static_cast<std::uint64_t>(m_in.tellg());
        if (!good() || bytes > m_size || at > m_size - bytes)
            return false;
        m_in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
        return good();
    }

    // A string up to `keep` bytes is read; a longer one is skipped (empty).
    bool string(std::string& out, std::uint64_t keep)
    {
        std::uint64_t length = 0;
        if (!get(length))
            return false;
        out.clear();
        if (length > keep)
            return skip(length);
        if (length > m_size)
            return false;
        out.resize(static_cast<std::size_t>(length));
        if (length)
            m_in.read(out.data(), static_cast<std::streamsize>(length));
        return good();
    }

    // A value not wanted: its bytes are passed over (an array of strings
    // one by one: the tokenizer's 150 000 words are read past, not kept).
    bool skipValue(std::uint32_t type, int depth = 0)
    {
        if (const auto size = scalarSize(type))
            return skip(*size);
        if (type == String) {
            std::string ignored;
            return string(ignored, 0);
        }
        if (type != Array || depth > 2)
            return false;
        std::uint32_t element = 0;
        std::uint64_t count = 0;
        if (!get(element) || !get(count))
            return false;
        if (const auto size = scalarSize(element)) {
            if (count > m_size / *size)
                return false;
            return skip(count * *size);
        }
        if (count > m_size)
            return false;
        for (std::uint64_t i = 0; i < count; ++i) {
            if (!skipValue(element, depth + 1))
                return false;
        }
        return true;
    }

    // A whole number of any width (a count or an enum), when `type` is one.
    std::optional<std::uint64_t> number(std::uint32_t type)
    {
        switch (type) {
        case U8: case I8: case Bool: { std::uint8_t v = 0; return get(v) ? std::optional<std::uint64_t>(v) : std::nullopt; }
        case U16: case I16: { std::uint16_t v = 0; return get(v) ? std::optional<std::uint64_t>(v) : std::nullopt; }
        case U32: case I32: { std::uint32_t v = 0; return get(v) ? std::optional<std::uint64_t>(v) : std::nullopt; }
        case U64: case I64: { std::uint64_t v = 0; return get(v) ? std::optional<std::uint64_t>(v) : std::nullopt; }
        default: return skipValue(type) ? std::optional<std::uint64_t>(0) : std::nullopt;
        }
    }

private:
    std::ifstream m_in;
    std::uint64_t m_size = 0;
};

bool endsWith(std::string_view text, std::string_view tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

std::span<const Recommended> recommended() { return kRecommended; }

const Recommended* findRecommended(std::string_view id)
{
    const auto it = std::ranges::find(kRecommended, id, &Recommended::id);
    return it == kRecommended.end() ? nullptr : &*it;
}

ModelInfo readModel(const std::filesystem::path& file, bool chinese)
{
    ModelInfo info;
    const auto fail = [&](const char* zh, const char* en) {
        info.error = chinese ? zh : en;
        return info;
    };
    std::error_code ec;
    if (file.empty())
        return fail("还没选模型文件", "No model file chosen");
    if (!std::filesystem::is_regular_file(file, ec))
        return fail("找不到模型文件", "The model file is not there");
    info.bytes = std::filesystem::file_size(file, ec);
    if (ec)
        return fail("模型文件读不了", "The model file cannot be read");
    GgufReader in(file, info.bytes);
    if (!in.open())
        return fail("模型文件打不开（可能正被别的程序占用）", "The model file cannot be opened (another program may hold it)");
    std::array<char, 4> magic {};
    std::uint32_t version = 0;
    std::uint64_t tensors = 0;
    std::uint64_t keys = 0;
    if (!in.get(magic) || std::string_view(magic.data(), magic.size()) != "GGUF")
        return fail("这不是 GGUF 模型文件（llama.cpp 用的格式，扩展名一般是 .gguf）",
            "Not a GGUF model file (the format llama.cpp loads, usually .gguf)");
    if (!in.get(version) || version < 2 || version > 3)
        return fail("这个 GGUF 文件的版本太旧或太新，读不了", "This GGUF file's version is not supported");
    if (!in.get(tensors) || !in.get(keys) || keys > 1'000'000)
        return fail("模型文件不完整", "The model file is incomplete");

    std::map<std::string, std::string> strings; // the general.* strings
    std::map<std::string, std::uint64_t> numbers; // the numbers kept
    for (std::uint64_t i = 0; i < keys; ++i) {
        std::string key;
        std::uint32_t type = 0;
        if (!in.string(key, 4096) || key.empty() || !in.get(type))
            return fail("模型文件不完整", "The model file is incomplete");
        const bool general = key.starts_with("general.");
        if (type == String && general) {
            std::string value;
            if (!in.string(value, 4096))
                return fail("模型文件不完整", "The model file is incomplete");
            strings[key] = std::move(value);
        } else if ((key == "general.file_type" || endsWith(key, ".context_length") || endsWith(key, ".pooling_type"))
            && scalarSize(type) && type != F32 && type != F64) {
            const auto value = in.number(type);
            if (!value)
                return fail("模型文件不完整", "The model file is incomplete");
            numbers[key] = *value;
        } else if (!in.skipValue(type)) {
            return fail("模型文件不完整", "The model file is incomplete");
        }
    }

    info.architecture = strings["general.architecture"];
    if (info.architecture.empty())
        return fail("模型文件里没写是哪种模型（general.architecture）", "The model file does not say what model it is (general.architecture)");
    const std::string kind = strings["general.type"];
    if (info.architecture == "clip" || kind == "mmproj")
        return fail("这是多模态模型的视觉部分（mmproj），不是模型本身：选同一文件夹里另一个 .gguf",
            "This is a multimodal projector (mmproj), not the model: pick the other .gguf beside it");
    if (kind == "adapter")
        return fail("这是 LoRA 适配器，不是完整的模型", "This is a LoRA adapter, not a whole model");
    info.name = strings["general.name"];
    if (info.name.empty())
        info.name = strings["general.basename"];
    if (info.name.empty())
        info.name = winshun::utf8(file.stem().wstring()); // not string(): that is the ANSI code page
    if (const auto it = numbers.find("general.file_type"); it != numbers.end())
        info.quantization = quantizationName(static_cast<std::uint32_t>(it->second));
    if (const auto it = numbers.find(info.architecture + ".context_length"); it != numbers.end())
        info.contextLength = it->second;
    // Embedding models pool their tokens into one vector (bert, nomic-bert,
    // Qwen3-Embedding, …): they cannot chat.
    info.embedding = numbers.contains(info.architecture + ".pooling_type")
        || info.architecture == "bert" || info.architecture == "nomic-bert" || info.architecture.starts_with("jina-bert");
    return info;
}

std::string sizeText(std::uint64_t bytes)
{
    char text[32];
    constexpr double kGiB = 1024.0 * 1024 * 1024;
    constexpr double kMiB = 1024.0 * 1024;
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / kGiB);
    else
        std::snprintf(text, sizeof text, "%.0f MB", std::max(1.0, static_cast<double>(bytes) / kMiB));
    return text;
}

std::string quantizationName(std::uint32_t fileType)
{
    // llama_ftype (llama.h): the most common ones.
    switch (fileType) {
    case 0: return "F32";
    case 1: return "F16";
    case 2: return "Q4_0";
    case 3: return "Q4_1";
    case 7: return "Q8_0";
    case 8: return "Q5_0";
    case 9: return "Q5_1";
    case 10: return "Q2_K";
    case 11: return "Q3_K_S";
    case 12: return "Q3_K_M";
    case 13: return "Q3_K_L";
    case 14: return "Q4_K_S";
    case 15: return "Q4_K_M";
    case 16: return "Q5_K_S";
    case 17: return "Q5_K_M";
    case 18: return "Q6_K";
    case 19: return "IQ2_XXS";
    case 20: return "IQ2_XS";
    case 21: return "Q2_K_S";
    case 22: return "IQ3_XS";
    case 23: return "IQ3_XXS";
    case 24: return "IQ1_S";
    case 25: return "IQ4_NL";
    case 26: return "IQ3_S";
    case 27: return "IQ3_M";
    case 28: return "IQ2_S";
    case 29: return "IQ2_M";
    case 30: return "IQ4_XS";
    case 31: return "IQ1_M";
    case 32: return "BF16";
    default: return {};
    }
}

std::string describe(const ModelInfo& info)
{
    std::string text = info.name;
    if (!info.quantization.empty())
        text += " · " + info.quantization;
    if (info.bytes)
        text += " · " + sizeText(info.bytes);
    return text;
}

} // namespace lm
