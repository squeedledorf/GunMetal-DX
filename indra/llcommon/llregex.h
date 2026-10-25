/**
 * @file llregex.h
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2021, Linden Research, Inc.
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

#ifndef LLREGEX_H
#define LLREGEX_H
#include <boost/regex.hpp>
#include <cstring>
#include <string>

namespace ll_regex_detail
{
    // Never log the full input of a failed regex: it may be huge (the
    // failure is usually a complexity limit) and may carry private text.
    constexpr size_t LOG_PREFIX_LENGTH = 64;

    inline std::string describe_input(const char* str, size_t len)
    {
        std::string desc = "input length " + std::to_string(len) + ", starting '";
        desc.append(str, len < LOG_PREFIX_LENGTH ? len : LOG_PREFIX_LENGTH);
        desc += (len > LOG_PREFIX_LENGTH) ? "...'" : "'";
        return desc;
    }

    inline std::string describe_input(const std::string& str)
    {
        return describe_input(str.data(), str.size());
    }

    inline std::string describe_input(const char* str)
    {
        return str ? describe_input(str, strlen(str)) : std::string("null input");
    }

    template <typename S>
    std::string describe_input(const S&)
    {
        return "non-narrow input";
    }
}

template <typename S, typename M, typename R>
LL_COMMON_API bool ll_regex_match(const S& string, M& match, const R& regex)
{
    try
    {
        return boost::regex_match(string, match, regex);
    }
    catch (const std::runtime_error& e)
    {
        LL_WARNS() << "error matching with '" << regex.str() << "': "
            << e.what() << " (" << ll_regex_detail::describe_input(string) << ")" << LL_ENDL;
        return false;
    }
}

template <typename S, typename R>
LL_COMMON_API bool ll_regex_match(const S& string, const R& regex)
{
    try
    {
        return boost::regex_match(string, regex);
    }
    catch (const std::runtime_error& e)
    {
        LL_WARNS() << "error matching with '" << regex.str() << "': "
            << e.what() << " (" << ll_regex_detail::describe_input(string) << ")" << LL_ENDL;
        return false;
    }
}

template <typename S, typename M, typename R>
bool ll_regex_search(const S& string, M& match, const R& regex)
{
    try
    {
        return boost::regex_search(string, match, regex);
    }
    catch (const std::runtime_error& e)
    {
        LL_WARNS() << "error searching with '" << regex.str() << "': "
            << e.what() << " (" << ll_regex_detail::describe_input(string) << ")" << LL_ENDL;
        return false;
    }
}

template <typename S, typename R>
bool ll_regex_search(const S& string, const R& regex)
{
    try
    {
        return boost::regex_search(string, regex);
    }
    catch (const std::runtime_error& e)
    {
        LL_WARNS() << "error searching with '" << regex.str() << "': "
            << e.what() << " (" << ll_regex_detail::describe_input(string) << ")" << LL_ENDL;
        return false;
    }
}
#endif  // LLREGEX_H
