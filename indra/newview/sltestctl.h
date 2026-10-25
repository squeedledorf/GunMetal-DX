/**
 * @file sltestctl.h
 * @brief Developer test-control channel (compile-time opt-in, never in releases).
 *
 * Built only with -DSLIPSTREAM_TESTCTL=ON (make TESTCTL=1), which defines SL_TESTCTL=1
 * for this file and llappviewer.cpp. A release build (non-zero VIEWER_BUILD_ID) refuses
 * to configure with it on.
 *
 * Listens on 127.0.0.1 only (port SLIPSTREAM_TESTCTL_PORT, default 47800; SLIPSTREAM_TESTCTL=0
 * disables it at run time). Remote use goes through SSH. Every connection must first send the
 * token from <user settings dir>/testctl.token (created 0600 on first start).
 *
 * Protocol: one JSON object per line each way. The first line is {"token":"..."}; then
 * {"cmd":"...", ...} requests, answered by {"ok":true|false, ...}. Commands run on the main
 * thread; see scripts/testctl/slctl.py for the client and the command list.
 */

#ifndef SL_TESTCTL_H
#define SL_TESTCTL_H

namespace SLTestCtl
{
    void start();   // LLAppViewer::init, after the mainloop work queue exists
    void stop();    // LLAppViewer::cleanup, main thread, before the event pumps reset
}

#endif
