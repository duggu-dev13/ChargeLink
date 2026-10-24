#pragma once

#include <functional>
#include <string>

namespace chargelink::csms {

    // Callbacks the network layer fires. Any of them may be empty. They may throw: the caller
    // catches and logs. They run on a connection's thread, so keep them short.
    struct ConnectionHooks {
        // A connection was registered. A reconnecting charger must send BootNotification again.
        std::function<void(const std::string&)> on_connect;
        // The REGISTERED connection of this charger ended (not an older, replaced one).
        std::function<void(const std::string&)> on_disconnect;
        // The server has stopped; every connection is gone.
        std::function<void()> on_shutdown;
    };

}  // namespace chargelink::csms