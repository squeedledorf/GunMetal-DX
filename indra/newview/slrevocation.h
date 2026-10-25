/**
 * @file slrevocation.h
 * @brief TLS certificate revocation for the viewer's own HTTPS stack: Chrome CRLSets plus a
 *        shipped static distrust list, applied to every verified chain.
 *
 * Neither libcurl+OpenSSL (with the shipped ca-bundle.crt) nor the embedded browser checks
 * revocation on their own, so a compromised and revoked CA intermediate stays trusted
 * (the October 2026 eMudhra intermediates). This module holds the revocation data:
 *
 *  - a Chrome CRLSet (component hfnkpimlhhgieaddgfemjhofmfblmnib): blocked SPKIs, per-issuer
 *    revoked serials and subject-limited SPKIs, parsed exactly as Chromium does;
 *  - a static distrust list (app_settings/revocation/distrusted_spki.txt): data, not code.
 *
 * and the chain check LLBasicCertificateStore runs after OpenSSL has built and verified a
 * path. It also unpacks and verifies the CRX3 the CRLSet is delivered in, and reads the
 * Omaha update-check reply that points at it.
 *
 * Depends only on the standard library, OpenSSL, zlib and Boost.JSON, so it is built and
 * tested outside the viewer too (scripts/revocation/test_revocation.sh). The viewer side
 * (loading, periodic fetch, handing the file to the browser plugin) is
 * slrevocationupdater.cpp.
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

#ifndef SL_SLREVOCATION_H
#define SL_SLREVOCATION_H

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

typedef struct x509_st X509;
struct stack_st_X509;

namespace SLRevocation
{
    // The CRLSet component: its id is derived from the publisher key (see crxIdFromPublicKey).
    constexpr const char* CRLSET_APP_ID = "hfnkpimlhhgieaddgfemjhofmfblmnib";

    enum class Status { Good, Unknown, Revoked };

    // A parsed Chrome CRLSet. Format and semantics follow Chromium net/cert/crl_set.cc
    // (CRLSet::Parse, CheckSPKI, CheckSubject, CheckSerial); hashes are raw 32-byte SHA-256.
    class CRLSet
    {
    public:
        // nullptr (and *error set) when `data` is not a valid version-0 CRLSet.
        static std::shared_ptr<const CRLSet> parse(std::string_view data, std::string* error = nullptr);

        uint32_t sequence() const { return mSequence; }
        uint64_t notAfter() const { return mNotAfter; }          // Unix seconds, 0 = none
        bool isExpired(std::time_t now) const { return mNotAfter && uint64_t(now) > mNotAfter; }

        size_t blockedSPKICount() const { return mBlockedSPKIs.size(); }
        size_t parentCount() const { return mCRLs.size(); }
        size_t serialCount() const { return mSerialCount; }
        size_t limitedSubjectCount() const { return mLimitedSubjects.size(); }

        Status checkSPKI(std::string_view spki_hash) const;
        Status checkSubject(std::string_view subject_der, std::string_view spki_hash) const;
        Status checkSerial(std::string_view serial, std::string_view issuer_spki_hash) const;

    private:
        uint32_t mSequence = 0;
        uint64_t mNotAfter = 0;
        size_t mSerialCount = 0;
        std::vector<std::string> mBlockedSPKIs;                                   // sorted
        std::unordered_map<std::string, std::vector<std::string>> mCRLs;          // issuer SPKI -> sorted serials
        std::unordered_map<std::string, std::vector<std::string>> mLimitedSubjects; // subject hash -> SPKIs
    };

    // The shipped static distrust list. Text, one entry per line, '#' starts a comment:
    //   <64 hex: SHA-256 of a DER SubjectPublicKeyInfo>          blocks that key anywhere in a chain
    //   serial <64 hex: issuer SPKI SHA-256> <hex serial>        blocks one certificate
    class DistrustList
    {
    public:
        // nullptr (and *error set, naming the line) on any malformed line.
        static std::shared_ptr<const DistrustList> parse(std::string_view text, std::string* error = nullptr);

        size_t spkiCount() const { return mSPKIs.size(); }
        size_t serialCount() const { return mSerials.size(); }
        bool blocksSPKI(std::string_view spki_hash) const;
        bool blocksSerial(std::string_view serial, std::string_view issuer_spki_hash) const;

    private:
        std::vector<std::string> mSPKIs;     // sorted
        std::vector<std::string> mSerials;   // sorted issuer_hash + normalized serial
    };

    struct Verdict
    {
        bool revoked = false;
        int depth = -1;          // index into the chain (0 = leaf) of the rejected certificate
        std::string subject;     // its subject, one line
        std::string reason;      // e.g. "SPKI blocked by CRLSet 10849"
    };

    // `chain` is a verified path as X509_STORE_CTX_get0_chain returns it: leaf first, trust
    // anchor last. Walks it root -> leaf like Chromium's CheckChainRevocationUsingCRLSet
    // (net/cert/internal/revocation_checker.cc): per certificate the SPKI, then the subject
    // limit, then the serial under the issuer's SPKI (not for the top of the path, whose
    // issuer is not in it). The distrust list is applied the same way. Revocations apply even
    // when the CRLSet is past NotAfter, as in Chromium. Either source may be null.
    Verdict checkChain(stack_st_X509* chain, const CRLSet* crlset, const DistrustList* distrust);

    // Raw SHA-256 of the certificate's DER SubjectPublicKeyInfo; empty on error.
    std::string spkiHash(X509* cert);

    // The certificate's serial as DER INTEGER content octets (with the sign octet), the form
    // CRLSet::checkSerial and DistrustList::blocksSerial take; empty on error.
    std::string certSerial(X509* cert);

    // The CRX id of a publisher key: first 16 bytes of SHA-256(SPKI DER), each nibble as 'a'+n.
    std::string crxIdFromPublicKey(std::string_view spki_der);

    // Unpacks a CRX3 package: checks the "Cr24" v3 header, requires an RSA-SHA256 or
    // ECDSA-SHA256 proof by the key whose CRX id is `expected_crx_id` (and that the signed
    // header names that id), verifies it over the signed data and the ZIP archive, then
    // extracts the ZIP member `member`. False with *error on any failure.
    bool extractFromCRX3(std::string_view crx, std::string_view expected_crx_id,
                         std::string_view member, std::string& out, std::string* error = nullptr);

    // The <updatecheck> of `appid` in an Omaha (update2/crx) reply.
    struct OmahaUpdate
    {
        std::string status;      // "ok" (codebase is set) or e.g. "noupdate"
        std::string codebase;    // CRX URL
        std::string version;     // CRLSet sequence as a string
        std::string sha256;      // hex SHA-256 of the CRX, if given
    };
    bool parseOmahaReply(std::string_view xml, std::string_view appid, OmahaUpdate& out);

    // The Omaha request for the CRLSet. `current_sequence` 0 asks for the latest; otherwise
    // Omaha answers "noupdate" while that is still current. `full` selects the complete
    // desktop CRLSet rather than the small default cohort.
    std::string omahaRequestURL(uint32_t current_sequence, bool full = true);

    // ---- process-wide state (thread-safe; read on the HTTP thread, written on the main thread) ----

    struct Snapshot
    {
        std::shared_ptr<const CRLSet> crlset;
        std::shared_ptr<const DistrustList> distrust;
        uint32_t generation = 0;     // bumps on every install
    };
    Snapshot snapshot();
    uint32_t generation();
    void installCRLSet(std::shared_ptr<const CRLSet> crlset);
    void installDistrustList(std::shared_ptr<const DistrustList> list);

    // Lower-case hex of raw bytes (for logs).
    std::string toHex(std::string_view bytes);
}

#endif // SL_SLREVOCATION_H
