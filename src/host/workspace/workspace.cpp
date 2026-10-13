#include "workspace/workspace.h"

#include <algorithm>
#include <functional>
#include <filesystem>

#include "core/log.h"
#include "core/util.h"

namespace fs = std::filesystem;

namespace workspace {

Workspace::Workspace(std::string source_root, std::string root)
    : source_root_(std::move(source_root)), root_(std::move(root)) {}

void Workspace::scan() {
  std::vector<Item> found;
  auto add = [&](const std::string &kind, const std::string &name, const fs::path &path, const fs::path &manifest) {
    Item item;
    item.kind = kind;
    item.name = name;
    item.id = kind + ":" + name;
    item.path = path.string();
    item.manifest = manifest.string();
    item.data = core::read_json(item.manifest);
    if (!item.data.is_object()) item.data = nlohmann::json::object();
    found.push_back(std::move(item));
  };
  auto each = [](const fs::path &dir, auto fn) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> entries;
    for (const auto &e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    for (const auto &p : entries) fn(p);
  };
  const fs::path root(root_), source(source_root_);
  each(root / "pipelines", [&](const fs::path &p) {
    if (p.extension() == ".json") add("pipeline", p.stem().string(), p, p);
  });
  each(root / "plotters", [&](const fs::path &p) {
    if (p.extension() == ".json") add("plotter", p.stem().string(), p, p);
  });
  each(root / "observatories", [&](const fs::path &p) {
    if (p.extension() == ".json") add("observatory", p.stem().string(), p, p);
  });
  each(source / "src/systems", [&](const fs::path &p) {
    if (fs::exists(p / "system.json")) add("system", p.filename().string(), p, p / "system.json");
  });
  each(source / "src/channels", [&](const fs::path &p) {
    std::string f = p.filename().string();
    if (p.extension() == ".json" && f.find(".manifest.") == std::string::npos) {
      std::string name = p.stem().string();
      add("channel", name, p, source / "src/channels" / (name + ".manifest.json"));
    }
  });
  // Config tables: config/<table>/<entry>.json, named <table>_<entry> (table.json describes the table).
  each(root / "config", [&](const fs::path &t) {
    each(t, [&](const fs::path &p) {
      if (p.extension() == ".json" && p.stem() != "table")
        add("config", t.filename().string() + "_" + p.stem().string(), p, p);
    });
  });
  each(root / "conformance", [&](const fs::path &p) {
    if (p.extension() == ".json") add("conformance", p.stem().string(), p, p);
  });
  each(root / "meshy", [&](const fs::path &p) {
    if (fs::exists(p / "item.json")) add("meshy", p.filename().string(), p, p / "item.json");
  });
  each(root / "fixed", [&](const fs::path &p) {
    if (fs::exists(p / "manifest.json")) add("fixed", p.filename().string(), p, p / "manifest.json");
  });
  // A fixed asset's fracture is its own item, beside it (design/transforms.md, "Fracture").
  each(root / "fixed", [&](const fs::path &p) {
    if (fs::exists(p / "fracture.json")) add("fracture", p.filename().string(), p, p / "fracture.json");
  });
  // Reference material: photos under reference/ (any depth), each with a JSON sidecar naming its file and
  // provenance; reference/trees/pine/01.json is reference:trees_pine_01 (design/flow.md, "Reference").
  std::function<void(const fs::path &, const std::string &)> refs = [&](const fs::path &dir, const std::string &prefix) {
    each(dir, [&](const fs::path &p) {
      std::error_code ec;
      if (fs::is_directory(p, ec)) {
        refs(p, prefix + p.filename().string() + "_");
      } else if (p.extension() == ".json") {
        const nlohmann::json side = core::read_json(p.string());
        if (side.is_object() && side.contains("file"))
          add("reference", prefix + p.stem().string(), p.parent_path() / side["file"].get<std::string>(), p);
      }
    });
  };
  refs(root / "reference", "");
  std::lock_guard<std::mutex> lock(mutex_);
  bool changed = found.size() != items_.size();
  for (size_t i = 0; !changed && i < found.size(); ++i)
    changed = found[i].id != items_[i].id || found[i].data != items_[i].data;
  if (changed) {
    items_ = std::move(found);
    ++version_;
  }
}

std::vector<Item> Workspace::items() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_;
}

