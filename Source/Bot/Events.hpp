#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Events {
    using Value = std::variant<bool, int64_t, double, std::string>;

    struct Event {
        int bot = 0;
        std::string name;
        std::vector<Value> args;
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual bool Wants(std::string_view name) const = 0;
        virtual void Push(const Event& event) = 0;
    };

    void Subscribe(Listener* listener);
    void Unsubscribe(Listener* listener);

    namespace Detail { extern std::atomic<int> listeners; bool AnyWants(std::string_view name); }

    inline bool Wanted(std::string_view name) {
        return Detail::listeners.load(std::memory_order_relaxed) > 0 && Detail::AnyWants(name);
    }
    void Emit(const Event& event);
}
