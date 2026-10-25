// Standalone check of indra/newview/slupdateverify.cpp against a manifest produced by
// sign_update_manifest.py. Driven by test_update_verify.sh.
// usage: test_update_verify FILE.json PUBKEY_HEX OTHER_PUBKEY_HEX ARTIFACT_FILE ARTIFACT_SHA256
#include "slupdateverify.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static std::string json_field(const std::string& doc, const std::string& key)
{
    // The test file is {"manifest": "<escaped json>", "signature": "...", "key_id": "1"}.
    std::smatch m;
    std::regex re("\"" + key + "\": \"((?:[^\"\\\\]|\\\\.)*)\"");
    if (!std::regex_search(doc, m, re)) return std::string();
    std::string v = m[1], out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == '\\' && i + 1 < v.size()) { ++i; out += v[i]; }
        else out += v[i];
    }
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 6) return 2;
    std::ifstream in(argv[1]);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string doc = ss.str();
    const std::string manifest = json_field(doc, "manifest");
    const std::string sig = json_field(doc, "signature");
    const std::string pub = argv[2], other = argv[3];

    CHECK(!manifest.empty() && !sig.empty());
    CHECK(SLUpdateVerify::keyConfigured(pub.c_str()));
    CHECK(!SLUpdateVerify::keyConfigured(""));                       // placeholder = not configured
    CHECK(!SLUpdateVerify::keyConfigured(("zz" + pub.substr(2)).c_str()));
    CHECK(SLUpdateVerify::verifyManifest(manifest, sig, pub.c_str()));
    CHECK(!SLUpdateVerify::verifyManifest(manifest, sig, other.c_str()));    // wrong key
    CHECK(!SLUpdateVerify::verifyManifest(manifest, sig, ""));       // no key: never verifies
    std::string tampered = manifest;
    tampered[tampered.size() / 2] ^= 1;
    CHECK(!SLUpdateVerify::verifyManifest(tampered, sig, pub.c_str()));      // altered manifest
    CHECK(!SLUpdateVerify::verifyManifest(manifest + " ", sig, pub.c_str()));
    CHECK(!SLUpdateVerify::verifyManifest(manifest, "AAAA", pub.c_str()));   // short signature
    CHECK(!SLUpdateVerify::verifyManifest(manifest, "!!!!", pub.c_str()));   // not base64
    CHECK(!SLUpdateVerify::verifyManifest(manifest, "", pub.c_str()));
    std::string bad_sig = sig;
    bad_sig[2] = bad_sig[2] == 'A' ? 'B' : 'A';
    CHECK(!SLUpdateVerify::verifyManifest(manifest, bad_sig, pub.c_str()));

    unsigned long long size = 0;
    const std::string digest = SLUpdateVerify::sha256File(argv[4], &size);
    CHECK(digest.size() == 64 && size > 0);
    CHECK(SLUpdateVerify::sameDigest(digest, argv[5]));
    CHECK(!SLUpdateVerify::sameDigest(digest, std::string(64, '0')));
    CHECK(SLUpdateVerify::sha256File("/nonexistent/file").empty());

    std::string raw;
    CHECK(SLUpdateVerify::hexDecode("00ff10", raw) && raw.size() == 3 && (unsigned char)raw[1] == 0xff);
    CHECK(!SLUpdateVerify::hexDecode("0", raw));
    CHECK(!SLUpdateVerify::hexDecode("0g", raw));
    CHECK(SLUpdateVerify::base64Decode("aGk=", raw) && raw == "hi");
    CHECK(!SLUpdateVerify::base64Decode("aGk", raw));

    std::printf(failures ? "%d FAILURES\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
