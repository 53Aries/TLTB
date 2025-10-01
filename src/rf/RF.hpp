#pragma once
#include <stdint.h>

namespace RF {

// Initialize SYN480R receiver and load saved codes
bool begin();

// Poll and process RF frames (call this often in loop)
void service();

// True if RF activity has been seen recently (~5s)
bool isPresent();

// Learn the current remote button and bind it to a relay [0..5]
bool learn(int relayIndex);

} // namespace RF
