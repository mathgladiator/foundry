#pragma once
#include <SDL3/SDL.h>

#include "mcp/handler.h"
#include "pipeline/live.h"
#include "workspace/workspace.h"
namespace pipeline {
void register_tools(mcp::Registry &registry, Runner &runner, Live &live, workspace::Workspace &ws);
}
