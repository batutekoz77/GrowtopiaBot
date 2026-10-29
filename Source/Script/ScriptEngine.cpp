#include "ScriptRuntime.hpp"
#include "../Main/Config.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
    constexpr size_t OUTPUT_LIMIT = 3000;

    bool CleanName(const std::string& name, std::string& out, std::string& error) {
        out = name;
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(out.begin());
        if (out.empty()) {
            error = "empty script name";
            return false;
        }
        if (out.find_first_of("/\\:*?\"<>|") != std::string::npos || out.find("..") != std::string::npos) {
            error = "script names can't contain / \\ : * ? \" < > | or ..";
            return false;
        }
        if (out.size() < 4 || out.substr(out.size() - 4) != ".lua") out += ".lua";
        return true;
    }
}

ScriptEngine::ScriptEngine(BotManager& manager) : manager_(manager) {
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
}

ScriptEngine::~ScriptEngine() {
    StopAll();
    std::lock_guard lock(mutex_);
    scripts_.clear();
}

int ScriptEngine::Run(const std::string& name, const std::string& code, int botId, bool echo, bool trace) {
    std::shared_ptr<Script> script;
    {
        std::lock_guard lock(mutex_);
        if (trace) for (const auto& s : scripts_) s->trace = false;
        script = std::make_shared<Script>(*this, nextId_++, name.empty() ? "script" : name, code, botId, echo, trace && !echo);
        scripts_.push_back(script);
    }
    if (!echo) Print(script->id, Log::Level::Info, "started " + script->name);
    script->Start();
    return script->id;
}

void ScriptEngine::Stop(int id) {
    std::lock_guard lock(mutex_);
    for (const auto& s : scripts_) {
        if (s->id == id) s->RequestStop();
    }
}

void ScriptEngine::SetTrace(int id) {
    std::lock_guard lock(mutex_);
    for (const auto& s : scripts_) s->trace = s->id == id;
}

bool ScriptEngine::Lines(int id, std::vector<uint8_t>& out) const {
    std::lock_guard lock(mutex_);
    for (const auto& s : scripts_) {
        if (s->id != id) continue;
        out.resize(static_cast<size_t>(s->lineCount));
        for (int i = 0; i < s->lineCount; ++i) out[static_cast<size_t>(i)] = s->lines[i].load(std::memory_order_relaxed);
        return true;
    }
    return false;
}

void ScriptEngine::StopAll() {
    std::vector<std::shared_ptr<Script>> scripts;
    {
        std::lock_guard lock(mutex_);
        scripts = scripts_;
    }
    for (const auto& s : scripts) s->RequestStop();
    for (const auto& s : scripts) {
        if (s->thread.joinable() && s->thread.get_id() != std::this_thread::get_id()) s->thread.join();
    }
}

void ScriptEngine::ClearFinished() {
    std::vector<std::shared_ptr<Script>> finished;
    {
        std::lock_guard lock(mutex_);
        for (auto it = scripts_.begin(); it != scripts_.end();) {
            if (!(*it)->running) {
                finished.push_back(*it);
                it = scripts_.erase(it);
            }
            else ++it;
        }
    }
    finished.clear();
}

std::vector<ScriptEngine::Info> ScriptEngine::List() const {
    std::lock_guard lock(mutex_);
    std::vector<Info> list;
    list.reserve(scripts_.size());
    const auto now = Script::Clock::now();
    for (const auto& s : scripts_) {
        Info info;
        info.id = s->id;
        info.name = s->name;
        info.bot = s->defaultBot;
        info.running = s->running;
        info.failed = s->failed;
        info.events = s->handled;
        info.dropped = s->dropped;
        std::lock_guard infoLock(s->infoMutex);
        info.error = s->error;
        const auto end = info.running ? now : s->finished;
        info.seconds = std::chrono::duration<double>(end - s->started).count();
        list.push_back(std::move(info));
    }
    return list;
}

size_t ScriptEngine::RunningCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<size_t>(std::ranges::count_if(scripts_, [](const auto& s) { return s->running.load(); }));
}

std::vector<ScriptEngine::Line> ScriptEngine::OutputSince(uint64_t after) const {
    std::lock_guard lock(outputMutex_);
    std::vector<Line> out;
    if (output_.empty() || output_.back().seq <= after) return out;
    const uint64_t first = output_.front().seq;
    const size_t start = after < first ? 0 : static_cast<size_t>(after - first + 1);
    out.assign(output_.begin() + static_cast<std::ptrdiff_t>(start), output_.end());
    return out;
}

void ScriptEngine::ClearOutput() {
    std::lock_guard lock(outputMutex_);
    output_.clear();
}

void ScriptEngine::Print(int script, Log::Level level, std::string text) {
    std::lock_guard lock(outputMutex_);
    if (output_.size() >= OUTPUT_LIMIT) output_.pop_front();
    output_.push_back({ ++outputSeq_, script, level, std::move(text) });
}

std::filesystem::path ScriptEngine::Folder() { return Config::ExeDirectory() / "scripts"; }

std::vector<std::string> ScriptEngine::ListFiles() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(Folder(), ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lua") names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

bool ScriptEngine::LoadFile(const std::string& name, std::string& code, std::string& error) {
    std::string clean;
    if (!CleanName(name, clean, error)) return false;
    std::ifstream file(Folder() / clean, std::ios::binary);
    if (!file) {
        error = "no script " + clean;
        return false;
    }
    std::stringstream content;
    content << file.rdbuf();
    code = content.str();
    return true;
}

bool ScriptEngine::SaveFile(const std::string& name, const std::string& code, std::string& error) {
    std::string clean;
    if (!CleanName(name, clean, error)) return false;
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    std::ofstream file(Folder() / clean, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "can't write " + clean;
        return false;
    }
    file << code;
    return true;
}

bool ScriptEngine::RemoveFile(const std::string& name, std::string& error) {
    std::string clean;
    if (!CleanName(name, clean, error)) return false;
    std::error_code ec;
    if (!std::filesystem::remove(Folder() / clean, ec)) {
        error = ec ? ec.message() : "no script " + clean;
        return false;
    }
    return true;
}
