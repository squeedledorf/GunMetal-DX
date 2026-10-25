/**
 * @file llexpat.h
 * @brief expat include + parser hardening shared by every XML parser in the viewer
 *        (LLSD-XML, XUI, LLXMLNode, LLXmlParser, the Vivox protocol parser) (#395).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLEXPAT_H
#define LL_LLEXPAT_H

#include <type_traits>

#ifndef XML_STATIC
#define XML_STATIC
#endif
extern "C"
{
#ifdef LL_USESYSTEMLIBS
# include <expat.h>
#else
# include "expat/expat.h"
#endif
}

namespace LLExpat
{
    template <class Owner, class P>
    Owner* owner_of(P Owner::*);

    // Refuse documents that carry a document type declaration.
    //
    // Nothing the viewer parses -- LLSD-XML (capabilities, settings, caches), XUI and
    // skin files, avatar/character definitions, the Vivox protocol -- uses a DOCTYPE,
    // and the DTD's internal subset is where XML entities are declared. Stopping the
    // parse as soon as a DOCTYPE starts removes entity expansion ("billion laughs")
    // and external entities (XXE) at the source, instead of relying on expat's
    // amplification limits. The parse then fails with XML_ERROR_ABORTED, which every
    // caller already treats as a parse error.
    //
    // Member is the owning object's parser data member (XML_Parser, or XML_Parser*
    // for LLXMLNode): the parser's user data must point at that owner, as
    // XML_SetUserData(parser, owner) arranges, at least until the root element
    // starts (a DOCTYPE can only come before it). Like every handler, this one is
    // cleared by XML_ParserReset(): install it again after a reset.
    //
    //   LLExpat::rejectDoctype<&MyParser::mParser>(mParser);
    template <auto Member>
    void rejectDoctype(XML_Parser parser)
    {
        using Owner = std::remove_pointer_t<decltype(owner_of(Member))>;
        XML_SetStartDoctypeDeclHandler(parser,
            [](void* user_data, const XML_Char*, const XML_Char*, const XML_Char*, int)
            {
                Owner* owner = static_cast<Owner*>(user_data);
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(owner->*Member)>, XML_Parser>)
                {
                    XML_StopParser(owner->*Member, XML_FALSE);
                }
                else
                {
                    XML_StopParser(*(owner->*Member), XML_FALSE);
                }
            });
    }
}

#endif // LL_LLEXPAT_H
