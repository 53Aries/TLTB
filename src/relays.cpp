#include "relays.hpp"

// Shared relay state storage definition (one definition for the whole program)
bool g_relay_on[R_COUNT] = {false, false, false, false, false, false, false};
