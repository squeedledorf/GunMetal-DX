/**
 * @file slavatarcomplexitytext.cpp
 * @brief Text widget showing our own avatar complexity, for the Appearance window (#627)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slavatarcomplexitytext.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slavatarcomplexitytext.h"

#include "llresmgr.h"
#include "lltrans.h"
#include "llvoavatarself.h"

static LLDefaultChildRegistry::Register<SLAvatarComplexityText> r("avatar_complexity_text");

SLAvatarComplexityText::SLAvatarComplexityText(const Params& p)
:   LLTextBox(p)
{
}

// Reading the complexity is a member load, so checking it every drawn frame is
// cheap; the text is only reformatted when the value changes.
void SLAvatarComplexityText::draw()
{
    const U32 complexity = isAgentAvatarValid() ? gAgentAvatarp->getVisualComplexity() : 0;
    if (complexity != mShownComplexity)
    {
        mShownComplexity = complexity;
        std::string weight;
        if (complexity > 0)
        {
            LLLocale locale(LLLocale::USER_LOCALE);
            LLResMgr::getInstance()->getIntegerString(weight, complexity);
        }
        else
        {
            weight = LLTrans::getString("LoadingData");
        }
        setTextArg("[WEIGHT]", weight);
    }
    LLTextBox::draw();
}
