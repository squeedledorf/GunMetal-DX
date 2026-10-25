/**
 * @file httpoptions.h
 * @brief Public-facing declarations for the HTTPOptions class
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2012-2013, Linden Research, Inc.
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

#ifndef _LLCORE_HTTP_OPTIONS_H_
#define _LLCORE_HTTP_OPTIONS_H_


#include "httpcommon.h"
#include "_refcounted.h"


namespace LLCore
{


/// Really a struct in spirit, it provides options that
/// modify HTTP requests.
///
/// Sharing instances across requests.  It's intended that
/// these be shared across requests:  caller can create one
/// of these, set it up as needed and then reference it
/// repeatedly in HTTP operations.  But see the Threading
/// note about references.
///
/// Threading:  While this class does nothing to ensure thread
/// safety, it *is* intended to be shared between the application
/// thread and the worker thread.  This means that once an instance
/// is delivered to the library in request operations, the
/// option data must not be written until all such requests
/// complete and relinquish their references.
///
/// Allocation:  Refcounted, heap only.  Caller of the constructor
/// is given a refcount.
///
class HttpOptions : private boost::noncopyable
{
public:
    HttpOptions();

    typedef std::shared_ptr<HttpOptions> ptr_t;

    virtual ~HttpOptions();                     // Use release()

protected:

    HttpOptions(const HttpOptions &);           // Not defined
    void operator=(const HttpOptions &);        // Not defined

public:

    // Default:   false
    void                setWantHeaders(bool wanted);
    bool                getWantHeaders() const
    {
        return mWantHeaders;
    }

    // Default:  0
    void                setTrace(int long);
    int                 getTrace() const
    {
        return mTracing;
    }

    // Default:  30
    void                setTimeout(unsigned int timeout);
    unsigned int        getTimeout() const
    {
        return mTimeout;
    }

    // Default:  0
    void                setTransferTimeout(unsigned int timeout);
    unsigned int        getTransferTimeout() const
    {
        return mTransferTimeout;
    }

    /// Sets the number of retries on an LLCore::HTTPRequest before the
    /// request fails.
    // Default:  5
    void                setRetries(unsigned int retries);
    unsigned int        getRetries() const
    {
        return mRetries;
    }

    /// Sets minimal delay before request retries. In microseconds.
    /// HttpPolicy will increase delay from min to max with each retry
    // Default: 1 000 000 mcs
    void                setMinBackoff(HttpTime delay);
    HttpTime            getMinBackoff() const
    {
        return mMinRetryBackoff;
    }

    /// Sets maximum delay before request retries. In microseconds.
    /// HttpPolicy will increase delay from min to max with each retry
    // Default:  5 000 000 mcs
    void                setMaxBackoff(HttpTime delay);
    HttpTime            getMaxBackoff() const
    {
        return mMaxRetryBackoff;
    }

    // Default:  true
    void                setUseRetryAfter(bool use_retry);
    bool                getUseRetryAfter() const
    {
        return mUseRetryAfter;
    }

    /// Instructs the LLCore::HTTPRequest to follow redirects
    /// Default: false
    void                setFollowRedirects(bool follow_redirect);
    bool                getFollowRedirects() const
    {
        return mFollowRedirects;
    }

    /// Instructs the LLCore::HTTPRequest to verify that the exchanged security
    /// certificate is authentic.
    /// Default: sDefaultVerifyPeer
    void                setSSLVerifyPeer(bool verify);
    bool                getSSLVerifyPeer() const
    {
        return mVerifyPeer;
    }

    /// Instructs the LLCore::HTTPRequest to verify that the name in the
    /// security certificate matches the name of the host contacted.
    /// Only takes effect while peer verification is on (a hostname check
    /// against an unverified certificate is meaningless).
    /// Default: true
    void                setSSLVerifyHost(bool verify);
    bool                getSSLVerifyHost() const
    {
        return mVerifyHost;
    }

    /// Sets the time for DNS name caching in seconds.  Setting this value
    /// to 0 will disable name caching.  Setting this value to -1 causes the
    /// name cache to never time out.
    /// Default: -1
    void                setDNSCacheTimeout(int timeout);
    int                 getDNSCacheTimeout() const
    {
        return mDNSCacheTimeout;
    }

    /// Retrieve only the headers and status from the request. Setting this
    /// to true implies setWantHeaders(true) as well.
    /// Default: false
    void                setHeadersOnly(bool nobody);
    bool                getHeadersOnly() const
    {
        return mNoBody;
    }

    // Default:   0
    void setLastModified(time_t lastModified);
    time_t getLastModified() const
    {
        return mLastModified;
    }

    /// Sets default behavior for verifying that the name in the
    /// security certificate matches the name of the host contacted.
    /// Defaults false if not set, but should be set according to
    /// viewer's initialization options and command argunments, see
    /// NoVerifySSLCert
    static void         setDefaultSSLVerifyPeer(bool verify);
    static bool         getDefaultSSLVerifyPeer() { return sDefaultVerifyPeer; }

    /// Per-request cap on the reply body, in bytes. Only ever LOWERS the
    /// transport-wide cap (HTTP_REPLY_BODY_MAX_BYTES); 0 = use that cap.
    /// Enforced on the declared Content-Length and while reading the body
    /// (the request fails with CURLE_FILESIZE_EXCEEDED, 63).
    /// Default: 0
    void                setMaxBodyBytes(size_t bytes);
    size_t              getMaxBodyBytes() const
    {
        return mMaxBodyBytes;
    }

    /// Refuse to connect to loopback / private / link-local / CGNAT /
    /// unique-local addresses (checked on the resolved address, so it also
    /// covers host names that resolve to such addresses). Meant for fetches
    /// of untrusted, user-supplied URLs. Not applied while an HTTP or SOCKS
    /// proxy is in use (the socket then goes to the proxy, which resolves).
    /// A refused connection fails with CURLE_COULDNT_CONNECT (7).
    /// Default: false
    void                setBlockPrivateAddresses(bool block);
    bool                getBlockPrivateAddresses() const
    {
        return mBlockPrivateAddresses;
    }

protected:
    bool                mWantHeaders;
    int                 mTracing;
    unsigned int        mTimeout;
    unsigned int        mTransferTimeout;
    unsigned int        mRetries;
    HttpTime            mMinRetryBackoff;
    HttpTime            mMaxRetryBackoff;
    bool                mUseRetryAfter;
    bool                mFollowRedirects;
    bool                mVerifyPeer;
    bool                mVerifyHost;
    int                 mDNSCacheTimeout;
    bool                mNoBody;
    time_t              mLastModified;
    size_t              mMaxBodyBytes;
    bool                mBlockPrivateAddresses;

    static bool         sDefaultVerifyPeer;
}; // end class HttpOptions


}  // end namespace HttpOptions

#endif  // _LLCORE_HTTP_OPTIONS_H_
