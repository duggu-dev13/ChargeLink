#pragma once

#include <chrono>
#include <functional>

namespace chargelink::util {

    // Exponential backoff with jitter, for reconnecting.
    //
    // The delay before attempt n is a random value in [ceiling/2, ceiling), where
    // ceiling = min(max, base * 2^n). The randomness ("jitter") stops thousands of chargers
    // that lost the same server from reconnecting in the same instant.
    //
    // Pure logic: the random source is injected, so tests are deterministic.
    class Backoff {
    public:
        using Random = std::function<double()>;  // uniform in [0, 1)

        // `max` is never allowed below `base`; `base` is at least 1 ms.
        // A null `random` means "use a real random source".
        Backoff(std::chrono::milliseconds base, std::chrono::milliseconds max, Random random);

        std::chrono::milliseconds next();  // the delay before the next attempt; grows each call
        void reset();                      // a connection worked: start from the beginning
        unsigned attempts() const { return attempts_; }

    private:
        std::chrono::milliseconds base_;
        std::chrono::milliseconds max_;
        Random random_;
        unsigned attempts_ = 0;
    };

    // A random source seeded from the operating system.
    Backoff::Random make_random_source();

}  // namespace chargelink::util