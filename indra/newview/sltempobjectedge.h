/**
 * @file sltempobjectedge.h
 * @brief Bounded existence checks for temporary objects stopped by boundary prediction.
 *
 * Copyright (c) 2026 Slipstream contributors
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_SLTEMPOBJECTEDGE_H
#define LL_SLTEMPOBJECTEDGE_H

class LLViewerObject;
namespace SLTempObjectEdge
{
    void stopped(LLViewerObject* object);
    void tick();
    void clear();
}
#endif
