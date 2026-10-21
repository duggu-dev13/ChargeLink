#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chargelink::csms {

    class ISession {
    public:
        virtual ~ISession() = default;
        virtual const std::string& id() const = 0;
        virtual void close() = 0;  // must be safe to call from any thread
    };

    // Who is connected. Thread-safe. It never calls into a session while holding its lock,
    // and holds only weak pointers.
    class ChargerRegistry {
    public:
        struct AddResult {
            bool accepted = false;               // false once close_all() was called
            std::shared_ptr<ISession> replaced;  // older session with the same id: the CALLER closes it
        };

        AddResult add(const std::shared_ptr<ISession>& session);  // newest connection wins
        bool remove(const ISession& session);  // only if it is still the registered one for its id
        std::shared_ptr<ISession> find(const std::string& id) const;
        std::size_t size() const;
        std::vector<std::string> ids() const;  // sorted
        std::vector<std::shared_ptr<ISession>> close_all();  // refuse new sessions; return live ones

    private:
        std::vector<std::shared_ptr<ISession>> snapshot_locked() const;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::weak_ptr<ISession>> sessions_;
        bool closed_ = false;
    };

}  // namespace chargelink::csms