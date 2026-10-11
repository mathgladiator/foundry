#pragma once
#include "mcp/handler.h"
#include "workspace/workspace.h"
namespace workspace {
void register_tools(mcp::Registry &registry, Workspace &ws);
}
