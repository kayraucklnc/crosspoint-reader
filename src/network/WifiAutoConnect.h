#pragma once

// Headless join for background fetches (credentials must already be loaded): tries the last-connected network,
// then saved networks visible in a scan in saved (priority) order, strongest
// first among equals. Blocking, no UI.
namespace wifi_auto_connect {

enum class Result { CONNECTED, NO_SAVED_NETWORKS, FAILED };

Result connect();

}  // namespace wifi_auto_connect
