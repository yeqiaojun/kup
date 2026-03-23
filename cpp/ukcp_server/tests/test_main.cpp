#include "test_framework.hpp"

#include <cstdlib>
#include <iostream>

namespace ukcp::test {

std::vector<TestCase> &Registry() {
        static std::vector<TestCase> registry;
        return registry;
}

void Register(std::string name, TestFn fn) { Registry().push_back(TestCase{std::move(name), std::move(fn)}); }

int RunAll(std::string_view filter) {
        int failed = 0;
        for (const auto &test : Registry()) {
                if (!filter.empty() && test.name.find(filter) == std::string::npos) { continue; }
                try {
                        test.fn();
                        std::cout << "[PASS] " << test.name << '\n';
                } catch (const std::exception &ex) {
                        ++failed;
                        std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
                }
        }
        return failed == 0 ? 0 : 1;
}

} // namespace ukcp::test

int main() {
        const char *filter = std::getenv("UKCP_TEST_FILTER");
        return ukcp::test::RunAll(filter == nullptr ? std::string_view{} : std::string_view(filter));
}
