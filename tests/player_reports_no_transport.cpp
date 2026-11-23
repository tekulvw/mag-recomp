// Where no platform HTTPS transport builds (tests only): the send command has none to use.
#include "player_portal.h"

namespace mag::portal {
std::unique_ptr<Transport> make_platform_transport() { return nullptr; }
}  // namespace mag::portal
