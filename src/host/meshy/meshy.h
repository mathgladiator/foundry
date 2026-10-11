// Meshy (design/third.party.md): concept images and image-to-3D, at build
// time only. Every call is checked against the credit budget and recorded in
// meshy/ledger.json; every result gets a provenance sidecar.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/config.h"

namespace meshy {

class Client {
 public:
  explicit Client(const core::Config &config);

  bool has_key() const { return !key_.empty(); }
  // GET /openapi/v1/balance
  bool balance(int &credits, std::string &error);

  // Text to image: writes meshy/<name>_concept/ (image.png, item.json).
  bool text_to_image(const std::string &name, const std::string &prompt, const std::string &model,
                     const std::string &aspect_ratio, const std::string &pose_mode, nlohmann::json &sidecar,
                     std::string &error);

  // Image to 3D from meshy/<concept>_concept (concept defaults to name):
  // writes meshy/<name>_mesh/ (model.glb, thumbnail.png, item.json). Waits up
  // to wait_seconds; if the task isn't done by then,
  // returns the task id in `sidecar` so meshy_task can resume.
  bool image_to_3d(const std::string &name, const std::string &concept_name, const nlohmann::json &options,
                   int wait_seconds, nlohmann::json &sidecar, std::string &error);

  // Resumes waiting on an image-to-3D task started earlier.
  bool resume_image_to_3d(const std::string &name, int wait_seconds, nlohmann::json &sidecar, std::string &error);

  nlohmann::json ledger() const;

 private:
  bool request(const std::string &method, const std::string &path, const nlohmann::json *body,
               nlohmann::json &response, std::string &error);
  bool poll(const std::string &kind, const std::string &id, int wait_seconds, nlohmann::json &task,
            std::string &error);
  bool reserve(const std::string &what, int estimate, std::string &error);
  void record(const nlohmann::json &entry);
  bool download(const std::string &url, const std::string &path, std::string &error);

  core::Config config_;
  std::string key_;
};

}  // namespace meshy
