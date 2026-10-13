#pragma once

#include "mcp/handler.h"

namespace sky {

// observatory_get, observatory_set, and observatory_view (design/observatory.md).
void register_tools(mcp::Registry &registry);

}  // namespace sky
