/**
 * @file llappcorehttp.cpp
 * @brief
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2012-2014, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llappcorehttp.h"

#include "llappviewer.h"
#include "llviewercontrol.h"
#include "llexception.h"
#include "stringize.h"

#include <openssl/x509_vfy.h>
#include <openssl/ssl.h>
#include "llsecapi.h"
#include <curl/curl.h>

#include "llcorehttputil.h"
#include "httpstats.h"

// Here is where we begin to get our connection usage under control.
// This establishes llcorehttp policy classes that, among other
// things, limit the maximum number of connections to outside
// services.  Each of the entries below maps to a policy class and
// has a limit, sometimes configurable, of how many connections can
// be open at a time.

const F64 LLAppCoreHttp::MAX_THREAD_WAIT_TIME(10.0);

//  Default and dynamic values for classes
static const struct
{
    U32                         mDefault;
    U32                         mMin;
    U32                         mMax;
    U32                         mRate;
    bool                        mRawContent;    // binary assets from the CDN, see PO_RAW_CONTENT
    std::string                 mKey;
    const char *                mUsage;
} init_data[LLAppCoreHttp::AP_COUNT] =
{
    { // AP_DEFAULT
        8,      8,      8,      0,  false,
        "",
        "other"
    },
    { // AP_TEXTURE
        8,      1,      12,     0,  true,
        "TextureFetchConcurrency",
        "texture fetch"
    },
    { // AP_MESH1
        32,     1,      128,    0,  true,
        "MeshMaxConcurrentRequests",
        "mesh fetch"
    },
    { // AP_MESH2
        8,      1,      32,     0,  true,
        "Mesh2MaxConcurrentRequests",
        "mesh2 fetch"
    },
    { // AP_LARGE_MESH
        2,      1,      8,      0,  true,
        "",
        "large mesh fetch"
    },
    { // AP_UPLOADS
        2,      1,      8,      0,  false,
        "",
        "asset upload"
    },
    { // AP_LONG_POLL
        32,     32,     32,     0,  false,
        "",
        "long poll"
    },
    { // AP_INVENTORY
        4,      1,      4,      0,  false,
        "",
        "inventory"
    },
    { // AP_MATERIALS
        2,      1,      8,      0,  false,
        "RenderMaterials",
        "material manager requests"
    },
    { // AP_AGENT
        2,      1,      32,     0,  false,
        "Agent",
        "Agent requests"
    },
    { // AP_TEXTURE_AUX (#485; the limit is derived from AP_TEXTURE's in refreshSettings)
        4,      1,      4,      0,  true,
        "",
        "texture aux fetch"
    }
};

// #485: asset CDN connection policy. Textures (AP_TEXTURE), mesh (AP_MESH2) and large
// mesh (AP_LARGE_MESH) all go to the one asset CDN host, each class with its own
// keep-alive pool. SLFetchMaxConnections caps their SUM (the per-host cap; default 18
// = 8 + 8 + 2, today's counts). TextureFetchConcurrency / Mesh2MaxConcurrentRequests
// stay the per-class knobs within the shipped TPV caps (texture <= 12, mesh2 <= 32);
// when they add up to more than the host cap, both are scaled down in proportion.
// The non-CDN texture class (bakes, map tiles) gets what is left of the texture cap:
// clamp(12 - texture, 1, 4): 12 texture-class connections, 13 at most.
static constexpr U32 TEXTURE_CONN_CAP = 12;

namespace
{
struct FetchConnPlan
{
    U32 mTexture = 8;
    U32 mMesh = 8;
    U32 mLargeMesh = 2;
    U32 mTextureAux = 4;
};

U32 class_setting(const char* key, U32 dflt, U32 lo, U32 hi)
{
    U32 v = dflt;
    if (gSavedSettings.controlExists(key))
    {
        const U32 s = gSavedSettings.getU32(key);
        if (s) v = s;   // zero asks for the default
    }
    return llclamp(v, lo, hi);
}

FetchConnPlan plan_fetch_connections()
{
    FetchConnPlan p;
    const U32 host_cap = llclamp(gSavedSettings.controlExists("SLFetchMaxConnections")
                                 ? gSavedSettings.getU32("SLFetchMaxConnections") : 18u, 3u, 46u);
    p.mLargeMesh = host_cap >= 8 ? 2 : 1;
    const U32 avail = host_cap - p.mLargeMesh;
    const U32 tex = class_setting("TextureFetchConcurrency", 8, 1, TEXTURE_CONN_CAP);
    const U32 mesh = class_setting("Mesh2MaxConcurrentRequests", 8, 1, 32);
    if (tex + mesh <= avail)
    {
        p.mTexture = tex;
        p.mMesh = mesh;
    }
    else
    {
        p.mTexture = llclamp((U32)ll_round((F32)avail * (F32)tex / (F32)(tex + mesh)), 1u, avail - 1);
        p.mMesh = llmax(avail - p.mTexture, 1u);
    }
    p.mTextureAux = llclamp(TEXTURE_CONN_CAP - llmin(p.mTexture, TEXTURE_CONN_CAP), 1u, 4u);
    return p;
}
}

static void setting_changed();
static void ssl_verification_changed();


LLAppCoreHttp::HttpClass::HttpClass()
    : mPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID),
      mConnLimit(0U)
{}


LLAppCoreHttp::LLAppCoreHttp()
    : mRequest(NULL),
      mStopHandle(LLCORE_HTTP_HANDLE_INVALID),
      mStopRequested(0.0),
      mStopped(false)
{}


LLAppCoreHttp::~LLAppCoreHttp()
{
    delete mRequest;
    mRequest = NULL;
}


void LLAppCoreHttp::init()
{
    LLCoreHttpUtil::setPropertyMethods(
        boost::bind(&LLControlGroup::getBOOL, boost::ref(gSavedSettings), _1),
        boost::bind(&LLControlGroup::declareBOOL, boost::ref(gSavedSettings), _1, _2, _3, LLControlVariable::PERSIST_NONDFT));

    LLCore::LLHttp::initialize();

    LLCore::HttpStatus status = LLCore::HttpRequest::createService();
    if (! status)
    {
        LL_ERRS("Init") << "Failed to initialize HTTP services.  Reason:  " << status.toString()
                        << LL_ENDL;
    }

    // Point to our certs or SSH/https: will fail on connect
    std::string ca_file = gDirUtilp->getCAFile();
    if ( LLFile::isfile(ca_file) )
    {
        LL_DEBUGS("Init") << "Setting CA File to " << ca_file << LL_ENDL;
        status = LLCore::HttpRequest::setStaticPolicyOption(LLCore::HttpRequest::PO_CA_FILE,
                                                            LLCore::HttpRequest::GLOBAL_POLICY_ID,
                                                            ca_file, NULL);
    }
    else
    {
        LLError::LLUserWarningMsg::showMissingFiles();
        LL_ERRS("Init") << "Missing CA File; should be at " << ca_file << LL_ENDL;
    }

    if (! status)
    {
        LL_ERRS("Init") << "Failed to set CA File for HTTP services.  Reason:  " << status.toString()
                        << LL_ENDL;
    }

    // Establish HTTP Proxy, if desired.
    status = LLCore::HttpRequest::setStaticPolicyOption(LLCore::HttpRequest::PO_LLPROXY,
                                                        LLCore::HttpRequest::GLOBAL_POLICY_ID,
                                                        1, NULL);
    if (! status)
    {
        LL_WARNS("Init") << "Failed to set HTTP proxy for HTTP services.  Reason:  " << status.toString()
                         << LL_ENDL;
    }

    // Set up SSL Verification call back.
    status = LLCore::HttpRequest::setStaticPolicyOption(LLCore::HttpRequest::PO_SSL_VERIFY_CALLBACK,
                                                        LLCore::HttpRequest::GLOBAL_POLICY_ID,
                                                        sslVerify, NULL);
    if (!status)
    {
        LL_WARNS("Init") << "Failed to set SSL Verification.  Reason:  " << status.toString() << LL_ENDL;
    }

    // Set up Default SSL Verification option.
    const std::string no_verify_ssl("NoVerifySSLCert");
    if (gSavedSettings.controlExists(no_verify_ssl))
    {
        LLPointer<LLControlVariable> cntrl_ptr = gSavedSettings.getControl(no_verify_ssl);
        if (cntrl_ptr.isNull())
        {
            LL_WARNS("Init") << "Unable to set signal on global setting '" << no_verify_ssl
                << "'" << LL_ENDL;
        }
        else
        {
            mSSLNoVerifySignal = cntrl_ptr->getCommitSignal()->connect(boost::bind(&ssl_verification_changed));
            LLCore::HttpOptions::setDefaultSSLVerifyPeer(!cntrl_ptr->getValue().asBoolean());
        }
    }

    // Tracing levels for library & libcurl (note that 2 & 3 are beyond spammy):
    // 0 - None
    // 1 - Basic start, stop simple transitions
    // 2 - libcurl CURLOPT_VERBOSE mode with brief lines
    // 3 - with partial data content
    static const std::string http_trace("QAModeHttpTrace");
    if (gSavedSettings.controlExists(http_trace))
    {
        long trace_level(0L);
        trace_level = long(gSavedSettings.getU32(http_trace));
        status = LLCore::HttpRequest::setStaticPolicyOption(LLCore::HttpRequest::PO_TRACE,
                                                            LLCore::HttpRequest::GLOBAL_POLICY_ID,
                                                            trace_level, NULL);
    }

    // Setup default policy and constrain if directed to
    mHttpClasses[AP_DEFAULT].mPolicy = LLCore::HttpRequest::DEFAULT_POLICY_ID;

    // Setup additional policies based on table and some special rules
    llassert(LL_ARRAY_SIZE(init_data) == AP_COUNT);
    for (int i(0); i < LL_ARRAY_SIZE(init_data); ++i)
    {
        const EAppPolicy app_policy(static_cast<EAppPolicy>(i));

        if (AP_DEFAULT == app_policy)
        {
            // Pre-created
            continue;
        }

        mHttpClasses[app_policy].mPolicy = LLCore::HttpRequest::createPolicyClass();
        // We have run out of available HTTP policies. Adjust HTTP_POLICY_CLASS_LIMIT in _httpinternal.h
        llassert(mHttpClasses[app_policy].mPolicy != LLCore::HttpRequest::INVALID_POLICY_ID);
        if (! mHttpClasses[app_policy].mPolicy)
        {
            // Use default policy (but don't accidentally modify default)
            LL_WARNS("Init") << "Failed to create HTTP policy class for " << init_data[i].mUsage
                             << ".  Using default policy."
                             << LL_ENDL;
            mHttpClasses[app_policy].mPolicy = mHttpClasses[AP_DEFAULT].mPolicy;
            continue;
        }
    }

    // Need a request object to handle dynamic options before setting them
    mRequest = new LLCore::HttpRequest;

    // Apply initial settings
    refreshSettings(true);

    // Kick the thread
    status = LLCore::HttpRequest::startThread();
    if (! status)
    {
        LL_ERRS("Init") << "Failed to start HTTP servicing thread.  Reason:  " << status.toString()
                        << LL_ENDL;
    }

    // Register signals for settings and state changes
    for (int i(0); i < LL_ARRAY_SIZE(init_data); ++i)
    {
        const EAppPolicy app_policy(static_cast<EAppPolicy>(i));

        if (! init_data[i].mKey.empty() && gSavedSettings.controlExists(init_data[i].mKey))
        {
            LLPointer<LLControlVariable> cntrl_ptr = gSavedSettings.getControl(init_data[i].mKey);
            if (cntrl_ptr.isNull())
            {
                LL_WARNS("Init") << "Unable to set signal on global setting '" << init_data[i].mKey
                                 << "'" << LL_ENDL;
            }
            else
            {
                mHttpClasses[app_policy].mSettingsSignal = cntrl_ptr->getCommitSignal()->connect(boost::bind(&setting_changed));
            }
        }
    }

    // #485: asset fetch connection / pacing / back-off settings are live too
    static const char* const fetch_keys[] = { "SLFetchMaxConnections", "SLFetchTextureStartsPerSec",
                                              "SLFetchMeshStartsPerSec", "SLFetchBusyBackoffMaxMs" };
    for (const char* key : fetch_keys)
    {
        if (gSavedSettings.controlExists(key))
        {
            LLPointer<LLControlVariable> cntrl_ptr = gSavedSettings.getControl(key);
            if (cntrl_ptr.notNull())
            {
                mFetchSignals.push_back(cntrl_ptr->getCommitSignal()->connect(boost::bind(&setting_changed)));
            }
        }
    }
}


void setting_changed()
{
    LLAppViewer::instance()->getAppCoreHttp().refreshSettings(false);
}

void ssl_verification_changed()
{
    LLCore::HttpOptions::setDefaultSSLVerifyPeer(!gSavedSettings.getBOOL("NoVerifySSLCert"));
}

namespace
{
    // The NoOpDeletor is used when wrapping LLAppCoreHttp in a smart pointer below for
    // passage into the LLCore::Http libararies.  When the smart pointer is destroyed,
    // no action will be taken since we do not in this case want the entire LLAppCoreHttp object
    // to be destroyed at the end of the call.
    //
    // *NOTE$: Yes! It is "Deletor"
    // http://english.stackexchange.com/questions/4733/what-s-the-rule-for-adding-er-vs-or-when-nouning-a-verb
    // "delete" derives from Latin "deletus"
    void NoOpDeletor(LLCore::HttpHandler *)
    { /*NoOp*/ }
}

