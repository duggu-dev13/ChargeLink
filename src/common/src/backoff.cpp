#include "chargelink/util/backoff.hpp"

#include <algorithm>
#include <memory>
#include <random>
#include <utility>

namespace chargelink::util {

    Backoff::Random make_random_source() {
        auto engine = std::make_shared<std::mt19937_64>(std::random_device{}());
        return [engine] { return std::uniform_real_distribution<double>(0.0, 1.0)(*engine); };
    }

    Backoff::Backoff(std::chrono::milliseconds base, std::chrono::milliseconds max, Random random)
        : base_(std::max(base, std::chrono::milliseconds(1))),
        max_(std::max(max, base_)),
        random_(random ? std::move(random) : make_random_source()) {
    }

    std::chrono::milliseconds Backoff::next() {
        // ceiling = min(max, base * 2^attempts), computed without ever overflowing
        std::chrono::milliseconds ceiling = base_;
        for (unsigned i = 0; i < attempts_ && ceiling < max_; ++i) {
            ceiling *= 2;
        }
        ceiling = std::min(ceiling, max_);

        if (attempts_ < 64) {
            ++attempts_;  // past this the ceiling no longer changes; do not let it wrap
        }

        double u = random_();
        if (!(u >= 0.0)) u = 0.0;           // also catches NaN
        if (u > 0.999999) u = 0.999999;     // keep the result strictly below the ceiling

        const long long half = ceiling.count() / 2;
        const long long span = ceiling.count() - half;
        const long long delay = half + static_cast<long long>(u * static_cast<double>(span));
        return std::chrono::milliseconds(std::max<long long>(delay, 1));
    }

    void Backoff::reset() {
        attempts_ = 0;
    }

}  // namespace chargelink::util