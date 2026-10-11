#include "assets/meshio.h"

#include "core/util.h"

namespace assets {

namespace {

constexpr uint32_t kVersion = 1;

void vecs(foundry::Writer &w, const std::vector<foundry::vec3> &v) {
  w.array(v, [](foundry::Writer &w2, const foundry::vec3 &x) { w2.vec(x); });
}
void u32s(foundry::Writer &w, const std::vector<uint32_t> &v) {
  w.array(v, [](foundry::Writer &w2, uint32_t x) { w2.u32(x); });
}
void vecs(foundry::Reader &r, std::vector<foundry::vec3> &v) {
  r.array(v, 12, [](foundry::Reader &r2, foundry::vec3 &x) { x = r2.vec(); });
}
void u32s(foundry::Reader &r, std::vector<uint32_t> &v) {
  r.array(v, 4, [](foundry::Reader &r2, uint32_t &x) { x = r2.u32(); });
}

std::vector<uint8_t> header(const char *format) {
  std::vector<uint8_t> out;
  foundry::Writer w(out);
  w.u32(foundry::kDiskMagic);
  w.u32(kVersion);
  w.str(format);
  return out;
}

bool check(foundry::Reader &r, const char *format) {
  return r.u32() == foundry::kDiskMagic && r.u32() == kVersion && r.str() == format && r.ok();
}

}  // namespace

bool save_mesh(const std::string &path, const runtime::Mesh &m) {
  std::vector<uint8_t> out = header("foundry_mesh");
  foundry::Writer w(out);
  vecs(w, m.positions);
  vecs(w, m.normals);
  u32s(w, m.colors);
  u32s(w, m.indices);
  return core::write_file(path, out.data(), out.size());
}

bool load_mesh(const std::string &path, runtime::Mesh &m) {
  std::vector<uint8_t> in;
  if (!core::read_file(path, in)) return false;
  foundry::Reader r(in.data(), in.size());
  if (!check(r, "foundry_mesh")) return false;
  m = {};
  vecs(r, m.positions);
  vecs(r, m.normals);
  u32s(r, m.colors);
  u32s(r, m.indices);
  return r.ok();
}

bool save_progressive(const std::string &path, const ProgressiveMesh &p) {
  std::vector<uint8_t> out = header("foundry_progressive");
  foundry::Writer w(out);
  vecs(w, p.positions);
  vecs(w, p.normals);
  u32s(w, p.colors);
  u32s(w, p.indices);
  u32s(w, p.collapse);
  u32s(w, p.welded);
  u32s(w, p.level_vertices);
  u32s(w, p.level_triangles);
  u32s(w, p.step_welded);
  vecs(w, p.step_position);
  w.str(p.report);
  return core::write_file(path, out.data(), out.size());
}

bool load_progressive_file(const std::string &path, ProgressiveMesh &p) {
  std::vector<uint8_t> in;
  if (!core::read_file(path, in)) return false;
  foundry::Reader r(in.data(), in.size());
  if (!check(r, "foundry_progressive")) return false;
  p = {};
  vecs(r, p.positions);
  vecs(r, p.normals);
  u32s(r, p.colors);
  u32s(r, p.indices);
  u32s(r, p.collapse);
  u32s(r, p.welded);
  u32s(r, p.level_vertices);
  u32s(r, p.level_triangles);
  u32s(r, p.step_welded);
  vecs(r, p.step_position);
  p.report = r.str();
  return r.ok();
}

}  // namespace assets
