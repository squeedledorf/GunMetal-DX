/**
 * @file slpanelgesturesetspulldown.h
 * @brief Status bar quick swap for gesture sets (#237).
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

#ifndef SL_PANELGESTURESETSPULLDOWN_H
#define SL_PANELGESTURESETSPULLDOWN_H

#include <boost/signals2.hpp>

#include "llpanelpulldown.h"

class SLPanelGestureSetsPulldown : public LLPanelPulldown
{
public:
    SLPanelGestureSetsPulldown();
    bool postBuild() override;

private:
    void populatePanel();
    void onRowClick();
    void onManageClick();

    boost::signals2::scoped_connection mSetsConnection;
};

#endif // SL_PANELGESTURESETSPULLDOWN_H
