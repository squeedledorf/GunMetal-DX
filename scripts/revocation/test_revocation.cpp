// Standalone driver for indra/newview/slrevocation.cpp (built by test_revocation.sh against
// OpenSSL, zlib and the Boost.JSON headers; no viewer code).
//
//   test_revocation crlset <crl-set>                      parse + print stats
//   test_revocation crx <file.crx3> [<crl-set to compare>] verify + unpack a CRLSet CRX3
//   test_revocation distrust <distrust list>               parse + print stats
//   test_revocation omaha <reply.xml>                      print the CRLSet updatecheck
//   test_revocation verify <ca-bundle> <crl-set|-> <distrust|-> <chain.pem> [host]
//       Verifies the PEM chain (leaf first) the way LLBasicCertificateStore does
//       (X509_verify_cert, ssl_server purpose, partial chains) and runs checkChain() on the
//       built path. Prints "REVOKED ...", "OK" or "UNTRUSTED ..."; exit 0 / 3 / 4.
//       Every certificate of the path is also probed individually (SPKI / serial).

#include "slrevocation.h"

#include <boost/json/src.hpp>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509_vfy.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
    bool read_file(const char* path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    std::shared_ptr<const SLRevocation::CRLSet> load_crlset(const char* path)
    {
        std::string data, error;
        if (!read_file(path, data)) { std::cerr << "cannot read " << path << "\n"; return nullptr; }
        auto set = SLRevocation::CRLSet::parse(data, &error);
        if (!set) std::cerr << "CRLSet parse failed: " << error << "\n";
        return set;
    }

    std::shared_ptr<const SLRevocation::DistrustList> load_distrust(const char* path)
    {
        std::string data, error;
        if (!read_file(path, data)) { std::cerr << "cannot read " << path << "\n"; return nullptr; }
        auto list = SLRevocation::DistrustList::parse(data, &error);
        if (!list) std::cerr << "distrust list parse failed: " << error << "\n";
        return list;
    }

    const char* status_name(SLRevocation::Status s)
    {
        switch (s)
        {
        case SLRevocation::Status::Good: return "good";
        case SLRevocation::Status::Unknown: return "unknown";
        default: return "REVOKED";
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: see source\n"; return 2; }
    const std::string mode = argv[1];

    if (mode == "crlset")
    {
        auto set = load_crlset(argv[2]);
        if (!set) return 1;
        std::cout << "sequence " << set->sequence() << " not_after " << set->notAfter()
                  << " blocked_spkis " << set->blockedSPKICount() << " parents " << set->parentCount()
                  << " serials " << set->serialCount() << " limited_subjects " << set->limitedSubjectCount() << "\n";
        return 0;
    }
    if (mode == "distrust")
    {
        auto list = load_distrust(argv[2]);
        if (!list) return 1;
        std::cout << "spkis " << list->spkiCount() << " serials " << list->serialCount() << "\n";
        return 0;
    }
    if (mode == "omaha")
    {
        std::string xml;
        if (!read_file(argv[2], xml)) return 1;
        SLRevocation::OmahaUpdate u;
        if (!SLRevocation::parseOmahaReply(xml, SLRevocation::CRLSET_APP_ID, u)) { std::cerr << "no updatecheck\n"; return 1; }
        std::cout << "status " << u.status << " version " << u.version << " sha256 " << u.sha256
                  << " codebase " << u.codebase << "\n";
        return 0;
    }
    if (mode == "crx")
    {
        std::string crx, out, error;
        if (!read_file(argv[2], crx)) return 1;
        if (!SLRevocation::extractFromCRX3(crx, SLRevocation::CRLSET_APP_ID, "crl-set", out, &error))
        {
            std::cerr << "CRX rejected: " << error << "\n";
            return 1;
        }
        auto set = SLRevocation::CRLSet::parse(out, &error);
        if (!set) { std::cerr << "CRLSet in CRX does not parse: " << error << "\n"; return 1; }
        std::cout << "CRX ok: crl-set " << out.size() << " bytes, sequence " << set->sequence() << "\n";
        if (argc > 3)
        {
            std::string expect;
            if (!read_file(argv[3], expect) || expect != out) { std::cerr << "extracted crl-set differs\n"; return 1; }
            std::cout << "matches " << argv[3] << "\n";
        }
        // Tampering must be detected: flip one archive byte near the end.
        std::string bad = crx;
        bad[bad.size() - 30] ^= 0x01;
        if (SLRevocation::extractFromCRX3(bad, SLRevocation::CRLSET_APP_ID, "crl-set", out, &error))
        {
            std::cerr << "tampered CRX accepted\n";
            return 1;
        }
        std::cout << "tampered CRX rejected: " << error << "\n";
        if (SLRevocation::extractFromCRX3(crx, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "crl-set", out, &error))
        {
            std::cerr << "CRX accepted for the wrong id\n";
            return 1;
        }
        std::cout << "wrong-id CRX rejected: " << error << "\n";
        return 0;
    }
    if (mode == "verify" && argc >= 6)
    {
        std::shared_ptr<const SLRevocation::CRLSet> set;
        std::shared_ptr<const SLRevocation::DistrustList> list;
        if (std::string(argv[3]) != "-" && !(set = load_crlset(argv[3]))) return 1;
        if (std::string(argv[4]) != "-" && !(list = load_distrust(argv[4]))) return 1;

        X509_STORE* store = X509_STORE_new();
        if (X509_STORE_load_locations(store, argv[2], nullptr) != 1) { std::cerr << "cannot load " << argv[2] << "\n"; return 1; }
        FILE* f = std::fopen(argv[5], "r");
        if (!f) return 1;
        X509* leaf = PEM_read_X509(f, nullptr, nullptr, nullptr);
        STACK_OF(X509)* untrusted = sk_X509_new_null();
        while (X509* c = PEM_read_X509(f, nullptr, nullptr, nullptr)) sk_X509_push(untrusted, c);
        std::fclose(f);
        ERR_clear_error();
        if (!leaf) { std::cerr << "no certificate in " << argv[5] << "\n"; return 1; }

        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        X509_STORE_CTX_init(ctx, store, leaf, untrusted);
        X509_STORE_CTX_set_default(ctx, "ssl_server");
        X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(ctx), X509_V_FLAG_PARTIAL_CHAIN);
        if (argc > 6) X509_VERIFY_PARAM_set1_host(X509_STORE_CTX_get0_param(ctx), argv[6], 0);
        int rc = 0;
        if (X509_verify_cert(ctx) != 1)
        {
            std::cout << "UNTRUSTED " << X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)) << "\n";
            rc = 4;
        }
        else
        {
            STACK_OF(X509)* chain = X509_STORE_CTX_get0_chain(ctx);
            for (int i = sk_X509_num(chain) - 1; i >= 0; --i)
            {
                X509* c = sk_X509_value(chain, i);
                const std::string spki = SLRevocation::spkiHash(c);
                char* subj = X509_NAME_oneline(X509_get_subject_name(c), nullptr, 0);
                std::cout << "  [" << i << "] " << subj << "\n      spki " << SLRevocation::toHex(spki);
                OPENSSL_free(subj);
                if (set)
                {
                    std::cout << " crlset-spki " << status_name(set->checkSPKI(spki));
                    if (i + 1 < sk_X509_num(chain))
                    {
                        const std::string serial = SLRevocation::certSerial(c);
                        const std::string issuer = SLRevocation::spkiHash(sk_X509_value(chain, i + 1));
                        std::cout << " crlset-serial(" << SLRevocation::toHex(serial) << ") "
                                  << status_name(set->checkSerial(serial, issuer));
                    }
                }
                if (list) std::cout << " distrust-spki " << (list->blocksSPKI(spki) ? "REVOKED" : "good");
                std::cout << "\n";
            }
            const SLRevocation::Verdict v = SLRevocation::checkChain(chain, set.get(), list.get());
            if (v.revoked)
            {
                std::cout << "REVOKED depth " << v.depth << " " << v.subject << ": " << v.reason << "\n";
                rc = 0;
            }
            else
            {
                std::cout << "OK\n";
                rc = 3;
            }
        }
        X509_STORE_CTX_free(ctx);
        sk_X509_pop_free(untrusted, X509_free);
        X509_free(leaf);
        X509_STORE_free(store);
        return rc;
    }
    std::cerr << "unknown mode\n";
    return 2;
}