bool Workspace::find(const std::string &id, Item &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &i : items_)
    if (i.id == id) {
      out = i;
      return true;
    }
  return false;
}

bool Workspace::update_manifest(const std::string &id, const std::function<void(nlohmann::json &)> &edit,
                                std::string &error) {
  Item item;
  if (!find(id, item)) {
    error = "no item " + id;
    return false;
  }
  nlohmann::json j = core::read_json(item.manifest);
  if (!j.is_object()) j = nlohmann::json::object();
  edit(j);
  if (!core::write_json(item.manifest, j)) {
    error = "can't write " + item.manifest;
    return false;
  }
  scan();
  return true;
}

bool Workspace::approve(const std::string &id, std::string &error) {
  if (id.rfind("reference:", 0) == 0) {
    error = "reference material isn't approved; it's what assets are checked against";
    return false;
  }
  bool ok = update_manifest(id, [](nlohmann::json &j) {
    j["state"] = "final";
    j["approved_by"] = "human";
    j["approved_at"] = core::now_iso();
  }, error);
  if (ok) {
    core::log().add("approval", id + " approved (final)");
    Item item;
    std::function<void(const Item &)> hook;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hook = on_approved_;
    }
    if (hook && find(id, item)) hook(item);
  }
  return ok;
}

bool Workspace::reject(const std::string &id, const std::string &reason, std::string &error) {
  if (id.rfind("reference:", 0) == 0) {
    error = "reference material isn't approved or rejected";
    return false;
  }
  Item item;
  if (find(id, item) && item.state() == "final") {
    error = id + " is final; fork it instead";
    return false;
  }
  bool ok = update_manifest(id, [&](nlohmann::json &j) {
    j["state"] = "rejected";
    if (!reason.empty()) {
      if (!j.contains("notes") || !j["notes"].is_array()) j["notes"] = nlohmann::json::array();
      j["notes"].push_back({{"author", "human"}, {"time", core::now_iso()}, {"text", "Rejected: " + reason}});
    }
  }, error);
  if (ok) core::log().add("approval", id + " rejected");
  return ok;
}

bool Workspace::add_note(const std::string &id, const std::string &author, const std::string &text, std::string &error) {
  return update_manifest(id, [&](nlohmann::json &j) {
    if (!j.contains("notes") || !j["notes"].is_array()) j["notes"] = nlohmann::json::array();
    j["notes"].push_back({{"author", author}, {"time", core::now_iso()}, {"text", text}});
  }, error);
}

void Workspace::select(const std::string &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (selected_ != id) {
    selected_ = id;
    ++version_;
  }
}

void Workspace::request_tab(const std::string &tab) {
  std::lock_guard<std::mutex> lock(mutex_);
  tab_ = tab;
}

void Workspace::request_reference(bool on) {
  std::lock_guard<std::mutex> lock(mutex_);
  reference_ = on ? 1 : 0;
}

int Workspace::take_reference() {
  std::lock_guard<std::mutex> lock(mutex_);
  const int r = reference_;
  reference_ = -1;
  return r;
}

std::string Workspace::take_tab() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string t;
  t.swap(tab_);
  return t;
}

std::string Workspace::selected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selected_;
}

void Workspace::set_preview(Preview preview) {
  std::lock_guard<std::mutex> lock(mutex_);
  preview_ = std::move(preview);
  selected_ = "preview";
  ++version_;
}

Preview Workspace::preview() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return preview_;
}

int Workspace::version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

}  // namespace workspace
