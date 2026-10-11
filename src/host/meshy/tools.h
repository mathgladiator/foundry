#pragma once
#include "mcp/handler.h"
#include "meshy/meshy.h"
#include "workspace/workspace.h"
namespace meshy {
void register_tools(mcp::Registry &registry, Client &client, workspace::Workspace &ws);
}