void LLAppCoreHttp::requestStop()
{
    llassert_always(mRequest);

    mStopHandle = mRequest->requestStopThread(LLCore::HttpHandler::ptr_t(this, NoOpDeletor));
    if (LLCORE_HTTP_HANDLE_INVALID != mStopHandle)
    {
        mStopRequested = LLTimer::getTotalSeconds();
    }
}


void LLAppCoreHttp::cleanup()
{
    LLCore::HTTPStats::instance().dumpStats();

    if (LLCORE_HTTP_HANDLE_INVALID == mStopHandle)
    {
        // Should have been started already...
        requestStop();
    }

    if (LLCORE_HTTP_HANDLE_INVALID == mStopHandle)
    {
        LL_WARNS("Cleanup") << "Attempting to cleanup HTTP services without thread shutdown"
                            << LL_ENDL;
    }
    else
    {
        while (! mStopped && LLTimer::getTotalSeconds() < (mStopRequested + MAX_THREAD_WAIT_TIME))
        {
            mRequest->update(200000);
            ms_sleep(50);
        }
        if (! mStopped)
        {
            LL_WARNS("Cleanup") << "Attempting to cleanup HTTP services with thread shutdown incomplete"
                                << LL_ENDL;
        }
    }

    for (int i(0); i < LL_ARRAY_SIZE(mHttpClasses); ++i)
    {
        mHttpClasses[i].mSettingsSignal.disconnect();
    }
    mSSLNoVerifySignal.disconnect();
    for (boost::signals2::connection& c : mFetchSignals)
    {
        c.disconnect();
    }
    mFetchSignals.clear();

    delete mRequest;
    mRequest = NULL;

    LLCore::HttpStatus status = LLCore::HttpRequest::destroyService();
    if (! status)
    {
        LL_WARNS("Cleanup") << "Failed to shutdown HTTP services, continuing.  Reason:  "
                            << status.toString()
                            << LL_ENDL;
    }
}


