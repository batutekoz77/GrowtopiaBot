#include "BotManager.hpp"
#include "../Core/Log.hpp"
#include "../Core/Text.hpp"

#include <thread>

BotManager::BotManager(Config::Settings settings) : settings_(std::move(settings)) {
    for (const auto& entry : settings_.bots) bots_.push_back(std::make_shared<Bot>(nextId_++, entry, settings_));
}

BotManager::~BotManager() { StopAll(); }

void BotManager::StartAll() {
    const auto bots = All();
    int interval = 0;
    {
        std::lock_guard lock(mutex_);
        interval = settings_.loginIntervalMs;
    }
    int slot = 0;
    for (const auto& bot : bots) {
        const BotStatus status = bot->Status();
        if (bot->Running() && status != BotStatus::Offline && status != BotStatus::Stopped) continue;
        bot->Connect(slot++ * interval);
    }
}

void BotManager::StopAll() {
    const auto bots = All();
    std::vector<std::thread> stoppers;
    stoppers.reserve(bots.size());
    for (const auto& bot : bots) stoppers.emplace_back([bot] { bot->Stop(); });
    for (auto& t : stoppers) t.join();
}

void BotManager::DisconnectAll() {
    for (const auto& bot : All()) bot->Disconnect();
}

size_t BotManager::Count() const {
    std::lock_guard lock(mutex_);
    return bots_.size();
}

std::vector<std::shared_ptr<Bot>> BotManager::All() const {
    std::lock_guard lock(mutex_);
    return bots_;
}

std::shared_ptr<Bot> BotManager::ById(int id) const {
    std::lock_guard lock(mutex_);
    for (const auto& bot : bots_) {
        if (bot->Id() == id) return bot;
    }
    return nullptr;
}

std::shared_ptr<Bot> BotManager::Find(const std::string& nameOrNumber) const {
    std::lock_guard lock(mutex_);
    if (const auto number = Text::ToInt(nameOrNumber); number && *number >= 1) {
        return static_cast<size_t>(*number) <= bots_.size() ? bots_[static_cast<size_t>(*number - 1)] : nullptr;
    }
    const std::string wanted = Text::Lower(Text::Trim(nameOrNumber));
    for (const auto& bot : bots_) {
        if (Text::Lower(bot->GrowId()) == wanted) return bot;
    }
    for (const auto& bot : bots_) {
        if (Text::Lower(bot->Read([](const BotSnapshot& s) { return s.name; })) == wanted) return bot;
    }
    return nullptr;
}

std::shared_ptr<Bot> BotManager::AddBot(const std::string& growId, const std::string& password, const std::string& proxy, std::string& error) {
    Config::BotEntry entry;
    entry.growId = std::string(Text::Trim(growId));
    entry.password = password;
    if (entry.growId.empty() || entry.password.empty()) {
        error = "GrowID and password are required";
        return nullptr;
    }
    if (!Config::SetProxy(entry, proxy)) {
        error = "bad proxy -- use socks5://user:pass@host:port or host:port:user:pass";
        return nullptr;
    }

    std::lock_guard lock(mutex_);
    for (const auto& bot : bots_) {
        if (Text::Lower(bot->GrowId()) == Text::Lower(entry.growId)) {
            error = entry.growId + " is already in the list";
            return nullptr;
        }
    }
    auto bot = std::make_shared<Bot>(nextId_++, entry, settings_);
    settings_.bots.push_back(std::move(entry));
    bots_.push_back(bot);
    Log::Info("MAIN", "added bot {}", bot->GrowId());
    return bot;
}

bool BotManager::RemoveBot(int id) {
    std::shared_ptr<Bot> removed;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < bots_.size(); ++i) {
            if (bots_[i]->Id() != id) continue;
            removed = bots_[i];
            bots_.erase(bots_.begin() + static_cast<std::ptrdiff_t>(i));
            std::erase_if(settings_.bots, [&](const Config::BotEntry& e) { return e.growId == removed->GrowId(); });
            break;
        }
    }
    if (!removed) return false;
    removed->Stop();
    Log::Info("MAIN", "removed bot {}", removed->GrowId());
    return true;
}

Config::Settings BotManager::Settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

void BotManager::SetCommon(const Config::Common& common) {
    std::lock_guard lock(mutex_);
    static_cast<Config::Common&>(settings_) = common;
    Log::EnableDebug(common.debug);
}
