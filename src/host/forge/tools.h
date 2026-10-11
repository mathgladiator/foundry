#pragma once
#include "forge/forge.h"
#include "mcp/handler.h"
#include "workspace/workspace.h"
namespace forge {
void register_tools(mcp::Registry &registry, const Paths &paths, wasm::Runner &runner, workspace::Workspace &ws);
}