void LLAppCoreHttp::refreshSettings(bool initial)
{
    LLCore::HttpStatus status;
    const FetchConnPlan plan(plan_fetch_connections());

    for (int i(0); i < LL_ARRAY_SIZE(init_data); ++i)
    {
        const EAppPolicy app_policy(static_cast<EAppPolicy>(i));

        if (initial)
        {
            // Init-time only settings, can use the static setters here

            if (init_data[i].mRawContent)
            {
                status = LLCore::HttpRequest::setStaticPolicyOption(LLCore::HttpRequest::PO_RAW_CONTENT,
                                                                    mHttpClasses[app_policy].mPolicy,
                                                                    1L,
                                                                    NULL);
                if (! status)
                {
                    LL_WARNS("Init") << "Unable to set " << init_data[i].mUsage
                                     << " raw content.  Reason:  " << status.toString()
                                     << LL_ENDL;
                }
            }

            if (init_data[i].mRate)
            {
                // Set any desired throttle
                status = LLCore::HttpRequest::setStaticPolicyOption(LLCore::HttpRequest::PO_THROTTLE_RATE,
                                                                    mHttpClasses[app_policy].mPolicy,
                                                                    init_data[i].mRate,
                                                                    NULL);
                if (! status)
                {
                    LL_WARNS("Init") << "Unable to set " << init_data[i].mUsage
                                     << " throttle rate.  Reason:  " << status.toString()
                                     << LL_ENDL;
                }
            }

        }

        // Init- or run-time settings.  Must use the queued request API.

        // Get target connection concurrency value
        U32 setting(init_data[i].mDefault);
        if (! init_data[i].mKey.empty() && gSavedSettings.controlExists(init_data[i].mKey))
        {
            U32 new_setting(gSavedSettings.getU32(init_data[i].mKey));
            if (new_setting)
            {
                // Treat zero settings as an ask for default
                setting = llclamp(new_setting, init_data[i].mMin, init_data[i].mMax);
            }
        }
        switch (app_policy)
        {
        // #485: the asset CDN classes share one per-host cap (see plan_fetch_connections)
        case AP_TEXTURE:     setting = plan.mTexture; break;
        case AP_MESH2:       setting = plan.mMesh; break;
        case AP_LARGE_MESH:  setting = plan.mLargeMesh; break;
        case AP_TEXTURE_AUX: setting = plan.mTextureAux; break;
        default: break;
        }

        if (initial || setting != mHttpClasses[app_policy].mConnLimit)
        {
            // Set it and report.  Llcorehttp manages connections itself:
            // one request per connection (HTTP/1.1, no pipelining -- libcurl
            // dropped it in 7.65 and it measured no faster against the asset
            // CDN, #391), so PO_CONNECTION_LIMIT is both the connection and
            // the in-flight request cap.  Both limits get the same value for
            // logical consistency, within the shipped caps (init_data mMax).
            LLCore::HttpHandle handle;
            handle = mRequest->setPolicyOption(LLCore::HttpRequest::PO_CONNECTION_LIMIT,
                                               mHttpClasses[app_policy].mPolicy,
                                               setting,
                                               LLCore::HttpHandler::ptr_t());
            if (LLCORE_HTTP_HANDLE_INVALID == handle)
            {
                status = mRequest->getStatus();
                LL_WARNS("Init") << "Unable to set " << init_data[i].mUsage
                                 << " concurrency.  Reason:  " << status.toString()
                                 << LL_ENDL;
            }
            else
            {
                handle = mRequest->setPolicyOption(LLCore::HttpRequest::PO_PER_HOST_CONNECTION_LIMIT,
                                                   mHttpClasses[app_policy].mPolicy,
                                                   setting,
                                                   LLCore::HttpHandler::ptr_t());
                if (LLCORE_HTTP_HANDLE_INVALID == handle)
                {
                    status = mRequest->getStatus();
                    LL_WARNS("Init") << "Unable to set " << init_data[i].mUsage
                                     << " per-host concurrency.  Reason:  " << status.toString()
                                     << LL_ENDL;
                }
                else
                {
                    LL_DEBUGS("Init") << "Changed " << init_data[i].mUsage
                                      << " concurrency.  New value:  " << setting
                                      << LL_ENDL;
                    mHttpClasses[app_policy].mConnLimit = setting;
                    if (initial && setting != init_data[i].mDefault)
                    {
                        LL_INFOS("Init") << "Application settings overriding default " << init_data[i].mUsage
                                         << " concurrency.  New value:  " << setting
                                         << LL_ENDL;
                    }
                }
            }
        }
    }

    // #485: asset fetch pacing and busy back-off (live). Start pacing is a smooth token
    // bucket per class (0 = off); back-off pauses a class's new starts on 429 / 503 /
    // timeouts, honouring Retry-After. Only the asset GET classes opt in.
    {
        const U32 tex_rate = gSavedSettings.controlExists("SLFetchTextureStartsPerSec")
                             ? gSavedSettings.getU32("SLFetchTextureStartsPerSec") : 300u;
        const U32 mesh_rate = gSavedSettings.controlExists("SLFetchMeshStartsPerSec")
                              ? gSavedSettings.getU32("SLFetchMeshStartsPerSec") : 800u;
        const U32 backoff_ms = gSavedSettings.controlExists("SLFetchBusyBackoffMaxMs")
                               ? gSavedSettings.getU32("SLFetchBusyBackoffMaxMs") : 5000u;
        const struct { EAppPolicy mClass; U32 mRate; } pacing[] =
        {
            { AP_TEXTURE, tex_rate }, { AP_TEXTURE_AUX, 0u }, { AP_MESH2, mesh_rate }, { AP_LARGE_MESH, 0u }
        };
        for (const auto& p : pacing)
        {
            const LLCore::HttpRequest::policy_t pc = mHttpClasses[p.mClass].mPolicy;
            if (pc == mHttpClasses[AP_DEFAULT].mPolicy)
            {
                continue;   // class creation failed: never pace / back off the default class
            }
            if (LLCORE_HTTP_HANDLE_INVALID == mRequest->setPolicyOption(LLCore::HttpRequest::PO_THROTTLE_RATE, pc,
                                                                        (long)llmin(p.mRate, 100000u), LLCore::HttpHandler::ptr_t())
                || LLCORE_HTTP_HANDLE_INVALID == mRequest->setPolicyOption(LLCore::HttpRequest::PO_BUSY_BACKOFF_MAX_MS, pc,
                                                                           (long)llmin(backoff_ms, 60000u), LLCore::HttpHandler::ptr_t()))
            {
                LL_WARNS("Init") << "Unable to set asset fetch pacing / back-off.  Reason:  "
                                 << mRequest->getStatus().toString() << LL_ENDL;
            }
        }
        if (initial)
        {
            LL_INFOS("Init") << "Asset fetch connections: texture " << plan.mTexture << " (+" << plan.mTextureAux
                             << " bakes/map), mesh " << plan.mMesh << ", large mesh " << plan.mLargeMesh
                             << "; starts/s texture " << tex_rate << " mesh " << mesh_rate
                             << "; busy back-off up to " << backoff_ms << " ms" << LL_ENDL;
        }
    }
}

