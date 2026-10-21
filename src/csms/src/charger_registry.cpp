#include "chargelink/csms/charger_registry.hpp"

#include <algorithm>
#include <utility>

namespace chargelink::csms {

    ChargerRegistry::AddResult ChargerRegistry::add(const std::shared_ptr<ISession>& session) {
        AddResult result;
        if (!session) {
            return result;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return result;
        }
        std::weak_ptr<ISession>& slot = sessions_[session->id()];
        std::shared_ptr<ISession> previous = slot.lock();
        if (previous.get() != session.get()) {
            result.replaced = std::move(previous);  // released outside the lock, by the caller
        }
        slot = session;
        result.accepted = true;
        return result;
    }

    bool ChargerRegistry::remove(const ISession& session) {
        std::shared_ptr<ISession> other;  // declared before the lock: released after it
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session.id());
        if (it == sessions_.end()) {
            return false;
        }
        other = it->second.lock();
        if (other && other.get() != &session) {
            return false;  // someone else has taken over this id
        }
        sessions_.erase(it);
        return true;
    }

    std::shared_ptr<ISession> ChargerRegistry::find(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second.lock();
    }

    std::size_t ChargerRegistry::size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(
            std::count_if(sessions_.begin(), sessions_.end(),
                [](const auto& entry) { return !entry.second.expired(); }));
    }

    std::vector<std::string> ChargerRegistry::ids() const {
        std::vector<std::string> out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : sessions_) {
                if (!entry.second.expired()) {
                    out.push_back(entry.first);
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::shared_ptr<ISession>> ChargerRegistry::snapshot_locked() const {
        std::vector<std::shared_ptr<ISession>> out;
        for (const auto& entry : sessions_) {
            if (auto session = entry.second.lock()) {
                out.push_back(std::move(session));
            }
        }
        return out;
    }

    std::vector<std::shared_ptr<ISession>> ChargerRegistry::close_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        return snapshot_locked();
    }

}  // namespace chargelink::csms