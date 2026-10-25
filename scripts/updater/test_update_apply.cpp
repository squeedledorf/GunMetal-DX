// Driver for indra/newview/slupdateapply.cpp, used by test_update_apply.sh.
// usage: test_update_apply ROOT STATE_DIR STAGED_JSON PUBKEY_HEX   -> prints "<code> <message>"
#include <boost/json/src.hpp>   // header-only Boost.JSON for this standalone build

#include "slupdateapply.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--slipstream-apply-update")
    {
        return SLUpdateApply::cliMain(argc, argv);   // what the viewer binary does at startup
    }
    if (argc == 3 && std::string(argv[1]) == "version-compare")
    {
        std::printf("%d\n", SLUpdateApply::compareBuild(argv[2], "1.2.0.3"));
        return 0;
    }
    if (argc != 5) return 2;
    std::string message;
    const int code = SLUpdateApply::applyStaged(argv[3], argv[1], argv[2], message, argv[4]);
    std::printf("%d %s\n", code, message.c_str());
    return 0;
}