LLCore::HttpStatus LLAppCoreHttp::sslVerify(const std::string &url,
    const LLCore::HttpHandler::ptr_t &handler, void *appdata)
{
    if (gDisconnected)
    {
        return LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_OPERATION_TIMEDOUT);
    }

    LLCore::HttpStatus result;
    try
    {
        X509_STORE_CTX *ctx = static_cast<X509_STORE_CTX *>(appdata);
        LLPointer<LLCertificateStore> store = gSecAPIHandler->getCertificateStore("");
        LLPointer<LLCertificateChain> chain = gSecAPIHandler->getCertificateChain(ctx);
        LLSD validation_params = LLSD::emptyMap();
        LLURI uri(url);

        validation_params[CERT_HOSTNAME] = uri.hostName();

        // *TODO: In the case of an exception while validating the cert, we need a way
        // to pass the offending(?) cert back out. *Rider*

        // validate() first runs OpenSSL's X509_verify_cert on the presented
        // chain with the store's certificates (shipped ca-bundle.crt plus the
        // user's CA.pem) as trust anchors, then applies LL's extra policy.
        //
        // Hostname is not checked here: LL's check only looks at the subject
        // CN (no subjectAltName support).  libcurl does a full RFC 6125 check
        // (SAN, then CN) on every connection, including redirects, because
        // CURLOPT_SSL_VERIFYHOST is 2 whenever peer verification is on.
        store->validate(VALIDATION_POLICY_SSL & (~VALIDATION_POLICY_HOSTNAME), chain, validation_params);
    }
    catch (LLCertValidationTrustException &cert_exception)
    {
        // this exception is is handled differently than the general cert
        // exceptions, as we allow the user to actually add the certificate
        // for trust.
        // therefore we pass back a different error code
        // NOTE: We're currently 'wired' to pass around CURL error codes.  This is
        // somewhat clumsy, as we may run into errors that do not map directly to curl
        // error codes.  Should be refactored with login refactoring, perhaps.
        //
        // Since curl 7.62 CURLE_SSL_CACERT and CURLE_SSL_PEER_CERTIFICATE are
        // both aliases of CURLE_PEER_FAILED_VERIFICATION, which is also what
        // libcurl itself returns for any failed verification (e.g. its own
        // hostname check).  "Untrusted issuer, offer to trust it" must stay
        // distinguishable from every other certificate failure, or an expired
        // or otherwise invalid certificate would get the 'trust this CA'
        // dialog.  CURLE_SSL_ISSUER_ERROR is only produced by libcurl for
        // CURLOPT_ISSUERCERT, which we never set, so it is ours alone.
        result = LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_SSL_ISSUER_ERROR);
        result.setMessage(cert_exception.what());
        LLSD certdata = cert_exception.getCertData();
        result.setErrorData(certdata);
        // We should probably have a more generic way of passing information
        // back to the error handlers.
    }
    catch (LLCertException &cert_exception)
    {
        result = LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_PEER_FAILED_VERIFICATION);
        result.setMessage(cert_exception.what());
        LLSD certdata = cert_exception.getCertData();
        result.setErrorData(certdata);
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION(STRINGIZE("('" << url << "')"));
        // any other odd error, we just handle as a connect error.
        result = LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_SSL_CONNECT_ERROR);
    }

    return result;
}




void LLAppCoreHttp::onCompleted(LLCore::HttpHandle, LLCore::HttpResponse *)
{
    mStopped = true;
}
