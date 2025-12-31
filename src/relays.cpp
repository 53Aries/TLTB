// File Overview: Provides the single definition of the relay state array referenced by
// the inline helpers, keeping ON/OFF tracking consistent across modules.
#include "relays.hpp"

// Shared relay state storage definition (one definition for the whole program)
bool g_relay_on[R_COUNT] = {false, false, false, false, false, false, false};
