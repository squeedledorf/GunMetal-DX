/**
 * @file slrevocation.cpp
 * @brief Chrome CRLSet / static distrust list parsing and the chain revocation check.
 *        See slrevocation.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "slrevocation.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include <boost/json.hpp>

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

// The viewer links the zlib-ng package (zlib-compatible API); the standalone test
// (scripts/revocation/test_revocation.sh) uses the system zlib.
#if defined(LL_USESYSTEMLIBS) || defined(SL_REVOCATION_STANDALONE)
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

namespace
{
    constexpr size_t HASH_LEN = 32;

    struct PkeyDeleter  { void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); } };
    struct MdCtxDeleter { void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); } };

    void set_error(std::string* error, const std::string& text)
    {
        if (error) *error = text;
    }

    std::string sha256(std::string_view data)
    {
        unsigned char md[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
        return std::string(reinterpret_cast<const char*>(md), sizeof(md));
    }

    uint32_t le32(const unsigned char* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    uint16_t le16(const unsigned char* p)
    {
        return uint16_t(p[0] | (p[1] << 8));
    }

    int hex_val(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex_decode(std::string_view hex, std::string& out)
    {
        out.clear();
        if (hex.empty() || hex.size() % 2) return false;
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = hex_val(hex[i]), lo = hex_val(hex[i + 1]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(char((hi << 4) | lo));
        }
        return true;
    }

    // Standard base64 with optional '=' padding (what Chromium's base::Base64Decode accepts
    // for these hashes). No whitespace.
    bool base64_decode(std::string_view in, std::string& out)
    {
        out.clear();
        uint32_t acc = 0;
        int bits = 0;
        size_t pad = 0;
        for (char c : in)
        {
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+') v = 62;
            else if (c == '/') v = 63;
            else if (c == '=') { ++pad; continue; }
            else return false;
            if (pad) return false;               // data after padding
            acc = (acc << 6) | uint32_t(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(char((acc >> bits) & 0xff));
            }
        }
        return pad <= 2 && (in.size() % 4 == 0 || pad == 0);
    }

    // Chromium CheckSerial: a negative serial is UNKNOWN; leading zero bytes are removed
    // (keeping at least one). Returns false for "negative".
    bool normalize_serial(std::string_view in, std::string& out)
    {
        if (!in.empty() && (static_cast<unsigned char>(in[0]) & 0x80) != 0) return false;
        while (in.size() > 1 && in[0] == '\0') in.remove_prefix(1);
        out.assign(in.data(), in.size());
        return true;
    }

    // ---- CRLSet header ----------------------------------------------------------------

    // Chromium CopyHashListFromHeader: optional; a present list must hold base64 strings.
    // (A JSON null counts as absent, as base::DictValue::FindList returns nullptr for it.)
    bool copy_hash_list(const boost::json::object& header, const char* key, std::vector<std::string>& out)
    {
        const boost::json::value* v = header.if_contains(key);
        if (!v || v->is_null()) return true;
        if (!v->is_array()) return true;   // FindList: not a list = not found
        for (const auto& item : v->as_array())
        {
            if (!item.is_string()) return false;
            std::string decoded;
            if (!base64_decode(std::string_view(item.as_string().data(), item.as_string().size()), decoded)) return false;
            out.push_back(std::move(decoded));
        }
        return true;
    }

    // Chromium CopyHashToHashesMapFromHeader.
    bool copy_hash_map(const boost::json::object& header, const char* key,
                       std::unordered_map<std::string, std::vector<std::string>>& out)
    {
        const boost::json::value* v = header.if_contains(key);
        if (!v || !v->is_object()) return true;
        for (const auto& kv : v->as_object())
        {
            if (!kv.value().is_array()) return false;
            std::vector<std::string> allowed;
            for (const auto& j : kv.value().as_array())
            {
                std::string decoded;
                if (!j.is_string() ||
                    !base64_decode(std::string_view(j.as_string().data(), j.as_string().size()), decoded))
                {
                    return false;
                }
                allowed.push_back(std::move(decoded));
            }
            std::string subject_hash;
            if (!base64_decode(std::string_view(kv.key().data(), kv.key().size()), subject_hash)) return false;
            out[subject_hash] = std::move(allowed);
        }
        return true;
    }

    // ---- minimal protobuf walker (CRX3 header) ----------------------------------------

    bool read_varint(std::string_view& in, uint64_t& out)
    {
        out = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if (in.empty()) return false;
            const unsigned char b = static_cast<unsigned char>(in[0]);
            in.remove_prefix(1);
            out |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }

    // Calls fn(field_number, bytes) for every length-delimited field; skips varint and
    // fixed fields. False on malformed input.
    template <typename Fn>
    bool walk_proto(std::string_view in, Fn fn)
    {
        while (!in.empty())
        {
            uint64_t key;
            if (!read_varint(in, key)) return false;
            const uint64_t field = key >> 3;
            switch (key & 7)
            {
            case 0: { uint64_t v; if (!read_varint(in, v)) return false; break; }
            case 1: if (in.size() < 8) return false; in.remove_prefix(8); break;
            case 5: if (in.size() < 4) return false; in.remove_prefix(4); break;
            case 2:
            {
                uint64_t len;
                if (!read_varint(in, len) || len > in.size()) return false;
                fn(field, in.substr(0, size_t(len)));
                in.remove_prefix(size_t(len));
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    // ---- minimal ZIP reader -----------------------------------------------------------

    constexpr size_t MAX_MEMBER_BYTES = 64u * 1024 * 1024;

    bool zip_extract(std::string_view zip, std::string_view member, std::string& out, std::string* error)
    {
        const unsigned char* z = reinterpret_cast<const unsigned char*>(zip.data());
        const size_t n = zip.size();
        if (n < 22) { set_error(error, "ZIP too short"); return false; }

        // End of central directory: last 22 bytes plus up to 64 KiB of comment.
        size_t eocd = std::string_view::npos;
        const size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;
        for (size_t p = n - 22 + 1; p-- > lowest;)
        {
            if (le32(z + p) == 0x06054b50) { eocd = p; break; }
        }
        if (eocd == std::string_view::npos) { set_error(error, "no ZIP end of central directory"); return false; }
        const uint16_t entries = le16(z + eocd + 10);
        const uint32_t cd_size = le32(z + eocd + 12);
        const uint32_t cd_off = le32(z + eocd + 16);
        if (size_t(cd_off) + cd_size > eocd) { set_error(error, "bad ZIP central directory"); return false; }

        size_t p = cd_off;
        for (uint16_t i = 0; i < entries; ++i)
        {
            if (p + 46 > eocd || le32(z + p) != 0x02014b50) { set_error(error, "bad ZIP central directory entry"); return false; }
            const uint16_t flags = le16(z + p + 8);
            const uint16_t method = le16(z + p + 10);
            const uint32_t crc = le32(z + p + 16);
            const uint32_t csize = le32(z + p + 20);
            const uint32_t usize = le32(z + p + 24);
            const uint16_t name_len = le16(z + p + 28);
            const uint16_t extra_len = le16(z + p + 30);
            const uint16_t comment_len = le16(z + p + 32);
            const uint32_t local_off = le32(z + p + 42);
            if (p + 46 + name_len > eocd) { set_error(error, "bad ZIP entry name"); return false; }
            const std::string_view name(zip.data() + p + 46, name_len);
            p += 46 + size_t(name_len) + extra_len + comment_len;
            if (name != member) continue;

            if (flags & 0x1) { set_error(error, "encrypted ZIP member"); return false; }
            if (usize > MAX_MEMBER_BYTES) { set_error(error, "ZIP member too large"); return false; }
            if (size_t(local_off) + 30 > n || le32(z + local_off) != 0x04034b50) { set_error(error, "bad ZIP local header"); return false; }
            const size_t data_off = size_t(local_off) + 30 + le16(z + local_off + 26) + le16(z + local_off + 28);
            if (data_off + csize > n) { set_error(error, "truncated ZIP member"); return false; }
            const std::string_view data(zip.data() + data_off, csize);

            if (method == 0)
            {
                if (csize != usize) { set_error(error, "bad stored ZIP member"); return false; }
                out.assign(data.data(), data.size());
            }
            else if (method == 8)
            {
                out.assign(usize, '\0');
                z_stream zs;
                std::memset(&zs, 0, sizeof(zs));
                if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { set_error(error, "inflateInit failed"); return false; }
                zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
                zs.avail_in = uInt(data.size());
                zs.next_out = reinterpret_cast<Bytef*>(out.data());
                zs.avail_out = uInt(out.size());
                const int rc = inflate(&zs, Z_FINISH);
                const uLong produced = zs.total_out;
                inflateEnd(&zs);
                if (rc != Z_STREAM_END || produced != usize) { set_error(error, "ZIP member does not inflate"); return false; }
            }
            else
            {
                set_error(error, "unsupported ZIP compression method");
                return false;
            }
            const uLong actual = crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(out.data()), uInt(out.size()));
            if (actual != crc) { set_error(error, "ZIP member CRC mismatch"); out.clear(); return false; }
            return true;
        }
        set_error(error, "ZIP has no member '" + std::string(member) + "'");
        return false;
    }

    std::string one_line_subject(X509* cert)
    {
        std::string out;
        if (char* s = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0))
        {
            out = s;
            OPENSSL_free(s);
        }
        return out;
    }

    std::string subject_der(X509* cert)
    {
        unsigned char* der = nullptr;
        const int len = i2d_X509_NAME(X509_get_subject_name(cert), &der);
        if (len <= 0 || !der) return std::string();
        std::string out(reinterpret_cast<const char*>(der), size_t(len));
        OPENSSL_free(der);
        return out;
    }

    std::mutex sStateMutex;
    std::shared_ptr<const SLRevocation::CRLSet> sCRLSet;
    std::shared_ptr<const SLRevocation::DistrustList> sDistrust;
    std::atomic<uint32_t> sGeneration{ 0 };
}

namespace SLRevocation
{
    std::string certSerial(X509* cert)
    {
        const ASN1_INTEGER* serial = X509_get0_serialNumber(cert);
        if (!serial) return std::string();
        // OpenSSL keeps the sign in the type and the magnitude without the DER sign octet.
        // Rebuild the DER content octets Chromium's ParsedCertificate::serial_number holds.
        const unsigned char* data = ASN1_STRING_get0_data(serial);
        const int len = ASN1_STRING_length(serial);
        std::string magnitude = (len > 0 && data) ? std::string(reinterpret_cast<const char*>(data), size_t(len))
                                                  : std::string(1, '\0');
        if (ASN1_STRING_type(serial) == V_ASN1_NEG_INTEGER)
        {
            // Any negative value: a content string with the sign bit set is enough for
            // CheckSerial, which only needs to see that it is negative.
            return std::string(1, char(0xff)) + magnitude;
        }
        if (static_cast<unsigned char>(magnitude[0]) & 0x80) magnitude.insert(magnitude.begin(), '\0');
        return magnitude;
    }

    // ---- CRLSet ---------------------------------------------------------------------------

    // Format (Chromium net/cert/crl_set.cc, "CRLSet format" comment and CRLSet::Parse):
    //   uint16le header_len
    //   byte[header_len] header_bytes      JSON: Version (0), ContentType ("CRLSet"),
    //                                      Sequence (int), NotAfter (optional, >= 0),
    //                                      BlockedSPKIs, LimitedSubjects,
    //                                      KnownInterceptionSPKIs, BlockedInterceptionSPKIs
    //   repeated {
    //     byte[32] parent_spki_sha256
    //     uint32le num_serials             (<= 32M: ReadCRL's sanity check)
    //     num_serials x { uint8 serial_length; byte[serial_length] serial }
    //   }
    // BlockedInterceptionSPKIs are blocked too (Parse appends them to blocked_spkis_).
    // Chromium also merges its compiled-in cert_verify_proc_blocklist.inc here; the viewer's
    // equivalent is the static distrust list, applied separately in checkChain().
    std::shared_ptr<const CRLSet> CRLSet::parse(std::string_view data, std::string* error)
    {
        const unsigned char* d = reinterpret_cast<const unsigned char*>(data.data());
        if (data.size() < 2) { set_error(error, "too short"); return nullptr; }
        const size_t header_len = le16(d);
        data.remove_prefix(2);
        if (data.size() < header_len) { set_error(error, "truncated header"); return nullptr; }
        const std::string_view header_bytes = data.substr(0, header_len);
        data.remove_prefix(header_len);

        boost::json::parse_options opts;
        opts.allow_trailing_commas = true;    // base::JSON_ALLOW_TRAILING_COMMAS
        boost::system::error_code ec;
        boost::json::value header_value = boost::json::parse(header_bytes, ec, {}, opts);
        if (ec || !header_value.is_object()) { set_error(error, "header is not a JSON object"); return nullptr; }
        const boost::json::object& header = header_value.as_object();

        const boost::json::value* content_type = header.if_contains("ContentType");
        if (!content_type || !content_type->is_string() || content_type->as_string() != "CRLSet")
        {
            set_error(error, "ContentType is not CRLSet");
            return nullptr;
        }
        const boost::json::value* version = header.if_contains("Version");
        if (!version || !version->is_int64() || version->as_int64() != 0)
        {
            set_error(error, "unsupported Version");
            return nullptr;
        }
        const boost::json::value* sequence = header.if_contains("Sequence");
        if (!sequence || !sequence->is_int64() ||
            sequence->as_int64() < INT32_MIN || sequence->as_int64() > INT32_MAX)
        {
            set_error(error, "missing Sequence");
            return nullptr;
        }
        double not_after = 0;
        if (const boost::json::value* na = header.if_contains("NotAfter"))
        {
            if (na->is_double()) not_after = na->as_double();
            else if (na->is_int64()) not_after = double(na->as_int64());
            else if (na->is_uint64()) not_after = double(na->as_uint64());
        }
        if (not_after < 0) { set_error(error, "negative NotAfter"); return nullptr; }
        // Delta CRLSets (a non-zero DeltaFrom) are no longer served and Chromium has no delta
        // support; one parsed as a full set would silently drop most revocations. Refuse it.
        if (const boost::json::value* delta = header.if_contains("DeltaFrom"))
        {
            if (!delta->is_number() || delta->to_number<double>() != 0)
            {
                set_error(error, "delta CRLSet (DeltaFrom) not supported");
                return nullptr;
            }
        }

        auto crl_set = std::make_shared<CRLSet>();
        crl_set->mSequence = static_cast<uint32_t>(sequence->as_int64());
        crl_set->mNotAfter = static_cast<uint64_t>(not_after);

        while (!data.empty())
        {
            // ReadCRL
            if (data.size() < HASH_LEN + 4) { set_error(error, "truncated CRL entry"); return nullptr; }
            std::string parent(data.substr(0, HASH_LEN));
            const uint32_t num_serials = le32(reinterpret_cast<const unsigned char*>(data.data()) + HASH_LEN);
            data.remove_prefix(HASH_LEN + 4);
            if (num_serials > 32u * 1024 * 1024) { set_error(error, "implausible serial count"); return nullptr; }
            std::vector<std::string> serials;
            serials.reserve(std::min<size_t>(num_serials, data.size()));
            for (uint32_t i = 0; i < num_serials; ++i)
            {
                if (data.empty()) { set_error(error, "truncated serial"); return nullptr; }
                const size_t len = static_cast<unsigned char>(data[0]);
                data.remove_prefix(1);
                if (data.size() < len) { set_error(error, "truncated serial"); return nullptr; }
                serials.emplace_back(data.substr(0, len));
                data.remove_prefix(len);
            }
            std::sort(serials.begin(), serials.end());
            // Chromium: crls_[parent] = serials (a repeated parent replaces the earlier one).
            std::vector<std::string>& slot = crl_set->mCRLs[std::move(parent)];
            crl_set->mSerialCount -= slot.size();
            crl_set->mSerialCount += serials.size();
            slot = std::move(serials);
        }

        std::vector<std::string> blocked_interception, known_interception;
        if (!copy_hash_list(header, "BlockedSPKIs", crl_set->mBlockedSPKIs) ||
            !copy_hash_map(header, "LimitedSubjects", crl_set->mLimitedSubjects) ||
            !copy_hash_list(header, "KnownInterceptionSPKIs", known_interception) ||
            !copy_hash_list(header, "BlockedInterceptionSPKIs", blocked_interception))
        {
            set_error(error, "malformed hash list in header");
            return nullptr;
        }
        crl_set->mBlockedSPKIs.insert(crl_set->mBlockedSPKIs.end(),
                                      blocked_interception.begin(), blocked_interception.end());
        std::sort(crl_set->mBlockedSPKIs.begin(), crl_set->mBlockedSPKIs.end());
        crl_set->mBlockedSPKIs.erase(std::unique(crl_set->mBlockedSPKIs.begin(), crl_set->mBlockedSPKIs.end()),
                                     crl_set->mBlockedSPKIs.end());
        return crl_set;
    }

    Status CRLSet::checkSPKI(std::string_view spki_hash) const
    {
        return std::binary_search(mBlockedSPKIs.begin(), mBlockedSPKIs.end(), spki_hash)
            ? Status::Revoked : Status::Good;
    }

    Status CRLSet::checkSubject(std::string_view subject_der, std::string_view spki_hash) const
    {
        if (mLimitedSubjects.empty()) return Status::Good;
        const auto it = mLimitedSubjects.find(sha256(subject_der));
        if (it == mLimitedSubjects.end()) return Status::Good;
        for (const auto& allowed : it->second)
        {
            if (allowed == spki_hash) return Status::Good;
        }
        return Status::Revoked;
    }

    Status CRLSet::checkSerial(std::string_view serial, std::string_view issuer_spki_hash) const
    {
        std::string normalized;
        if (!normalize_serial(serial, normalized)) return Status::Unknown;
        const auto it = mCRLs.find(std::string(issuer_spki_hash));
        if (it == mCRLs.end()) return Status::Unknown;
        return std::binary_search(it->second.begin(), it->second.end(), normalized)
            ? Status::Revoked : Status::Good;
    }

    // ---- static distrust list -------------------------------------------------------------

    std::shared_ptr<const DistrustList> DistrustList::parse(std::string_view text, std::string* error)
    {
        auto list = std::make_shared<DistrustList>();
        size_t line_no = 0;
        while (!text.empty())
        {
            ++line_no;
            size_t eol = text.find('\n');
            std::string_view line = text.substr(0, eol);
            text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
            if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);

            std::vector<std::string_view> words;
            size_t p = 0;
            while (p < line.size())
            {
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
                size_t q = p;
                while (q < line.size() && line[q] != ' ' && line[q] != '\t' && line[q] != '\r') ++q;
                if (q > p) words.push_back(line.substr(p, q - p));
                p = q;
            }
            if (words.empty()) continue;

            std::string spki, serial;
            if (words.size() == 1 && words[0].size() == HASH_LEN * 2 && hex_decode(words[0], spki))
            {
                list->mSPKIs.push_back(spki);
                continue;
            }
            if (words.size() == 3 && words[0] == "serial" && words[1].size() == HASH_LEN * 2 &&
                hex_decode(words[1], spki) &&
                hex_decode(words[2].size() % 2 ? "0" + std::string(words[2]) : std::string(words[2]), serial))
            {
                // The serial as `openssl x509 -serial` prints it: the positive magnitude. Stored
                // in CheckSerial's normalized form (leading zero octets removed).
                std::string_view magnitude(serial);
                while (magnitude.size() > 1 && magnitude[0] == '\0') magnitude.remove_prefix(1);
                list->mSerials.push_back(spki + std::string(magnitude));
                continue;
            }
            set_error(error, "line " + std::to_string(line_no) + " is not '<sha256 hex>' or 'serial <issuer sha256 hex> <serial hex>'");
            return nullptr;
        }
        std::sort(list->mSPKIs.begin(), list->mSPKIs.end());
        std::sort(list->mSerials.begin(), list->mSerials.end());
        return list;
    }

    bool DistrustList::blocksSPKI(std::string_view spki_hash) const
    {
        return std::binary_search(mSPKIs.begin(), mSPKIs.end(), spki_hash);
    }

    bool DistrustList::blocksSerial(std::string_view serial, std::string_view issuer_spki_hash) const
    {
        if (mSerials.empty()) return false;
        std::string normalized;
        if (!normalize_serial(serial, normalized)) return false;
        return std::binary_search(mSerials.begin(), mSerials.end(), std::string(issuer_spki_hash) + normalized);
    }

    // ---- chain check ----------------------------------------------------------------------

    std::string spkiHash(X509* cert)
    {
        unsigned char* der = nullptr;
        const int len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
        if (len <= 0 || !der) return std::string();
        std::string out = sha256(std::string_view(reinterpret_cast<const char*>(der), size_t(len)));
        OPENSSL_free(der);
        return out;
    }

    Verdict checkChain(stack_st_X509* chain, const CRLSet* crlset, const DistrustList* distrust)
    {
        Verdict verdict;
        if (!chain || (!crlset && !distrust)) return verdict;
        const int count = sk_X509_num(chain);
        std::string issuer_spki;
        // Root (top of the path) towards the leaf, as CheckChainRevocationUsingCRLSet does.
        for (int i = count - 1; i >= 0; --i)
        {
            X509* cert = sk_X509_value(chain, i);
            const bool is_root = (i == count - 1);
            const std::string spki = spkiHash(cert);
            if (spki.empty())
            {
                // Cannot hash the key: treat as revoked rather than skip the check silently.
                verdict.revoked = true;
                verdict.depth = i;
                verdict.subject = one_line_subject(cert);
                verdict.reason = "cannot hash the certificate's public key";
                return verdict;
            }
            // DER content octets; checkSerial / blocksSerial apply Chromium's normalization.
            const std::string serial = is_root ? std::string() : certSerial(cert);
            const bool have_serial = !serial.empty();

            std::string reason;
            if (distrust && distrust->blocksSPKI(spki))
            {
                reason = "public key is on the static distrust list";
            }
            else if (distrust && have_serial && distrust->blocksSerial(serial, issuer_spki))
            {
                reason = "serial is on the static distrust list";
            }
            else if (crlset)
            {
                if (crlset->checkSPKI(spki) == Status::Revoked)
                {
                    reason = "public key blocked by CRLSet " + std::to_string(crlset->sequence());
                }
                else if (crlset->checkSubject(subject_der(cert), spki) == Status::Revoked)
                {
                    reason = "subject/key pair not allowed by CRLSet " + std::to_string(crlset->sequence());
                }
                else if (have_serial && crlset->checkSerial(serial, issuer_spki) == Status::Revoked)
                {
                    reason = "serial revoked by CRLSet " + std::to_string(crlset->sequence());
                }
            }
            if (!reason.empty())
            {
                verdict.revoked = true;
                verdict.depth = i;
                verdict.subject = one_line_subject(cert);
                verdict.reason = reason;
                return verdict;
            }
            issuer_spki = spki;
        }
        return verdict;
    }

    // ---- CRX3 ----------------------------------------------------------------------------

    std::string crxIdFromPublicKey(std::string_view spki_der)
    {
        const std::string digest = sha256(spki_der);
        std::string id;
        for (size_t i = 0; i < 16; ++i)
        {
            const unsigned char b = static_cast<unsigned char>(digest[i]);
            id.push_back(char('a' + (b >> 4)));
            id.push_back(char('a' + (b & 0xf)));
        }
        return id;
    }

    // CRX3 (Chromium components/crx_file/crx3.proto, crx_verifier.cc):
    //   "Cr24" | uint32le version (3) | uint32le header_size | CrxFileHeader | ZIP archive
    //   CrxFileHeader { repeated AsymmetricKeyProof sha256_with_rsa = 2;
    //                   repeated AsymmetricKeyProof sha256_with_ecdsa = 3;
    //                   bytes signed_header_data = 10000; }   (SignedData { bytes crx_id = 1; })
    //   AsymmetricKeyProof { bytes public_key = 1 (SPKI DER); bytes signature = 2; }
    // Every proof signs "CRX3 SignedData\0" + uint32le(len(signed_header_data)) +
    // signed_header_data + archive (RSA PKCS#1 v1.5 / ECDSA, SHA-256). Like the component
    // updater, require a valid proof from the key whose CRX id is the expected one.
    bool extractFromCRX3(std::string_view crx, std::string_view expected_crx_id,
                         std::string_view member, std::string& out, std::string* error)
    {
        out.clear();
        const unsigned char* c = reinterpret_cast<const unsigned char*>(crx.data());
        if (crx.size() < 12 || std::memcmp(c, "Cr24", 4) != 0) { set_error(error, "not a CRX file"); return false; }
        if (le32(c + 4) != 3) { set_error(error, "not a CRX3 file"); return false; }
        const uint32_t header_size = le32(c + 8);
        if (header_size > crx.size() - 12) { set_error(error, "truncated CRX header"); return false; }
        const std::string_view header = crx.substr(12, header_size);
        const std::string_view archive = crx.substr(12 + size_t(header_size));

        struct Proof { std::string_view key, sig; };
        std::vector<Proof> proofs;
        std::string_view signed_header_data;
        bool have_signed_header = false;
        const bool ok = walk_proto(header, [&](uint64_t field, std::string_view bytes)
        {
            if (field == 2 || field == 3)
            {
                Proof proof;
                if (walk_proto(bytes, [&](uint64_t f, std::string_view b)
                    {
                        if (f == 1) proof.key = b;
                        else if (f == 2) proof.sig = b;
                    }))
                {
                    proofs.push_back(proof);
                }
            }
            else if (field == 10000)
            {
                signed_header_data = bytes;
                have_signed_header = true;
            }
        });
        if (!ok || !have_signed_header) { set_error(error, "malformed CRX3 header"); return false; }

        std::string_view crx_id_raw;
        if (!walk_proto(signed_header_data, [&](uint64_t f, std::string_view b) { if (f == 1) crx_id_raw = b; }) ||
            crx_id_raw.size() != 16)
        {
            set_error(error, "CRX3 signed header has no crx_id");
            return false;
        }
        std::string declared_id;
        for (unsigned char b : crx_id_raw)
        {
            declared_id.push_back(char('a' + (b >> 4)));
            declared_id.push_back(char('a' + (b & 0xf)));
        }
        if (declared_id != expected_crx_id)
        {
            set_error(error, "CRX is for " + declared_id + ", not " + std::string(expected_crx_id));
            return false;
        }

        std::string prefix("CRX3 SignedData", 15);
        prefix.push_back('\0');
        const uint32_t shd_len = uint32_t(signed_header_data.size());
        for (int i = 0; i < 4; ++i) prefix.push_back(char((shd_len >> (8 * i)) & 0xff));

        bool verified = false;
        for (const Proof& proof : proofs)
        {
            if (proof.key.empty() || proof.sig.empty() || crxIdFromPublicKey(proof.key) != declared_id) continue;
            const unsigned char* kp = reinterpret_cast<const unsigned char*>(proof.key.data());
            std::unique_ptr<EVP_PKEY, PkeyDeleter> pkey(d2i_PUBKEY(nullptr, &kp, long(proof.key.size())));
            std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> md(EVP_MD_CTX_new());
            if (!pkey || !md) continue;
            if (EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1 ||
                EVP_DigestVerifyUpdate(md.get(), prefix.data(), prefix.size()) != 1 ||
                EVP_DigestVerifyUpdate(md.get(), signed_header_data.data(), signed_header_data.size()) != 1 ||
                EVP_DigestVerifyUpdate(md.get(), archive.data(), archive.size()) != 1)
            {
                continue;
            }
            if (EVP_DigestVerifyFinal(md.get(), reinterpret_cast<const unsigned char*>(proof.sig.data()),
                                      proof.sig.size()) == 1)
            {
                verified = true;
                break;
            }
        }
        if (!verified)
        {
            set_error(error, "no valid CRX3 signature by the " + std::string(expected_crx_id) + " key");
            return false;
        }
        return zip_extract(archive, member, out, error);
    }

    // ---- Omaha -----------------------------------------------------------------------------

    namespace
    {
        // Value of attribute `name` inside the tag text `tag` (no entity decoding beyond &amp;).
        std::string attribute(std::string_view tag, std::string_view name)
        {
            size_t p = 0;
            while ((p = tag.find(name, p)) != std::string_view::npos)
            {
                const bool boundary = p > 0 && (tag[p - 1] == ' ' || tag[p - 1] == '\t' || tag[p - 1] == '\n' || tag[p - 1] == '\r');
                size_t q = p + name.size();
                if (boundary && q + 1 < tag.size() && tag[q] == '=' && (tag[q + 1] == '"' || tag[q + 1] == '\''))
                {
                    const char quote = tag[q + 1];
                    const size_t end = tag.find(quote, q + 2);
                    if (end == std::string_view::npos) return std::string();
                    std::string value(tag.substr(q + 2, end - q - 2));
                    for (size_t amp; (amp = value.find("&amp;")) != std::string::npos;) value.replace(amp, 5, "&");
                    return value;
                }
                p = q;
            }
            return std::string();
        }
    }

    bool parseOmahaReply(std::string_view xml, std::string_view appid, OmahaUpdate& out)
    {
        out = OmahaUpdate();
        size_t p = 0;
        while ((p = xml.find("<app", p)) != std::string_view::npos)
        {
            const size_t tag_end = xml.find('>', p);
            if (tag_end == std::string_view::npos) return false;
            const std::string_view app_tag = xml.substr(p, tag_end - p);
            p = tag_end;
            if (attribute(app_tag, "appid") != appid) continue;
            const size_t app_close = xml.find("</app>", p);
            const std::string_view body = xml.substr(p, app_close == std::string_view::npos ? std::string_view::npos : app_close - p);
            const size_t uc = body.find("<updatecheck");
            if (uc == std::string_view::npos) return false;
            const size_t uc_end = body.find('>', uc);
            if (uc_end == std::string_view::npos) return false;
            const std::string_view uc_tag = body.substr(uc, uc_end - uc);
            out.status = attribute(uc_tag, "status");
            out.codebase = attribute(uc_tag, "codebase");
            out.version = attribute(uc_tag, "version");
            out.sha256 = attribute(uc_tag, "hash_sha256");
            return !out.status.empty();
        }
        return false;
    }

    std::string omahaRequestURL(uint32_t current_sequence, bool full)
    {
        // The update2/crx GET form (as used by agl/crlset-tools): x = url-encoded
        // "id=<appid>&v=<version>&uc&acceptformat=crx3"; tag=force_full selects the desktop
        // ("Auto full") cohort instead of the small default one.
        std::string url = "https://clients2.google.com/service/update2/crx?x=id%3D";
        url += CRLSET_APP_ID;
        url += "%26v%3D";
        if (current_sequence) url += std::to_string(current_sequence);
        url += "%26uc%26acceptformat%3Dcrx3";
        if (full) url += "&tag=force_full";
        return url;
    }

    // ---- state -----------------------------------------------------------------------------

    Snapshot snapshot()
    {
        std::lock_guard<std::mutex> lock(sStateMutex);
        Snapshot s;
        s.crlset = sCRLSet;
        s.distrust = sDistrust;
        s.generation = sGeneration.load();
        return s;
    }

    uint32_t generation()
    {
        return sGeneration.load();
    }

    void installCRLSet(std::shared_ptr<const CRLSet> crlset)
    {
        std::lock_guard<std::mutex> lock(sStateMutex);
        sCRLSet = std::move(crlset);
        ++sGeneration;
    }

    void installDistrustList(std::shared_ptr<const DistrustList> list)
    {
        std::lock_guard<std::mutex> lock(sStateMutex);
        sDistrust = std::move(list);
        ++sGeneration;
    }

    std::string toHex(std::string_view bytes)
    {
        static const char* digits = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char b : bytes)
        {
            out.push_back(digits[b >> 4]);
            out.push_back(digits[b & 0xf]);
        }
        return out;
    }
}
