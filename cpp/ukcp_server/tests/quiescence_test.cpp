#include "quiescence.hpp"
#include "test_framework.hpp"

using ukcp::QuiescenceDrain;

UKCP_TEST(QuiescenceDrain_ExtendsDeadlineOnProgress) {
        QuiescenceDrain drain(std::chrono::milliseconds(50));
        const auto start = std::chrono::steady_clock::now();

        drain.Observe(10, start);
        UKCP_REQUIRE(!drain.Done(start + std::chrono::milliseconds(40)));

        drain.Observe(11, start + std::chrono::milliseconds(40));
        UKCP_REQUIRE(!drain.Done(start + std::chrono::milliseconds(80)));
        UKCP_REQUIRE(drain.Done(start + std::chrono::milliseconds(91)));
}

UKCP_TEST(QuiescenceDrain_DoesNotResetWithoutProgress) {
        QuiescenceDrain drain(std::chrono::milliseconds(30));
        const auto start = std::chrono::steady_clock::now();

        drain.Observe(5, start);
        drain.Observe(5, start + std::chrono::milliseconds(20));

        UKCP_REQUIRE(drain.Done(start + std::chrono::milliseconds(31)));
}
