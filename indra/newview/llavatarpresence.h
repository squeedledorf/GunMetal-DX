#ifndef LL_LLAVATARPRESENCE_H
#define LL_LLAVATARPRESENCE_H

#include "llcallingcard.h"
#include "llviewercontrol.h"

namespace LLAvatarPresence
{
inline bool useClassicNameColors()
{
    return gSavedSettings.getBOOL("SLClassicNameColors");
}

inline const char* colorName(LLAvatarTracker::EPresence presence)
{
    switch (presence)
    {
    case LLAvatarTracker::EPresence::ONLINE:
        return "FriendOnlineColor";
    case LLAvatarTracker::EPresence::HIDDEN:
    case LLAvatarTracker::EPresence::OFFLINE:
        return "FriendOfflineColor";
    case LLAvatarTracker::EPresence::NOT_FRIEND:
        return "LabelTextColor";
    }
    return "LabelTextColor";
}
}

#endif
