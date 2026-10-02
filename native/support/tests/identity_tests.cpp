#include <doctest/doctest.h>

#include <algorithm>

#include "qstate/support/identity.h"
#include "test_vectors.h"

using namespace qstate::support;

TEST_CASE("identity: core generated vectors, both directions") {
    for (const auto& v : testvec::kIdentityVectors) {
        PublicKey key;
        REQUIRE(hexToPublicKey(v.pubkeyHex, key));
        CHECK(publicKeyToIdentity(key) == v.identity);

        PublicKey back{};
        IdentityError err = IdentityError::BadLength;
        CHECK_MESSAGE(identityToPublicKey(v.identity, back, &err), v.identity);
        CHECK(err == IdentityError::None);
        CHECK(back == key);

        std::string lower = v.identity;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(c + 32); });
        CHECK(publicKeyToIdentity(key, true) == lower);
        CHECK(identityToPublicKey(lower, back));
        CHECK(back == key);
    }
}

TEST_CASE("identity: parse failures") {
    PublicKey key{};
    key.fill(0x55);
    const PublicKey untouched = key;
    IdentityError err = IdentityError::None;

    CHECK_FALSE(identityToPublicKey("", key, &err));
    CHECK(err == IdentityError::BadLength);
    CHECK_FALSE(identityToPublicKey(std::string(59, 'A'), key, &err));
    CHECK(err == IdentityError::BadLength);
    CHECK_FALSE(identityToPublicKey(std::string(61, 'A'), key, &err));
    CHECK(err == IdentityError::BadLength);

    std::string good = "BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARMID";
    REQUIRE(identityToPublicKey(good, key));

    std::string bad = good;
    bad[10] = '1';
    CHECK_FALSE(identityToPublicKey(bad, key, &err));
    CHECK(err == IdentityError::BadCharacter);

    bad = good;
    bad[59] = 'E';  // wrong checksum letter
    key = untouched;
    CHECK_FALSE(identityToPublicKey(bad, key, &err));
    CHECK(err == IdentityError::BadChecksum);
    CHECK(key == untouched);
    CHECK(std::string(identityErrorText(err)).find("checksum") != std::string::npos);

    // 14 letters 'Z' encode a value above 2^64.
    std::string big(60, 'Z');
    CHECK_FALSE(identityToPublicKey(big, key, &err));
    CHECK(err == IdentityError::ValueOutOfRange);

    CHECK(isValidIdentity(good));
    CHECK_FALSE(isValidIdentity(bad));
}

TEST_CASE("identity: digest identity of the epoch 229 computer digest") {
    std::vector<uint8_t> bytes;
    REQUIRE(fromHex("9271f1eca4512e52bc9f0b40d343e4d8cf42f5ffbaf5ec5c6a4b3391cfd8155a", bytes));
    CHECK(digestToIdentity(bytes.data()) == "kyefkvylhpkbkcikgnzscucpcuhgzjdqwsfpidjescouzahdeonipaqctgui");
}

TEST_CASE("identity: contract ids") {
    PublicKey key;
    REQUIRE(hexToPublicKey("0100000000000000000000000000000000000000000000000000000000000000", key));
    CHECK(isContractId(key) == std::optional<uint32_t>(1));
    CHECK(contractIdOf(28) == [] {
        PublicKey k{};
        k[0] = 28;
        return k;
    }());
    CHECK(publicKeyToIdentity(contractIdOf(28)) == "CBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARUCN");
    CHECK(isContractId(PublicKey{}) == std::optional<uint32_t>(0));
    CHECK_FALSE(isContractId(contractIdOf(1024)).has_value());
    CHECK(isContractId(contractIdOf(1023)) == std::optional<uint32_t>(1023));
    CHECK(isContractId(contractIdOf(5), 5) == std::nullopt);
    PublicKey other = contractIdOf(3);
    other[31] = 1;
    CHECK_FALSE(isContractId(other).has_value());
    // 16 bit indices and above: second byte is part of the index
    CHECK(isContractId(contractIdOf(300)) == std::optional<uint32_t>(300));
}

TEST_CASE("identity: asset names") {
    CHECK(assetNameToString(0x5851) == "QX");
    CHECK(assetNameToString(0) == "");
    CHECK(stringToAssetName("QX") == std::optional<uint64_t>(0x5851));
    CHECK(stringToAssetName("VOTTUN").has_value());
    CHECK(assetNameToString(*stringToAssetName("VOTTUN")) == "VOTTUN");
    CHECK(assetNameToString(*stringToAssetName("ABCDEFGH")) == "ABCDEFGH");
    CHECK_FALSE(stringToAssetName("ABCDEFGHI").has_value());
    CHECK_FALSE(stringToAssetName(std::string_view("A\0B", 3)).has_value());
}

TEST_CASE("identity: hex helpers") {
    std::vector<uint8_t> out;
    CHECK(fromHex("00ffAb", out));
    CHECK(out == std::vector<uint8_t>{0x00, 0xff, 0xab});
    CHECK(toHex(out) == "00ffab");
    CHECK(fromHex("", out));
    CHECK(out.empty());
    CHECK_FALSE(fromHex("abc", out));
    CHECK_FALSE(fromHex("zz", out));
    CHECK(out.empty());
    PublicKey k;
    CHECK_FALSE(hexToPublicKey("00", k));
    CHECK(toHex(std::array<uint8_t, 2>{1, 2}) == "0102");
}
