#include "meshy/meshy.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "core/jobs.h"
#include "core/log.h"
#include "core/util.h"

namespace fs = std::filesystem;

namespace meshy {

namespace {

const char *kBase = "https://api.meshy.ai";
std::mutex g_ledger_mutex;

int image_cost(const std::string &model) {
  if (model == "nano-banana") return 3;
  if (model == "nano-banana-2") return 6;
  return 9;
}

}  // namespace

Client::Client(const core::Config &config) : config_(config) {
  key_ = core::read_env_key(config.meshy_keys_file, "MESHY_API_KEY");
  if (key_.empty()) {
    const char *env = std::getenv("MESHY_API_KEY");
    if (env) key_ = env;
  }
}

bool Client::request(const std::string &method, const std::string &path, const nlohmann::json *body,
                     nlohmann::json &response, std::string &error) {
  if (key_.empty()) {
    error = "no MESHY_API_KEY in " + config_.meshy_keys_file + " (meshy.keys_file in foundry.json)";
    return false;
  }
  // Pass the key in a private curl config file, not on the command line.
  char cfg[] = "/tmp/foundry-meshy-XXXXXX";
  int fd = mkstemp(cfg);
  if (fd < 0) {
    error = "can't create a temporary file";
    return false;
  }
  fchmod(fd, 0600);
  std::string conf = "header = \"Authorization: Bearer " + key_ + "\"\n";
  bool wrote = write(fd, conf.data(), conf.size()) == static_cast<ssize_t>(conf.size());
  close(fd);
  if (!wrote) {
    unlink(cfg);
    error = "can't write the temporary curl config";
    return false;
  }
  std::vector<std::string> argv = {"curl", "-sS", "-K", cfg, "-X", method, "--max-time", "120",
                                   "-w", "\n%{http_code}"};
  std::vector<uint8_t> input;
  if (body) {
    std::string b = body->dump();
    input.assign(b.begin(), b.end());
    argv.insert(argv.end(), {"-H", "Content-Type: application/json", "--data-binary", "@-"});
  }
  argv.push_back(std::string(kBase) + path);
  std::string err;
  std::vector<uint8_t> out;
  int status = core::run_process(argv, err, body ? &input : nullptr, &out);
  unlink(cfg);
  std::string text(out.begin(), out.end());
  auto nl = text.find_last_of('\n');
  int http = nl == std::string::npos ? 0 : std::atoi(text.c_str() + nl + 1);
  text = nl == std::string::npos ? text : text.substr(0, nl);
  if (status != 0) {
    error = "curl failed: " + err;
    return false;
  }
  try {
    response = text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text);
  } catch (const std::exception &) {
    response = {{"raw", text}};
  }
  if (http < 200 || http >= 300) {
    error = "Meshy HTTP " + std::to_string(http) + ": " + text.substr(0, 400);
    return false;
  }
  return true;
}

bool Client::balance(int &credits, std::string &error) {
  nlohmann::json r;
  if (!request("GET", "/openapi/v1/balance", nullptr, r, error)) return false;
  credits = r.value("balance", 0);
  return true;
}

nlohmann::json Client::ledger() const {
  std::lock_guard<std::mutex> lock(g_ledger_mutex);
  nlohmann::json l = core::read_json(config_.workspace + "/meshy/ledger.json");
  if (!l.is_object()) l = {{"entries", nlohmann::json::array()}};
  int spent = 0;
  for (const auto &e : l["entries"]) spent += e.value("credits", 0);
  l["budget"] = config_.meshy_budget_credits;
  l["spent"] = spent;
  l["remaining"] = config_.meshy_budget_credits - spent;
  return l;
}

bool Client::reserve(const std::string &what, int estimate, std::string &error) {
  nlohmann::json l = ledger();
  int remaining = l["remaining"];
  if (estimate > remaining) {
    error = what + " would cost about " + std::to_string(estimate) + " credits, but only " +
            std::to_string(remaining) + " of the " + std::to_string(config_.meshy_budget_credits) +
            "-credit budget remain. Ask the human to raise meshy.budget_credits in foundry.json.";
    return false;
  }
  return true;
}

void Client::record(const nlohmann::json &entry) {
  std::lock_guard<std::mutex> lock(g_ledger_mutex);
  const std::string path = config_.workspace + "/meshy/ledger.json";
  nlohmann::json l = core::read_json(path);
  if (!l.is_object()) l = {{"entries", nlohmann::json::array()}};
  l["entries"].push_back(entry);
  fs::create_directories(config_.workspace + "/meshy");
  core::write_json(path, l);
}

