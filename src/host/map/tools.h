#pragma once

#include "map/view.h"
#include "mcp/handler.h"

namespace map {

// map_list, map_get, map_set, map_plan, and map_view (design/map.md, "In the foundry and the runtime").
void register_tools(mcp::Registry &registry, View &view);

}  // namespace map
