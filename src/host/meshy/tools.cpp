// MCP tools for Meshy. Every call that spends credits checks the budget.
#include "meshy/tools.h"

namespace meshy {

void register_tools(mcp::Registry &registry, Client &client, workspace::Workspace &ws) {
  using nlohmann::json;
  registry.add({"meshy_status", "Meshy account balance and this project's credit budget and ledger.",
                mcp::schema(json::object()),
                [&client](const json &) {
                  json out = client.ledger();
                  int credits = 0;
                  std::string error;
                  if (client.balance(credits, error)) out["account_balance"] = credits;
                  else out["account_balance_error"] = error;
                  return mcp::Result::json(out);
                }});
  registry.add({"meshy_text_to_image",
                "Generate concept art with Meshy (3-9 credits; nano-banana is cheapest) as meshy/<name>_concept/ "
                "(image.png and item.json with provenance), for the human to approve before any 3D. Use the house "
                "prompt blocks from design/style.md section 8.",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"prompt", {{"type", "string"}}},
                             {"ai_model", {{"type", "string"}, {"description", "nano-banana (3), nano-banana-2 (6), nano-banana-pro (9)"}}},
                             {"aspect_ratio", {{"type", "string"}}},
                             {"pose_mode", {{"type", "string"}, {"enum", {"", "a-pose", "t-pose"}}}}},
                            {"name", "prompt"}),
                [&client, &ws](const json &args) {
                  json sidecar;
                  std::string error;
                  if (!client.text_to_image(args.value("name", ""), args.value("prompt", ""), args.value("ai_model", ""),
                                            args.value("aspect_ratio", ""), args.value("pose_mode", ""), sidecar, error))
                    return mcp::Result::error(error);
                  ws.scan();
                  ws.select("meshy:" + args.value("name", "") + "_concept");
                  return mcp::Result::json(sidecar);
                }});
  registry.add({"meshy_image_to_3d",
                "Turn the approved concept meshy/<name>_concept into a 3D model with Meshy (about 20 credits, 30 "
                "with texture), as the mesh item meshy/<name>_mesh/ (model.glb downloaded right away, since Meshy "
                "deletes API results after 3 days). Waits up to wait_seconds (default 300); if unfinished, call "
                "meshy_task. Then size it with mesh_transform and run mesh_recommend.",
                mcp::schema({{"name", {{"type", "string"}}},
                             {"concept", {{"type", "string"}, {"description", "the concept's base name if different from name"}}},
                             {"options", {{"type", "object"}, {"description", "Meshy image-to-3d fields, e.g. should_texture, should_remesh, target_polycount, model_type"}}},
                             {"wait_seconds", {{"type", "integer"}}}},
                            {"name", "concept"}),
                [&client, &ws](const json &args) {
                  ws.scan();
                  workspace::Item concept_item;
                  std::string cid = "meshy:" + args.value("concept", args.value("name", "")) + "_concept";
                  if (!ws.find(cid, concept_item)) return mcp::Result::error("no concept " + cid);
                  if (concept_item.state() != "final")
                    return mcp::Result::error(cid + " isn't approved yet; ask the human (and use approval_wait)");
                  json sidecar;
                  std::string error;
                  bool ok = client.image_to_3d(args.value("name", ""), args.value("concept", args.value("name", "")),
                                               args.value("options", json::object()), args.value("wait_seconds", 300),
                                               sidecar, error);
                  if (!ok) return mcp::Result{{{"text", error + "\n" + sidecar.dump(2), "", ""}}, true};
                  ws.scan();
                  ws.select("meshy:" + args.value("name", "") + "_mesh");
                  return mcp::Result::json(sidecar);
                }});
  registry.add({"meshy_task", "Keep waiting on an unfinished meshy_image_to_3d task, by its base name (without _mesh).",
                mcp::schema({{"name", {{"type", "string"}}}, {"wait_seconds", {{"type", "integer"}}}}, {"name"}),
                [&client](const json &args) {
                  json sidecar;
                  std::string error;
                  if (!client.resume_image_to_3d(args.value("name", ""), args.value("wait_seconds", 300), sidecar, error))
                    return mcp::Result{{{"text", error + "\n" + sidecar.dump(2), "", ""}}, true};
                  return mcp::Result::json(sidecar);
                }});
}

}  // namespace meshy
