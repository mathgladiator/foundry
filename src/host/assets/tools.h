#pragma once
#include "forge/forge.h"
#include "mcp/handler.h"
#include "workspace/workspace.h"
namespace assets {
void register_tools(mcp::Registry &registry, const forge::Paths &paths, wasm::Runner &runner,
                    workspace::Workspace &ws);
}
