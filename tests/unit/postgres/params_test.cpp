#include "core/util/uuid.hpp"

#include "params.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using infra::postgres::encode_bytea_array;
using infra::postgres::kBoolOid;
using infra::postgres::kByteaArrayOid;
using infra::postgres::kByteaOid;
using infra::postgres::kInt8Oid;
using infra::postgres::kTextOid;
using infra::postgres::kUuidOid;
using infra::postgres::Params;

std::string bytes_of(const Params::Wire& wire, std::size_t i) {
    return {wire.values.at(i), static_cast<std::size_t>(wire.lengths.at(i))};
}

TEST(Params, IntegersAreBigEndianInt8) {
    Params params;
    params.add_int(0x0102030405060708);
    const Params::Wire wire = params.wire();
    ASSERT_EQ(wire.count, 1);
    EXPECT_EQ(wire.types[0], kInt8Oid);
    EXPECT_EQ(wire.formats[0], 1);
    EXPECT_EQ(bytes_of(wire, 0), std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

TEST(Params, NegativeIntegersAreTwosComplement) {
    Params params;
    params.add_int(-2);
    EXPECT_EQ(bytes_of(params.wire(), 0), std::string("\xff\xff\xff\xff\xff\xff\xff\xfe", 8));
}

TEST(Params, UuidIsItsSixteenBytes) {
    const auto uuid = core::Uuid::parse("01890a5d-ac96-774b-bcce-b302099a8057");
    ASSERT_TRUE(uuid);
    Params params;
    params.add_uuid(*uuid);
    const Params::Wire wire = params.wire();
    EXPECT_EQ(wire.types[0], kUuidOid);
    EXPECT_EQ(bytes_of(wire, 0),
              std::string("\x01\x89\x0a\x5d\xac\x96\x77\x4b\xbc\xce\xb3\x02\x09\x9a\x80\x57", 16));
}

TEST(Params, TextBindsTheCallersBytesWithoutACopy) {
    const std::string title = "holiday, day one";
    Params params;
    params.add_text(title);
    const Params::Wire wire = params.wire();
    EXPECT_EQ(wire.types[0], kTextOid);
    EXPECT_EQ(wire.formats[0], 1);
    EXPECT_EQ(wire.values[0], title.data());
    EXPECT_EQ(wire.lengths[0], static_cast<int>(title.size()));
}

TEST(Params, EmptyTextIsAnEmptyValueNotNull) {
    Params params;
    params.add_text(std::string_view{});
    const Params::Wire wire = params.wire();
    EXPECT_NE(wire.values[0], nullptr);
    EXPECT_EQ(wire.lengths[0], 0);
}

TEST(Params, BoolIsOneByte) {
    Params params;
    params.add_bool(true).add_bool(false);
    const Params::Wire wire = params.wire();
    EXPECT_EQ(wire.types[1], kBoolOid);
    EXPECT_EQ(bytes_of(wire, 0), std::string(1, '\x01'));
    EXPECT_EQ(bytes_of(wire, 1), std::string(1, '\x00'));
}

TEST(Params, WireListsParametersInTheOrderAdded) {
    const std::string owner = "auth0|42";
    Params params;
    params.add_text(owner).add_int(7).add_bool(true);
    const Params::Wire wire = params.wire();
    ASSERT_EQ(wire.count, 3);
    EXPECT_EQ((std::array{wire.types[0], wire.types[1], wire.types[2]}),
              (std::array{kTextOid, kInt8Oid, kBoolOid}));
    EXPECT_EQ(wire.values[3], nullptr);
}

TEST(Params, ByteaBindsTheCallersBytesWithoutACopy) {
    const std::vector<std::byte> body{std::byte{0x00}, std::byte{0x5c}, std::byte{0xff}};
    Params params;
    params.add_bytea(body);
    const Params::Wire wire = params.wire();
    EXPECT_EQ(wire.types[0], kByteaOid);
    EXPECT_EQ(wire.formats[0], 1);
    EXPECT_EQ(static_cast<const void*>(wire.values[0]), static_cast<const void*>(body.data()));
    EXPECT_EQ(bytes_of(wire, 0), std::string("\x00\x5c\xff", 3));
}

TEST(Params, ByteaArrayIsTheBinaryArrayFormat) {
    const std::vector<std::vector<std::byte>> elements{{std::byte{0xab}},
                                                       {std::byte{0x01}, std::byte{0x02}}};
    const std::string encoded = encode_bytea_array(elements);
    // ndim 1, no nulls, element type 17 (bytea), 2 elements from index 1, then each element's
    // length and bytes.
    EXPECT_EQ(encoded, std::string("\0\0\0\1"
                                   "\0\0\0\0"
                                   "\0\0\0\x11"
                                   "\0\0\0\2"
                                   "\0\0\0\1"
                                   "\0\0\0\1\xab"
                                   "\0\0\0\2\1\2",
                                   31));
    Params params;
    params.add_bytea_array(encoded);
    EXPECT_EQ(params.wire().types[0], kByteaArrayOid);
    EXPECT_EQ(bytes_of(params.wire(), 0), encoded);
}

TEST(Params, EmptyByteaArrayHasNoDimension) {
    EXPECT_EQ(encode_bytea_array({}), std::string("\0\0\0\0"
                                                  "\0\0\0\0"
                                                  "\0\0\0\x11",
                                                  12));
}

} // namespace
