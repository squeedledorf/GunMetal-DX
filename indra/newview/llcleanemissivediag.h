/** Emissive diagnostic: bounded, requested ordinary-object name replies only. */
#ifndef LL_CLEAN_EMISSIVE_DIAG_H
#define LL_CLEAN_EMISSIVE_DIAG_H
#include "lluuid.h"
#include <string>
// True only for a pending diagnostic request. Never logs avatar/attachment names.
bool llEmissiveDiagProperties(const LLUUID& id, const std::string& name);
#endif
