#include "Events.hpp"

#include <algorithm>
#include <shared_mutex>

namespace Events {
    namespace {
        std::shared_mutex gMutex;
        std::vector<Listener*> gListeners;
    }

    std::atomic<int> Detail::listeners{ 0 };

    void Subscribe(Listener* listener) {
        std::unique_lock lock(gMutex);
        gListeners.push_back(listener);
        Detail::listeners = static_cast<int>(gListeners.size());
    }

    void Unsubscribe(Listener* listener) {
        std::unique_lock lock(gMutex);
        std::erase(gListeners, listener);
        Detail::listeners = static_cast<int>(gListeners.size());
    }

    bool Detail::AnyWants(std::string_view name) {
        std::shared_lock lock(gMutex);
        return std::ranges::any_of(gListeners, [&](const Listener* l) { return l->Wants(name); });
    }

    void Emit(const Event& event) {
        std::shared_lock lock(gMutex);
        for (Listener* listener : gListeners) {
            if (listener->Wants(event.name)) listener->Push(event);
        }
    }
}
