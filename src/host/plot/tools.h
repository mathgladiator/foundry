#pragma once

#include "assets/view.h"
#include "mcp/handler.h"
#include "plot/view.h"

namespace plot {

void register_tools(mcp::Registry &registry, View &view);

// The view's latest picture as viewport content (waits up to timeout_ms for the current state).
bool content(View &view, assets::ViewContent &out, int timeout_ms);

// The overlays a region's picture is drawn with: its own lines, no hero, hex, or conformance.
viewport::Overlays overlays(bool collision);

}  // namespace plot
