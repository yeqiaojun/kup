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

} // namespace

UKCP_TEST(SplitPerf_ServerClient_Smoke) {
#if defined(_WIN32)
        const auto server_path = ukcp::test::FindBuiltBinary("ukcp_perf_server.exe");
        const auto client_path = ukcp::test::FindBuiltBinary("ukcp_perf_client.exe");
#else
        const auto server_path = ukcp::test::FindBuiltBinary("ukcp_perf_server");
        const auto client_path = ukcp::test::FindBuiltBinary("ukcp_perf_client");
#endif
        UKCP_REQUIRE(std::filesystem::exists(server_path));
        UKCP_REQUIRE(std::filesystem::exists(client_path));

#if defined(_WIN32)
        const std::string port = "39107";
        std::string start_command = "start /B \"ukcp_perf_server\" \"" + server_path.string() + "\" --listen 127.0.0.1:" + port +
                                    " --seconds 4 --downlink-pps 4 --downlink-payload-bytes 15";
        std::ignore = std::system(start_command.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const std::string output = RunCommand("\"" + client_path.string() + "\" --server 127.0.0.1:" + port +
                                              " --clients 16 --client-workers 2 --seconds 2 --uplink-pps 2 --downlink-pps 4 "
                                              "--uplink-payload-bytes 10 --downlink-payload-bytes 15");
        UKCP_REQUIRE(output.find("[FIXED RATE CLIENT]") != std::string::npos);
#endif
}

UKCP_TEST(SplitPerf_RoomMode_Smoke) {
#if defined(_WIN32)
        const auto server_path = ukcp::test::FindBuiltBinary("ukcp_perf_server.exe");
        const auto client_path = ukcp::test::FindBuiltBinary("ukcp_perf_client.exe");
        UKCP_REQUIRE(std::filesystem::exists(server_path));
        UKCP_REQUIRE(std::filesystem::exists(client_path));

        const std::string port = "39108";
        std::string start_command = "start /B \"ukcp_perf_server_room\" \"" + server_path.string() + "\" --listen 127.0.0.1:" + port +
                                    " --mode room --rooms 2 --room-size 10 --seconds 3 --room-fps 5";
        std::ignore = std::system(start_command.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const std::string output = RunCommand("\"" + client_path.string() + "\" --server 127.0.0.1:" + port +
                                              " --mode room --clients 20 --client-workers 2 --seconds 2 --room-size 10 "
                                              "--room-fps 5 --kcp-pps 3 --udp-pps 5");
        UKCP_REQUIRE(output.find("[ROOM CLIENT]") != std::string::npos);
#endif
}
