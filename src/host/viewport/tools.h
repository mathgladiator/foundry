#pragma once

#include "mcp/handler.h"

namespace viewport {

// look_get and look_set: the game's look every view uses (design/submit.md, "The look").
void register_tools(mcp::Registry &registry);

}  // namespace viewport
