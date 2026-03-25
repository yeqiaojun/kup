#include "test_support.hpp"

#include <array>
#include <cstdio>
#include <filesystem>

namespace {

std::string RunCommand(const std::string &command) {
        std::array<char, 512> buffer{};
        std::string output;
#if defined(_WIN32)
        FILE *pipe = _popen(command.c_str(), "r");
#else
        FILE *pipe = popen(command.c_str(), "r");
#endif
        if (pipe == nullptr) { return {}; }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) { output += buffer.data(); }
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return output;
}

bool CommandExists(const char *command) {
#if defined(_WIN32)
        const std::string probe = std::string("where ") + command + " >nul 2>nul";
#else
        const std::string probe = std::string("command -v ") + command + " >/dev/null 2>&1";
#endif
        return std::system(probe.c_str()) == 0;
}

std::filesystem::path FindCSharpConsoleProject() {
        const auto cwd = std::filesystem::current_path();
        const std::vector<std::filesystem::path> candidates{
                cwd / "csharp" / "UkcpSharp.Console" / "UkcpSharp.Console.csproj",
                cwd.parent_path() / "csharp" / "UkcpSharp.Console" / "UkcpSharp.Console.csproj",
                cwd.parent_path().parent_path() / "csharp" / "UkcpSharp.Console" / "UkcpSharp.Console.csproj",
        };
        for (const auto &candidate : candidates) {
                if (std::filesystem::exists(candidate)) { return candidate; }
        }
        return {};
}

} // namespace

UKCP_TEST(Interop_CSharp_Client_Against_Cpp_Echo_Server) {
#if !defined(_WIN32)
        if (!CommandExists("dotnet")) { return; }
#endif

        const auto server_path = ukcp::test::FindBuiltBinary("ukcp_echo_server.exe");
        const auto console_project = FindCSharpConsoleProject();
        if (!std::filesystem::exists(server_path) || console_project.empty()) { return; }

#if defined(_WIN32)
        std::string start_command = "start /B \"ukcp_echo_server\" \"" + server_path.string() + "\" 127.0.0.1:39105";
        std::ignore = std::system(start_command.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
#else
        return;
#endif

        const std::string output = RunCommand("dotnet run --project \"" + console_project.string() + "\" -- --host 127.0.0.1 "
                                              "--port "
                                              "39105 --sess 9901 --count 5");
        UKCP_REQUIRE(output.find("RESULT sent_kcp=5 sent_udp=15 received=20 expected=20") != std::string::npos);
}
