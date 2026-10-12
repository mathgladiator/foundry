// How information flows through a pipeline, and where caching can hold
// (design/flow.md, "Pipelines"): which parameters reach each stage, what a
// change to each parameter reruns, how many distinct outputs each stage can
// have, and (by probing) which declared dependencies actually change a
// stage's output.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pipeline/pipeline.h"

namespace pipeline {

struct FlowEdge {
  std::string from;   // a stage id
  std::string field;  // the field read ("bones"), or empty for the whole output
};

struct FlowStage {
  std::string id, system, phase, in, out;
  int depth = 0;                    // longest path from the parameters
  std::vector<std::string> direct;  // parameters its input binds
  std::vector<std::pair<std::string, std::string>> binds;  // (parameter, the input field it binds), e.g. (species, genome)
  std::vector<std::string> all;     // every parameter its output depends on: its cache key
  std::vector<FlowEdge> upstream;
  double distinct = 0;    // how many distinct outputs it can have; 0 when unbounded
  std::string distinct_text;
};

struct FlowParam {
  std::string name, type;
  double cardinality = 0;  // 0 when continuous or a clock
  std::string cardinality_text;
  std::vector<std::string> reruns;  // stages that rerun when it changes
  std::string table;                // config parameters: the table they pick from (config/<table>/)
  size_t entries = 0;               // and how many entries it has
  std::string when;                 // its gate (species.pickable), if any
  size_t when_on = 0;               // and how many of the gate's entries turn it on
};

// What a stage's output feeds on the GPU (the runtime's renderer): the draw model's vertices and
// indices, uploaded once per build, and the pose's bone matrices, streamed every frame.
struct FlowGpu {
  std::string stage;        // the stage whose output is uploaded
  std::string fields;       // the output fields it reads, e.g. "positions, normals, colors"
  std::string destination;  // e.g. "GPU vertex buffer"
  std::string detail;       // its layout and how often it's uploaded
};

struct Flow {
  std::vector<FlowParam> params;
  std::vector<FlowStage> stages;
  std::vector<FlowGpu> gpu;
  nlohmann::json to_json() const;
};

Flow analyze(const Definition &def);

// Varies each parameter away from `base` (other values held) and reports, per
// parameter and stage, whether the stage's output changed. A declared
// dependency that never changes an output is where a cache could be shared.
nlohmann::json probe(Runner &runner, const Definition &def, const nlohmann::json &base, int samples = 3);

}  // namespace pipeline