bool Client::poll(const std::string &kind, const std::string &id, int wait_seconds, nlohmann::json &task,
                  std::string &error) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
  int last_progress = -1;
  core::Job job("Meshy " + kind + ": " + id.substr(0, 8));
  while (true) {
    if (!request("GET", "/openapi/v1/" + kind + "/" + id, nullptr, task, error)) return false;
    std::string status = task.value("status", "");
    int progress = task.value("progress", 0);
    job.update(progress / 100.0f, status + " " + std::to_string(progress) + "%");
    if (progress != last_progress) {
      core::log().add("meshy", kind + " " + id + ": " + status + " " + std::to_string(progress) + "%");
      last_progress = progress;
    }
    if (status == "SUCCEEDED") return true;
    if (status == "FAILED" || status == "CANCELED") {
      error = kind + " task " + status + ": " + task.value("task_error", nlohmann::json::object()).dump();
      return false;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      error = "still " + status + " (" + std::to_string(progress) + "%) after " + std::to_string(wait_seconds) +
              "s; call meshy_task to keep waiting";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
  }
}

bool Client::download(const std::string &url, const std::string &path, std::string &error) {
  std::string out;
  int status = core::run_process({"curl", "-sSL", "--fail", "--max-time", "300", "-o", path, url}, out);
  if (status != 0) {
    error = "download failed: " + out;
    return false;
  }
  return true;
}

bool Client::text_to_image(const std::string &name, const std::string &prompt, const std::string &model_in,
                           const std::string &aspect_ratio, const std::string &pose_mode, nlohmann::json &item,
                           std::string &error) {
  if (!core::valid_name(name)) {
    error = "name must be lowercase letters, digits, and underscores";
    return false;
  }
  const std::string dir = config_.workspace + "/meshy/" + name + "_concept";
  if (fs::exists(dir + "/item.json")) {
    error = name + "_concept already exists; pick another name";
    return false;
  }
  const std::string model = model_in.empty() ? "nano-banana" : model_in;
  int estimate = image_cost(model);
  if (!reserve("text to image", estimate, error)) return false;
  nlohmann::json body = {{"ai_model", model}, {"prompt", prompt}};
  if (!aspect_ratio.empty()) body["aspect_ratio"] = aspect_ratio;
  if (!pose_mode.empty()) body["pose_mode"] = pose_mode;
  nlohmann::json created, task;
  if (!request("POST", "/openapi/v1/text-to-image", &body, created, error)) return false;
  const std::string id = created.value("result", "");
  core::log().add("meshy", "text to image " + name + " started (" + id + ")");
  // Credits are charged at creation; record what the task reports.
  bool done = poll("text-to-image", id, 300, task, error);
  int consumed = task.value("consumed_credits", done ? estimate : 0);
  record({{"time", core::now_iso()}, {"type", "text-to-image"}, {"task_id", id}, {"name", name + "_concept"},
          {"credits", consumed}, {"status", task.value("status", "")}});
  if (!done) return false;
  auto urls = task.value("image_urls", nlohmann::json::array());
  if (urls.empty()) {
    error = "task succeeded but returned no image";
    return false;
  }
  fs::create_directories(dir);
  if (!download(urls[0].get<std::string>(), dir + "/image.png", error)) return false;
  item = {{"name", name + "_concept"},
          {"kind", "concept"},
          {"state", "draft"},
          {"notes", nlohmann::json::array()},
          {"provenance",
           {{"service", "meshy"},
            {"type", "text-to-image"},
            {"ai_model", model},
            {"prompt", prompt},
            {"aspect_ratio", aspect_ratio},
            {"pose_mode", pose_mode},
            {"task_id", id},
            {"cost_credits", consumed},
            {"created_at", core::now_iso()},
            {"license", "Meshy-generated; check Meshy's terms for your plan before shipping"}}}};
  core::write_json(dir + "/item.json", item);
  core::log().add("meshy", name + "_concept saved (" + std::to_string(consumed) + " credits)");
  return true;
}

