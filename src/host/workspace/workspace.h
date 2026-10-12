// The workspace: everything a human can inspect or approve, mirroring the
// project layout (design/flow.md, "The workspace").
//
// Items and their manifests live on disk; this class scans them, tracks the
// selection, and applies approvals and notes. It's shared between the UI
// thread and MCP tool threads, so every method locks.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/mesh.h"

namespace workspace {

struct Item {
  std::string id;        // "<kind>:<name>", e.g. "system:mesh_progressive_v1"
  std::string kind;      // pipeline, system, channel, config, conformance, meshy, fixed, fracture
  std::string name;      // e.g. mesh_progressive_v1
  std::string path;      // the item's file or directory
  std::string manifest;  // the JSON file holding state and notes
  nlohmann::json data;   // the manifest's contents
  std::string state() const { return data.value("state", "draft"); }
};

// Something the viewport should show that isn't a file, such as a system's output.
struct Preview {
  std::string label;
  std::shared_ptr<const runtime::Mesh> mesh;
  bool has_conformance = false;
  float radius = 0, height = 0;
};

class Workspace {
 public:
  // Systems and channels come from the source root; everything else (pipelines,
  // config tables, conformance shapes, Meshy items, fixed assets) from the workspace root.
  Workspace(std::string source_root, std::string root);

  void scan();
  std::vector<Item> items() const;
  bool find(const std::string &id, Item &out) const;

  // Approval is the human's: the UI calls these, MCP tools never do.
  bool approve(const std::string &id, std::string &error);
  bool reject(const std::string &id, const std::string &reason, std::string &error);
  bool add_note(const std::string &id, const std::string &author, const std::string &text, std::string &error);

  void select(const std::string &id);
  std::string selected() const;
  // A viewport tab the UI should switch to (view, flow, video), taken once.
  void request_tab(const std::string &tab);
  std::string take_tab();
  // Whether the viewport shows reference photos in its corner, taken once (-1: no request).
  void request_reference(bool on);
  int take_reference();

  void set_preview(Preview preview);  // also selects the preview
  Preview preview() const;

  // Called (on the approving thread) after an item is approved, e.g. to stamp
  // an approved mesh's levels of detail.
  void set_on_approved(std::function<void(const Item &)> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_approved_ = std::move(hook);
  }

  // Bumped on every change, so the UI knows when to refresh.
  int version() const;

  const std::string &root() const { return root_; }  // the workspace root

 private:
  bool update_manifest(const std::string &id, const std::function<void(nlohmann::json &)> &edit, std::string &error);
  std::string source_root_;
  std::string root_;
  mutable std::mutex mutex_;
  std::vector<Item> items_;
  std::string selected_;
  std::string tab_;
  int reference_ = -1;
  Preview preview_;
  std::function<void(const Item &)> on_approved_;
  int version_ = 0;
};

}  // namespace workspace
