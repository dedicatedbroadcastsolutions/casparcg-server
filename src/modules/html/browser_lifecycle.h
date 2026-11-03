#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace caspar::html {

// Accessed only on the CEF UI thread. Entries include pending browser creations.
class browser_lifecycle
{
    std::map<const void*, std::function<void()>> clients_;
    std::function<void()>                        on_closed_;
    bool                                         shutting_down_ = false;

  public:
    explicit browser_lifecycle(std::function<void()> on_closed)
        : on_closed_(std::move(on_closed))
    {
    }

    void add(const void* client, std::function<void()> close)
    {
        if (shutting_down_ || !clients_.emplace(client, std::move(close)).second) {
            throw std::logic_error("Cannot register HTML browser during shutdown or register it twice.");
        }
    }

    void remove(const void* client)
    {
        if (clients_.erase(client) != 1) {
            throw std::logic_error("Cannot unregister an unknown HTML browser.");
        }
        if (shutting_down_ && clients_.empty()) {
            on_closed_();
        }
    }

    void shutdown()
    {
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        if (clients_.empty()) {
            on_closed_();
            return;
        }
        // Close callbacks can synchronously unregister clients.
        const auto clients = clients_;
        for (const auto& [client, close] : clients) {
            close();
        }
    }
};

} // namespace caspar::html
