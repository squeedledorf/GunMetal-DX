/**
 * @file slupdateverify.cpp
 * @brief Ed25519 manifest verification and SHA-256 of files for the auto-updater (#421).
 *
 * Deliberately free of viewer headers (std + OpenSSL only): it is compiled by itself in
 * scripts/updater/test_update_verify.sh.
 */

#include "slupdateverify.h"

#include <openssl/evp.h>

#include <cstdio>
#include <memory>

namespace
{
    int hex_val(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    struct MdCtxDeleter { void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); } };
    struct PkeyDeleter  { void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); } };
    struct FileCloser   { void operator()(FILE* f) const { if (f) std::fclose(f); } };
}

namespace SLUpdateVerify
{
    bool hexDecode(const std::string& hex, std::string& out)
    {
        out.clear();
        if (hex.empty() || hex.size() % 2) return false;
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = hex_val(hex[i]), lo = hex_val(hex[i + 1]);
            if (hi < 0 || lo < 0) { out.clear(); return false; }
            out.push_back((char)((hi << 4) | lo));
        }
        return true;
    }

    bool base64Decode(const std::string& text, std::string& out)
    {
        out.clear();
        if (text.empty() || text.size() > 4096 || text.size() % 4) return false;
        std::string buf(text.size() / 4 * 3 + 3, '\0');
        const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&buf[0]),
                                      reinterpret_cast<const unsigned char*>(text.data()), (int)text.size());
        if (n < 0) return false;
        size_t pad = 0;
        for (size_t i = text.size(); i > 0 && text[i - 1] == '='; --i) ++pad;
        if (pad > 2 || (size_t)n < pad) return false;
        buf.resize((size_t)n - pad);
        out = std::move(buf);
        return true;
    }

    bool keyConfigured(const char* pubkey)
    {
        const std::string pubkey_hex = pubkey ? pubkey : SLIPSTREAM_UPDATE_PUBKEY_HEX;
        std::string raw;
        return pubkey_hex.size() == 64 && hexDecode(pubkey_hex, raw);
    }

    bool verifyManifest(const std::string& manifest, const std::string& signature_b64, const char* pubkey)
    {
        const std::string pubkey_hex = pubkey ? pubkey : SLIPSTREAM_UPDATE_PUBKEY_HEX;
        std::string key, sig;
        if (!keyConfigured(pubkey_hex.c_str()) || !hexDecode(pubkey_hex, key) || key.size() != 32) return false;
        if (!base64Decode(signature_b64, sig) || sig.size() != 64) return false;
        std::unique_ptr<EVP_PKEY, PkeyDeleter> pkey(EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(key.data()), key.size()));
        if (!pkey) return false;
        std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
        if (!ctx) return false;
        if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) return false;
        const std::string message = std::string(SLIPSTREAM_UPDATE_SIGN_PREFIX) + manifest;
        return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                                reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    }

    std::string sha256File(const std::string& path, unsigned long long* size_out)
    {
        if (size_out) *size_out = 0;
        std::unique_ptr<FILE, FileCloser> f(std::fopen(path.c_str(), "rb"));
        if (!f) return std::string();
        std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return std::string();
        static thread_local unsigned char buf[1 << 16];
        unsigned long long total = 0;
        for (;;)
        {
            const size_t n = std::fread(buf, 1, sizeof(buf), f.get());
            if (n > 0)
            {
                if (EVP_DigestUpdate(ctx.get(), buf, n) != 1) return std::string();
                total += n;
            }
            if (n < sizeof(buf))
            {
                if (std::ferror(f.get())) return std::string();
                break;
            }
        }
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        if (EVP_DigestFinal_ex(ctx.get(), md, &len) != 1) return std::string();
        static const char* digits = "0123456789abcdef";
        std::string hex;
        for (unsigned int i = 0; i < len; ++i)
        {
            hex.push_back(digits[md[i] >> 4]);
            hex.push_back(digits[md[i] & 15]);
        }
        if (size_out) *size_out = total;
        return hex;
    }

    bool sameDigest(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size() || a.empty()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const char x = (a[i] >= 'A' && a[i] <= 'F') ? (char)(a[i] + 32) : a[i];
            const char y = (b[i] >= 'A' && b[i] <= 'F') ? (char)(b[i] + 32) : b[i];
            diff |= (unsigned char)(x ^ y);
        }
        return diff == 0;
    }
}
