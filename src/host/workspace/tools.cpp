// MCP tools for browsing the workspace. Approval is deliberately absent:
// only the human approves, in the UI.
#include "workspace/tools.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace workspace {

void register_tools(mcp::Registry &registry, Workspace &ws) {
  using nlohmann::json;
  registry.add({"workspace_list",
                "List workspace items (pipelines, systems, channels, conformance shapes, Meshy items, stamped fixed assets) "
                "with their approval state. Optionally filter by kind.",
                mcp::schema({{"kind", {{"type", "string"}, {"description", "pipeline, system, channel, conformance, meshy, or fixed"}}}}),
                [&ws](const json &args) {
                  ws.scan();
                  std::string kind = args.value("kind", "");
                  json out = json::array();
                  for (const auto &i : ws.items()) {
                    if (!kind.empty() && i.kind != kind) continue;
                    json e = {{"id", i.id}, {"state", i.state()}};
                    if (i.data.contains("doc")) e["doc"] = i.data["doc"];
                    if (i.data.contains("notes")) e["notes"] = i.data["notes"].size();
                    out.push_back(e);
                  }
                  return mcp::Result::json({{"selected", ws.selected()}, {"items", out}});
                }});
  registry.add({"workspace_get", "Get one workspace item's manifest (state, notes, details) and its path.",
                mcp::schema({{"id", {{"type", "string"}, {"description", "e.g. fixed:garden_gnome"}}}}, {"id"}),
                [&ws](const json &args) {
                  ws.scan();
                  Item item;
                  if (!ws.find(args.value("id", ""), item)) return mcp::Result::error("no item " + args.value("id", ""));
                  return mcp::Result::json({{"id", item.id}, {"kind", item.kind}, {"path", item.path},
                                            {"manifest", item.manifest}, {"data", item.data}});
                }});
  registry.add({"workspace_select", "Select an item in the workspace UI, so the human sees it in the viewport.",
                mcp::schema({{"id", {{"type", "string"}}}}, {"id"}),
                [&ws](const json &args) {
                  ws.scan();
                  Item item;
                  std::string id = args.value("id", "");
                  if (id != "preview" && !ws.find(id, item)) return mcp::Result::error("no item " + id);
                  ws.select(id);
                  return mcp::Result::text("selected " + id);
                }});
  registry.add({"approval_wait",
                "Block until the human approves or rejects an item (its state leaves draft), or until "
                "timeout_seconds (default 600, max 1800). Returns the state and the notes added while waiting, so a "
                "rejection reason reaches you. Use it instead of asking the human to tell you they approved.",
                mcp::schema({{"id", {{"type", "string"}}}, {"timeout_seconds", {{"type", "integer"}}}}, {"id"}),
                [&ws](const json &args) {
                  const std::string id = args.value("id", "");
                  const int timeout = std::clamp(args.value("timeout_seconds", 600), 1, 1800);
                  ws.scan();
                  Item item;
                  if (!ws.find(id, item)) return mcp::Result::error("no item " + id);
                  const size_t notes_before = item.data.value("notes", json::array()).size();
                  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
                  while (item.state() == "draft" && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    ws.scan();
                    if (!ws.find(id, item)) return mcp::Result::error(id + " disappeared while waiting");
                  }
                  json notes = item.data.value("notes", json::array());
                  json new_notes = json::array();
                  for (size_t i = notes_before; i < notes.size(); ++i) new_notes.push_back(notes[i]);
                  json out = {{"id", id}, {"state", item.state()}, {"new_notes", new_notes}};
                  if (item.state() == "draft") out["timed_out"] = true;
                  return mcp::Result::json(out);
                }});
  registry.add({"workspace_note", "Leave a note on a workspace item for the human (it appears in the inspector).",
                mcp::schema({{"id", {{"type", "string"}}}, {"text", {{"type", "string"}}}}, {"id", "text"}),
                [&ws](const json &args) {
                  std::string error;
                  if (!ws.add_note(args.value("id", ""), "agent", args.value("text", ""), error))
                    return mcp::Result::error(error);
                  return mcp::Result::text("noted");
                }});
}

}  // namespace workspace
