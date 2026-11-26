#pragma once

// Turns text into vectors through an OpenAI-compatible /embeddings endpoint:
// a cloud service (OpenAI, 阿里云百炼, 硅基流动…) or one on this computer
// (Win顺's own 本地模型 plugin, Ollama, LM Studio). The user picks the service and the model in the
// plugin's settings; nothing is sent until they did.

#include "winshun.h"

#include <string>
#include <vector>

namespace semantic {

struct Provider {
    std::string id; // none | local | ollama | lmstudio | siliconflow | bailian | openai | custom
    std::string name; // for the user: "Ollama（本机）"
    std::string endpoint; // ".../v1", without "/embeddings"
    std::string model;
    std::string key;
    std::string queryPrefix; // what the model wants before a query…
    std::string docPrefix; // … and before a passage
    bool local = false; // runs on this computer: nothing leaves it, but it takes the CPU
    int batch = 10; // texts per request
    // "local" with no model named in the settings: the model is the one the
    // 本地模型 plugin runs, asked by localModel() (empty until then).
    bool modelFromService = false;

    bool configured() const { return id != "none" && !endpoint.empty() && !model.empty() && (local || !key.empty()); }
    // What the stored vectors depend on: another one means indexing again.
    std::string signature() const;
};

Provider providerFrom(const winshun::Json& settings, bool chinese);

// The 本地模型 plugin's card in Win顺's settings (an item's open.url).
inline constexpr const char* kLocalModelCard = "winshun:settings/plugin/winshun.localmodel";

// The embedding model the 本地模型 plugin runs ("qwen3-embedding-0.6b"), into
// model; else why there is none, for the user (not installed, not
// downloaded yet…).
std::string localModel(const Provider& provider, bool chinese, std::string& model);
// model as the provider's, with what its family wants before a text.
void setModel(Provider& provider, const std::string& model);

struct Embedded {
    std::vector<std::vector<float>> vectors; // normalized; one per text, in order
    std::string error; // for the user; empty when it worked
    bool retryLater = false; // the service is away or busy: the same texts may work later
    bool badInput = false; // the service refused these texts (too long…): skip them
};

// `query`: the texts are what the user typed (queryPrefix), else passages.
Embedded embed(const Provider& provider, const std::vector<std::string>& texts, bool query, bool chinese);

} // namespace semantic
