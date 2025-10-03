#pragma once
#include <stdint.h>

namespace RF {

// Initialize SYN480R receiver and load saved codes
bool begin();

// Poll and process RF frames (call this often in loop)
void service();

// Always true in rc-switch mode (passive receiver can't be probed)
bool isPresent();

// Learn the current remote button and bind it to a relay [0..5]
bool learn(int relayIndex);

// Clear all saved remote signatures (all slots 0..5)
bool clearAll();

} // namespace RF
