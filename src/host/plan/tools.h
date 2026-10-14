#pragma once

#include "assets/view.h"
#include "mcp/handler.h"
#include "plan/view.h"

namespace plan {

// planner_get, planner_set, planner_plan, and planner_view (design/planner.md, "In the foundry").
void register_tools(mcp::Registry &registry, View &view);

// The view's latest picture as viewport content (waits up to timeout_ms for the current state).
bool content(View &view, assets::ViewContent &out, int timeout_ms);

}  // namespace plan
