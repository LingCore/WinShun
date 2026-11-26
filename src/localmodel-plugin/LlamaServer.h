// One llama-server.exe (llama.cpp, in the plugin's llama\ folder) with one
// model loaded, on a port of its own on 127.0.0.1: started when a request
// needs it, stopped when the model is changed or has not been used for a
// while. Its output goes to server.log in the plugin's data folder; what it
// says about the device it runs on, and why it could not load a model, is
// read from there.
#pragma once

#include <windows.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lm {

struct Launch {
    std::filesystem::path program; // llama-server.exe
    std::filesystem::path model;
    std::string alias; // the model's name in the API (/v1/models)
    bool gpu = true; // offload to a GPU when one is there (Vulkan); false: the CPU alone
    int context = 8192; // tokens, shared by the slots
    int port = 0;
    // Each turn gets this (the slot cache keeps the system prompt warm).
    int parallel = 2;
    // An embedding model: /v1/embeddings and nothing else. Each slot takes a
    // whole text at once (context / parallel tokens at most).
    bool embedding = false;

    bool operator==(const Launch&) const = default;
};

// The program's arguments (testable): the model, 127.0.0.1 and the port,
// no web downloads, a fixed context, the device.
std::vector<std::wstring> arguments(const Launch& launch);
std::wstring commandLine(const Launch& launch); // quoted as CreateProcess wants it

// What the log says (the lines are llama.cpp's own).
struct LogFacts {
    std::string device; // "NVIDIA GeForce RTX 4060 Laptop GPU": the GPU it offloaded to; empty: CPU
    int offloaded = -1; // layers on the GPU, -1 unknown
    int layers = -1;
};
void readLogLine(const std::string& line, LogFacts& facts);
// Why it stopped, said for the user, from its last lines.
std::string failureText(const std::vector<std::string>& lastLines, bool gpu, bool chinese);
// Why it ended by itself after it had loaded (its exit code and last lines):
// memory, else the code.
std::string stoppedText(const std::vector<std::string>& lastLines, unsigned long exitCode, bool gpu, bool chinese);

class LlamaServer {
public:
    LlamaServer() = default;
    LlamaServer(const LlamaServer&) = delete;
    LlamaServer& operator=(const LlamaServer&) = delete;
    ~LlamaServer();

    // Starts it and waits until it answers /health (the model loaded), it
    // ends, `cancel` is set or `timeoutMs` passes. False with `error` (a
    // sentence for the user) when it is not serving.
    bool start(const Launch& launch, const std::filesystem::path& log, const std::atomic_bool& cancel, int timeoutMs,
        std::string& error, bool chinese);
    void stop();
    bool running() const; // the process is alive
    // After it ended by itself: why (stoppedText), and the process let go of.
    std::string reap(bool chinese);
    // After a failed start: it could not have its port (taken meanwhile).
    bool lostPort() const;
    // After a failed start: the GPU was the trouble (its memory, its driver),
    // so the CPU alone may do.
    bool gpuTrouble() const;
    const Launch& launch() const { return m_launch; }
    LogFacts facts() const;
    // The process's private bytes (what the model and its cache take).
    std::uint64_t memory() const;

private:
    void readOutput(HANDLE pipe, std::filesystem::path log);
    std::vector<std::string> lastLines() const;

    Launch m_launch;
    HANDLE m_process = nullptr;
    std::thread m_reader;
    mutable std::mutex m_mutex;
    LogFacts m_facts;
    std::deque<std::string> m_tail; // the last lines it wrote
};

// Whether something answers 200 on GET http://127.0.0.1:<port><path>
// (-1: nothing listening, else the status).
int probe(int port, const char* path, int timeoutMs);
// A port free on 127.0.0.1 now (0 when none could be had).
int freePort();

} // namespace lm
