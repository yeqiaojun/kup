#include "test_framework.hpp"
#include "ukcp/protocol.hpp"

using ukcp::Header;
using ukcp::HeaderFlags;
using ukcp::MsgType;

UKCP_TEST(Protocol_EncodeDecodeLittleEndian) {
        Header input{};
        input.msg_type = MsgType::Kcp;
        input.flags = HeaderFlags::Connect;
        input.body_len = 5;
        input.sess_id = 0x11223344;
        input.packet_seq = 0x55667788;

        const auto bytes = input.Encode();
        UKCP_REQUIRE(bytes[0] == 0x02);
        UKCP_REQUIRE(bytes[1] == 0x01);
        UKCP_REQUIRE(bytes[2] == 0x05);
        UKCP_REQUIRE(bytes[3] == 0x00);

        Header output{};
        UKCP_REQUIRE(Header::Decode(bytes, output));
        UKCP_REQUIRE(output.msg_type == MsgType::Kcp);
        UKCP_REQUIRE(output.flags == HeaderFlags::Connect);
        UKCP_REQUIRE(output.body_len == 5);
        UKCP_REQUIRE(output.sess_id == 0x11223344);
        UKCP_REQUIRE(output.packet_seq == 0x55667788);
}
