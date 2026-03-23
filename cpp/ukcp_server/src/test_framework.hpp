#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ukcp::test {

using TestFn = std::function<void()>;

struct TestCase {
        std::string name;
        TestFn fn;
};

std::vector<TestCase> &Registry();
void Register(std::string name, TestFn fn);
int RunAll(std::string_view filter = {});

inline void Require(bool condition, std::string_view message) {
        if (!condition) { throw std::runtime_error(std::string(message)); }
}

} // namespace ukcp::test

#define UKCP_TEST(name)                                                                                                                                        \
        static void name();                                                                                                                                    \
        namespace {                                                                                                                                            \
        struct name##_registrar {                                                                                                                              \
                name##_registrar() { ukcp::test::Register(#name, &name); }                                                                                     \
        } name##_registrar_instance;                                                                                                                           \
        }                                                                                                                                                      \
        static void name()

#define UKCP_REQUIRE(cond) ukcp::test::Require((cond), #cond)
