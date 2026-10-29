#pragma once

#include "Bot.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class BotManager {
public:
    explicit BotManager(Config::Settings settings);
    ~BotManager();

    void StartAll();
    void StopAll();
    void DisconnectAll();

    size_t Count() const;
    std::vector<std::shared_ptr<Bot>> All() const;
    std::shared_ptr<Bot> ById(int id) const;
    std::shared_ptr<Bot> Find(const std::string& nameOrNumber) const;

    std::shared_ptr<Bot> AddBot(const std::string& growId, const std::string& password, const std::string& proxy, std::string& error);
    bool RemoveBot(int id);

    Config::Settings Settings() const;
    void SetCommon(const Config::Common& common);

private:
    mutable std::mutex mutex_;
    Config::Settings settings_;
    std::vector<std::shared_ptr<Bot>> bots_;
    int nextId_ = 1;
};
