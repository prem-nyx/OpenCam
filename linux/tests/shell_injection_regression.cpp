#include "rtsp_probe.hpp"

#include <cassert>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main()
{
    char markerTemplate[] = "/tmp/opencam-shell-regression-XXXXXX";
    const int markerFd = mkstemp(markerTemplate);
    assert(markerFd >= 0);
    close(markerFd);
    assert(unlink(markerTemplate) == 0);

    // This is inert when passed as one argv item. Under the old std::system
    // construction, the command substitution would create this marker.
    const std::string url = std::string("rtsp://127.0.0.1:9/live$(touch ") +
                            markerTemplate + ")";
    assert(!waitForRtsp(url));
    const bool markerAbsent = access(markerTemplate, F_OK) != 0;
    unlink(markerTemplate);
    assert(markerAbsent);
    return 0;
}
