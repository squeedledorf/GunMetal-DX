/** @file llhazeenv.h
 * Shared renderer/editor display of environment haze after local overrides.
 */
#ifndef LL_LLHAZEENV_H
#define LL_LLHAZEENV_H
#include "llhaze.h"
class LLSettingsSky;
struct LLHazeDisplay
{
    float startDistance = 0.f;
    float maxOpacity = 0.f;
    LLHaze::Source startSource = LLHaze::Source::Derived;
    LLHaze::Source opacitySource = LLHaze::Source::Derived;
    bool physicalActive = false;
};
LLHazeDisplay llGetHazeDisplay();
LLHazeDisplay llGetHazeDisplay(const LLSettingsSky& sky);
LLHaze::Pair llResolveHaze(const LLSettingsSky& sky, double startPreference = -1., double opacityPreference = -1.);
#endif
