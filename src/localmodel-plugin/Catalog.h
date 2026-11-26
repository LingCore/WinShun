// What WinShunLocalModel.exe knows about models without running one: the
// models Win顺 recommends (and downloads itself, pinned to one revision on
// ModelScope), and what a .gguf file says about itself (its header and a
// few metadata keys) so that a file the user picks is checked before
// llama-server is asked to load it. Plain C++: tests/tst_localmodel.cpp
// builds it in again.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace lm {

// The port the local models answer on unless the settings name another.
// The translation plugin's "local" service calls the same default:
// http://127.0.0.1:39281/v1. Below the default dynamic ports (49152+) and
// outside the ranges Hyper-V usually reserves.
inline constexpr int kDefaultPort = 39281;

// A model Win顺 recommends: downloaded into the plugin's data folder from
// ModelScope (reachable from China), at a fixed revision, then checked by
// size and SHA-256 before it is used.
struct Recommended {
    std::string_view id; // the `model` setting's value
    std::string_view name; // "Qwen3.5 2B"
    std::string_view file; // the file's name in the models folder
    std::string_view url;
    std::uint64_t size = 0;
    std::string_view sha256; // lower-case hex
    bool translationOnly = false; // translates, and nothing else (no dictionary)
    bool embedding = false; // turns text into vectors (search by meaning): no chat
};

std::span<const Recommended> recommended();
const Recommended* findRecommended(std::string_view id);

// What a model file says about itself.
struct ModelInfo {
    std::string error; // empty when the file reads as a model llama.cpp can load
    std::string architecture; // general.architecture: "qwen35", "hunyuan-dense", …
    std::string name; // general.name, else the file's name
    std::string quantization; // "Q4_K_M", from general.file_type; may be empty
    std::uint64_t contextLength = 0; // <arch>.context_length
    std::uint64_t bytes = 0; // the file's size
    bool embedding = false; // an embedding model (it pools): no chat
};

// Reads the GGUF header and metadata of `file` (any extension: Ollama keeps
// its models as sha256-… blobs). The error, when there is one, is a sentence
// for the user.
ModelInfo readModel(const std::filesystem::path& file, bool chinese);

// "1.2 GB", "856 MB": binary units with the names Windows' Explorer uses.
std::string sizeText(std::uint64_t bytes);

// general.file_type's name ("Q4_K_M"); empty when unknown.
std::string quantizationName(std::uint32_t fileType);

// One line for the settings card: "Qwen3.5-2B · Q4_K_M · 1.2 GB".
std::string describe(const ModelInfo& info);

} // namespace lm