bool Client::image_to_3d(const std::string &name, const std::string &concept_name, const nlohmann::json &options,
                         int wait_seconds, nlohmann::json &item, std::string &error) {
  if (!core::valid_name(name)) {
    error = "name must be lowercase letters, digits, and underscores";
    return false;
  }
  const std::string dir = config_.workspace + "/meshy/" + name + "_mesh";
  if (fs::exists(dir + "/item.json")) {
    error = name + "_mesh already exists; use meshy_task to resume it, or pick another name";
    return false;
  }
  const std::string concept_item = (concept_name.empty() ? name : concept_name) + "_concept";
  const std::string concept_dir = config_.workspace + "/meshy/" + concept_item;
  nlohmann::json c = core::read_json(concept_dir + "/item.json");
  if (!c.is_object()) {
    error = "no concept " + concept_item;
    return false;
  }
  const nlohmann::json cp = c.value("provenance", nlohmann::json::object());
  bool texture = options.value("should_texture", true);
  int estimate = 20 + (texture ? 10 : 0);
  if (!reserve("image to 3D", estimate, error)) return false;

  nlohmann::json body = options;
  body["should_texture"] = texture;
  if (!body.contains("ai_model")) body["ai_model"] = "latest";
  // Reuse the concept's Meshy task when possible; otherwise upload the image.
  if (cp.value("service", "") == "meshy" && cp.contains("task_id")) {
    body["input_task_id"] = cp["task_id"];
  } else {
    std::vector<uint8_t> png;
    if (!core::read_file(concept_dir + "/image.png", png)) {
      error = concept_item + " has no image";
      return false;
    }
    body["image_url"] = "data:image/png;base64," + core::base64(png.data(), png.size());
  }
  nlohmann::json created;
  if (!request("POST", "/openapi/v1/image-to-3d", &body, created, error)) return false;
  const std::string id = created.value("result", "");
  core::log().add("meshy", "image to 3D " + name + " started (" + id + ")");
  fs::create_directories(dir);
  nlohmann::json body_record = body;
  if (body_record.contains("image_url")) body_record["image_url"] = "(" + concept_item + " uploaded)";
  item = {{"name", name + "_mesh"},
          {"kind", "mesh"},
          {"state", "draft"},
          {"notes", nlohmann::json::array()},
          {"concept", concept_item},
          {"ops", nlohmann::json::array()},
          {"provenance",
           {{"service", "meshy"}, {"type", "image-to-3d"}, {"concept", concept_item}, {"prompt", cp.value("prompt", "")},
            {"request", body_record}, {"task_id", id}, {"status", "PENDING"}, {"estimated_credits", estimate},
            {"created_at", core::now_iso()},
            {"license", "Meshy-generated; check Meshy's terms for your plan before shipping"}}}};
  core::write_json(dir + "/item.json", item);
  record({{"time", core::now_iso()}, {"type", "image-to-3d"}, {"task_id", id}, {"name", name + "_mesh"},
          {"credits", estimate}, {"status", "started (estimate)"}});
  return resume_image_to_3d(name, wait_seconds, item, error);
}

bool Client::resume_image_to_3d(const std::string &name, int wait_seconds, nlohmann::json &item, std::string &error) {
  const std::string dir = config_.workspace + "/meshy/" + name + "_mesh";
  item = core::read_json(dir + "/item.json");
  if (!item.is_object()) {
    error = "no mesh item " + name + "_mesh";
    return false;
  }
  nlohmann::json &prov = item["provenance"];
  const std::string status = prov.value("status", "");
  if (status == "SUCCEEDED") return true;
  if (status == "FAILED" || status == "CANCELED") {
    error = "task " + status + "; start a new one";
    return false;
  }
  nlohmann::json task;
  const std::string id = prov.value("task_id", "");
  bool done = poll("image-to-3d", id, wait_seconds, task, error);
  prov["status"] = task.value("status", status);
  prov["progress"] = task.value("progress", 0);
  if (task.value("status", "") == "SUCCEEDED" || task.value("status", "") == "FAILED") {
    int consumed = task.value("consumed_credits", 0);
    prov["cost_credits"] = consumed;
    // Correct the ledger's estimate with the real charge.
    record({{"time", core::now_iso()}, {"type", "image-to-3d correction"}, {"task_id", id}, {"name", name + "_mesh"},
            {"credits", consumed - prov.value("estimated_credits", 0)}, {"status", task.value("status", "")}});
  }
  if (done) {
    std::string url = task.value("model_urls", nlohmann::json::object()).value("glb", "");
    if (url.empty()) {
      error = "task succeeded but returned no GLB";
      core::write_json(dir + "/item.json", item);
      return false;
    }
    if (!download(url, dir + "/model.glb", error)) return false;
    std::string thumb = task.value("thumbnail_url", "");
    std::string ignored;
    if (!thumb.empty()) download(thumb, dir + "/thumbnail.png", ignored);
    core::log().add("meshy", name + "_mesh downloaded");
  }
  core::write_json(dir + "/item.json", item);
  return done;
}

}  // namespace meshy
