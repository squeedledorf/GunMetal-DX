/**
 * @file slavatarcomplexitytext.h
 * @brief Text widget showing our own avatar complexity, for the Appearance window (#627)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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

#ifndef SL_AVATARCOMPLEXITYTEXT_H
#define SL_AVATARCOMPLEXITYTEXT_H

#include "lltextbox.h"

/**
 * A text box whose [WEIGHT] argument follows our avatar's complexity
 * (LLVOAvatar::getVisualComplexity()). It keeps itself up to date while drawn,
 * so a panel only has to place it in XUI:
 *
 *   <avatar_complexity_text name="avatar_complexity_label" ...>Complexity: [WEIGHT]</avatar_complexity_text>
 *
 * Firestorm shows the same total in its Appearance window; this is our own
 * implementation of it.
 */
class SLAvatarComplexityText : public LLTextBox
{
public:
    struct Params : public LLInitParam::Block<Params, LLTextBox::Params>
    {
        Params() {}
    };

    void draw() override;

protected:
    SLAvatarComplexityText(const Params& p);
    friend class LLUICtrlFactory;

private:
    U32 mShownComplexity = U32_MAX;
};

#endif // SL_AVATARCOMPLEXITYTEXT_H
