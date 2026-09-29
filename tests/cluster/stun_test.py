#!/usr/bin/env python3
"""stun.py against the test vectors of RFC 5769, which were produced by other implementations:
a codec that got the XOR, the padding, the HMAC's length rule or the CRC wrong fails here
instead of against STUNner, where every failure looks like a network problem."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stun  # noqa: E402

# RFC 5769 section 2.1: a Binding request with short-term credentials.
REQUEST = bytes.fromhex(
    "000100582112a442b7e7a701bc34d686fa87dfae"
    "80220010" + b"STUN test client".hex()
    + "00240004" "6e0001ff"
    "80290008" "932ff9b151263b36"
    "00060009" "6576746a3a68367659202020"
    "00080014" "9aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2"
    "80280004" "e57a3bcf")
# Section 2.2: its IPv4 success response, mapped address 192.0.2.1:32853.
RESPONSE_V4 = bytes.fromhex(
    "0101003c2112a442b7e7a701bc34d686fa87dfae"
    "8022000b" "7465737420766563746f7220"
    "00200008" "0001a147e112a643"
    "00080014" "2b91f599fd9e90c38c7489f92af9ba53f06be7d7"
    "80280004" "c07d4c96")
# Section 2.3: the IPv6 response, mapped address [2001:db8:1234:5678:11:2233:4455:6677]:32853.
RESPONSE_V6 = bytes.fromhex(
    "010100482112a442b7e7a701bc34d686fa87dfae"
    "8022000b" "7465737420766563746f7220"
    "00200014" "0002a147" "0113a9faa5d3f179bc25f4b5bed2b9d9"
    "00080014" "a382954e4be67bf11784c97c8292c275bfe3ed41"
    "80280004" "c8fb0b4c")
SHORT_TERM_PASSWORD = b"VOkJxbRl1RmTxUk/WvJxBt"
# Section 2.4: a request with long-term credentials, no FINGERPRINT.
LONG_TERM_REQUEST = bytes.fromhex(
    "000100602112a442" "78ad3433c6ad72c029da412e"
    "00060012" "e3839ee38388e383aae38383e382afe382b90000"
    "0015001c" + b"f//499k954d6OL34oL9FSTvy64sA".hex()
    + "0014000b" "6578616d706c652e6f726700"
    "00080014" "f67024656dd64a3e02b8e0712e85c9a28ca89666")


class VectorTest(unittest.TestCase):
    def test_the_ipv4_response_decodes_to_the_published_address(self):
        message = stun.decode(RESPONSE_V4)
        self.assertEqual((message.method, message.cls), (stun.BINDING, stun.SUCCESS))
        self.assertEqual(message.xor_address(stun.XOR_MAPPED_ADDRESS), ("192.0.2.1", 32853))
        self.assertEqual(message.get(stun.SOFTWARE), b"test vector")

    def test_the_ipv6_response_decodes_to_the_published_address(self):
        message = stun.decode(RESPONSE_V6)
        self.assertEqual(message.xor_address(stun.XOR_MAPPED_ADDRESS),
                         ("2001:db8:1234:5678:11:2233:4455:6677", 32853))

    def test_published_integrity_and_fingerprints_verify(self):
        for raw in (REQUEST, RESPONSE_V4, RESPONSE_V6):
            message = stun.decode(raw)
            self.assertTrue(stun.integrity_ok(message, SHORT_TERM_PASSWORD))
            self.assertTrue(stun.fingerprint_ok(message))

    def test_the_long_term_key_verifies_the_published_request(self):
        message = stun.decode(LONG_TERM_REQUEST)
        username = message.get(stun.USERNAME).decode()
        self.assertEqual(username, "マトリックス")
        self.assertEqual(message.get(stun.REALM), b"example.org")
        key = stun.long_term_key(username, "example.org", "TheMatrIX")
        self.assertTrue(stun.integrity_ok(message, key))
        self.assertFalse(stun.integrity_ok(message, stun.long_term_key(username, "example.org",
                                                                        "TheMatrix")))

    def test_encoding_the_published_response_reproduces_its_attributes_and_verifies(self):
        # Byte for byte is out of reach: the vector pads SOFTWARE with a space, and padding
        # content is the sender's choice (section 15), covered by the HMAC and the CRC.
        message = stun.decode(RESPONSE_V4)
        rebuilt = stun.decode(stun.encode(stun.BINDING, stun.SUCCESS, message.transaction_id, [
            (stun.SOFTWARE, b"test vector"),
            (stun.XOR_MAPPED_ADDRESS,
             stun.encode_xor_address("192.0.2.1", 32853, message.transaction_id)),
        ], key=SHORT_TERM_PASSWORD))
        self.assertEqual(len(rebuilt.raw), len(RESPONSE_V4))
        self.assertEqual(rebuilt.raw[:4], RESPONSE_V4[:4])
        self.assertEqual([k for k, _ in rebuilt.attributes], [k for k, _ in message.attributes])
        self.assertEqual(rebuilt.get(stun.XOR_MAPPED_ADDRESS),
                         message.get(stun.XOR_MAPPED_ADDRESS))
        self.assertTrue(stun.integrity_ok(rebuilt, SHORT_TERM_PASSWORD))
        self.assertTrue(stun.fingerprint_ok(rebuilt))

    def test_an_ipv6_address_encodes_as_published(self):
        message = stun.decode(RESPONSE_V6)
        self.assertEqual(stun.encode_xor_address("2001:db8:1234:5678:11:2233:4455:6677", 32853,
                                                 message.transaction_id),
                         message.get(stun.XOR_MAPPED_ADDRESS))


class CodecTest(unittest.TestCase):
    def test_message_types_place_the_class_bits_between_the_method_bits(self):
        self.assertEqual(stun.message_type(stun.BINDING, stun.REQUEST), 0x0001)
        self.assertEqual(stun.message_type(stun.BINDING, stun.SUCCESS), 0x0101)
        self.assertEqual(stun.message_type(stun.ALLOCATE, stun.ERROR), 0x0113)
        self.assertEqual(stun.message_type(stun.CREATE_PERMISSION, stun.SUCCESS), 0x0108)
        self.assertEqual(stun.split_type(0x0113), (stun.ALLOCATE, stun.ERROR))

    def test_a_flipped_byte_fails_integrity_and_fingerprint(self):
        tampered = bytearray(RESPONSE_V4)
        tampered[31] ^= 0x01  # inside XOR-MAPPED-ADDRESS
        message = stun.decode(bytes(tampered))
        self.assertFalse(stun.integrity_ok(message, SHORT_TERM_PASSWORD))
        self.assertFalse(stun.fingerprint_ok(message))

    def test_error_code_splits_class_and_number(self):
        raw = stun.encode(stun.ALLOCATE, stun.ERROR, b"\1" * 12,
                          [(stun.ERROR_CODE, b"\0\0\x04\x01Unauthorized")])
        self.assertEqual(stun.decode(raw).error(), (401, "Unauthorized"))

    def test_malformed_datagrams_are_rejected(self):
        for data in (b"", RESPONSE_V4[:-1], b"\x40" + RESPONSE_V4[1:],
                     RESPONSE_V4[:4] + b"\0\0\0\0" + RESPONSE_V4[8:]):
            with self.assertRaises(stun.Malformed):
                stun.decode(data)

    def test_an_attribute_running_past_the_end_is_rejected(self):
        raw = bytearray(RESPONSE_V4)
        raw[22:24] = b"\x00\xff"  # SOFTWARE's length
        with self.assertRaises(stun.Malformed):
            stun.decode(bytes(raw))

    def test_ephemeral_password_matches_an_independent_hmac(self):
        # printf '1700000000:alice' | openssl dgst -sha1 -hmac ulw-sandbox-turn-testtest123 \
        #   -binary | base64
        self.assertEqual(stun.ephemeral_credentials("ulw-sandbox-turn-testtest123", "alice",
                                                    1700000000),
                         ("1700000000:alice", "t7bpEZ9AkvRCOJHsEHYGDKUrR18="))


if __name__ == "__main__":
    unittest.main()
