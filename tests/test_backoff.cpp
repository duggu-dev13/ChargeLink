#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include "chargelink/util/backoff.hpp"

using chargelink::util::Backoff;
using chargelink::util::make_random_source;
using ms = std::chrono::milliseconds;

namespace {

    Backoff::Random fixed(double value) {
        return [value] { return value; };
    }

}  // namespace

TEST(Backoff, CeilingDoublesUntilTheCap) {
    Backoff b(ms(1000), ms(60000), fixed(0.0));  // u = 0 gives exactly half the ceiling
    std::vector<long long> got;
    for (int i = 0; i < 9; ++i) got.push_back(b.next().count());

    // ceilings: 1000 2000 4000 8000 16000 32000 60000 60000 60000
    std::vector<long long> expected = { 500, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    EXPECT_TRUE(got == expected);
}

TEST(Backoff, JitterStaysBetweenHalfAndTheCeiling) {
    Backoff low(ms(1000), ms(60000), fixed(0.0));
    Backoff high(ms(1000), ms(60000), fixed(0.999));
    for (int i = 0; i < 5; ++i) {
        const long long ceiling = std::min<long long>(1000LL << i, 60000);
        const long long a = low.next().count();
        const long long b = high.next().count();
        EXPECT_EQ(a, ceiling / 2);
        EXPECT_TRUE(b >= ceiling / 2);
        EXPECT_TRUE(b < ceiling);
        EXPECT_TRUE(b > a);
    }
}

TEST(Backoff, ResetStartsOver) {
    Backoff b(ms(1000), ms(60000), fixed(0.0));
    b.next();
    b.next();
    b.next();
    EXPECT_EQ(b.attempts(), 3u);
    b.reset();
    EXPECT_EQ(b.attempts(), 0u);
    EXPECT_EQ(b.next().count(), 500);  // back to the first delay
}

TEST(Backoff, ManyAttemptsNeverOverflowOrExceedTheCap) {
    Backoff b(ms(1000), ms(60000), fixed(0.5));
    for (int i = 0; i < 10000; ++i) {
        const auto d = b.next();
        EXPECT_TRUE(d.count() >= 1);
        EXPECT_TRUE(d.count() < 60000);
    }
    EXPECT_TRUE(b.attempts() <= 64u);
}

TEST(Backoff, BadRandomValuesAreClamped) {
    for (double bad : {-1.0, 5.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
        Backoff b(ms(1000), ms(60000), fixed(bad));
        const auto d = b.next();
        EXPECT_TRUE(d.count() >= 500);
        EXPECT_TRUE(d.count() < 1000);
    }
}

TEST(Backoff, MaxIsNeverBelowBase) {
    Backoff b(ms(5000), ms(1000), fixed(0.0));  // nonsense settings: max is raised to base
    EXPECT_EQ(b.next().count(), 2500);
    EXPECT_EQ(b.next().count(), 2500);  // no growth: base == max
}

TEST(Backoff, ZeroBaseIsTreatedAsOneMillisecond) {
    Backoff b(ms(0), ms(0), fixed(0.5));
    EXPECT_EQ(b.next().count(), 1);
}

TEST(Backoff, DifferentSourcesSpreadTheChargersOut) {
    // The point of jitter: chargers that lose the server together must not retry together.
    std::set<long long> first_delays;
    for (unsigned seed = 1; seed <= 5; ++seed) {
        auto engine = std::make_shared<std::mt19937>(seed);
        Backoff b(ms(1000), ms(60000), [engine] {
            return std::uniform_real_distribution<double>(0.0, 1.0)(*engine);
            });
        first_delays.insert(b.next().count());
    }
    EXPECT_TRUE(first_delays.size() >= 2);
}

TEST(Backoff, DefaultRandomSourceWorks) {
    Backoff b(ms(100), ms(1000), nullptr);  // null means "use a real random source"
    const auto d = b.next();
    EXPECT_TRUE(d.count() >= 50);
    EXPECT_TRUE(d.count() < 100);

    auto random = make_random_source();
    for (int i = 0; i < 100; ++i) {
        const double u = random();
        EXPECT_TRUE(u >= 0.0);
        EXPECT_TRUE(u <= 1.0);
    }
}