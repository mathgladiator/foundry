#include "assets/glb.h"

#include <cmath>
#include <algorithm>
#include <map>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "viewport/image.h"

namespace assets {

namespace {

uint32_t pack_rgba(float r, float g, float b) {
  auto q = [](float v) { return static_cast<uint32_t>(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
  return q(r) | (q(g) << 8) | (q(b) << 16) | (255u << 24);
}

float srgb_to_linear_identity(uint8_t v) { return v / 255.0f; }

struct Texture {
  viewport::Image image;
  bool ok = false;
};

const Texture &texture_for(const cgltf_image *img, const std::string &base_dir, std::map<const cgltf_image *, Texture> &cache) {
  auto it = cache.find(img);
  if (it != cache.end()) return it->second;
  Texture t;
  if (img->buffer_view) {
    const auto *bv = img->buffer_view;
    const uint8_t *data = static_cast<const uint8_t *>(bv->buffer->data) + bv->offset;
    t.ok = viewport::decode_image(data, bv->size, t.image);
  } else if (img->uri && std::string(img->uri).rfind("data:", 0) != 0) {
    t.ok = viewport::load_image(base_dir + "/" + img->uri, t.image);
  }
  return cache[img] = std::move(t);
}

}  // namespace

bool import_gltf(const std::string &path, runtime::Mesh &out, std::string &error) {
  cgltf_options options = {};
  cgltf_data *data = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
    error = "can't parse " + path;
    return false;
  }
  if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
    cgltf_free(data);
    error = "can't load buffers for " + path;
    return false;
  }
  std::string base_dir = path.substr(0, path.find_last_of('/'));
  std::map<const cgltf_image *, Texture> textures;
  out = {};

  for (cgltf_size n = 0; n < data->nodes_count; ++n) {
    const cgltf_node *node = &data->nodes[n];
    if (!node->mesh) continue;
    float world[16];
    cgltf_node_transform_world(node, world);
    auto xform = [&](const float *v, float w) {
      return foundry::vec3{world[0] * v[0] + world[4] * v[1] + world[8] * v[2] + world[12] * w,
                           world[1] * v[0] + world[5] * v[1] + world[9] * v[2] + world[13] * w,
                           world[2] * v[0] + world[6] * v[1] + world[10] * v[2] + world[14] * w};
    };
    for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi) {
      const cgltf_primitive &prim = node->mesh->primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles) continue;
      const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *col = nullptr;
      for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const auto &attr = prim.attributes[a];
        if (attr.type == cgltf_attribute_type_position) pos = attr.data;
        else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
        else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
        else if (attr.type == cgltf_attribute_type_color && attr.index == 0) col = attr.data;
      }
      if (!pos) continue;
      float factor[4] = {1, 1, 1, 1};
      const Texture *tex = nullptr;
      if (prim.material && prim.material->has_pbr_metallic_roughness) {
        const auto &pbr = prim.material->pbr_metallic_roughness;
        for (int k = 0; k < 4; ++k) factor[k] = pbr.base_color_factor[k];
        if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
          const Texture &t = texture_for(pbr.base_color_texture.texture->image, base_dir, textures);
          if (t.ok) tex = &t;
        }
      }
      const uint32_t base = static_cast<uint32_t>(out.positions.size());
      for (cgltf_size v = 0; v < pos->count; ++v) {
        float p[3] = {0, 0, 0}, nn[3] = {0, 1, 0}, t[2] = {0, 0}, c[4] = {1, 1, 1, 1};
        cgltf_accessor_read_float(pos, v, p, 3);
        out.positions.push_back(xform(p, 1));
        if (nrm) {
          cgltf_accessor_read_float(nrm, v, nn, 3);
          foundry::vec3 w = xform(nn, 0);
          float l = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
          if (l > 0) w = {w.x / l, w.y / l, w.z / l};
          out.normals.push_back(w);
        }
        if (col) cgltf_accessor_read_float(col, v, c, 4);
        float r = factor[0] * c[0], g = factor[1] * c[1], b = factor[2] * c[2];
        if (tex && uv) {
          cgltf_accessor_read_float(uv, v, t, 2);
          const auto &img = tex->image;
          float u = t[0] - std::floor(t[0]), vv = t[1] - std::floor(t[1]);
          int x = std::min(img.width - 1, static_cast<int>(u * img.width));
          int y = std::min(img.height - 1, static_cast<int>(vv * img.height));
          const uint8_t *px = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
          r *= srgb_to_linear_identity(px[0]);
          g *= srgb_to_linear_identity(px[1]);
          b *= srgb_to_linear_identity(px[2]);
        }
        out.colors.push_back(pack_rgba(r, g, b));
      }
      if (prim.indices) {
        for (cgltf_size i = 0; i < prim.indices->count; ++i)
          out.indices.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
      } else {
        for (cgltf_size i = 0; i < pos->count; ++i) out.indices.push_back(base + static_cast<uint32_t>(i));
      }
    }
  }
  cgltf_free(data);
  // Keep normals all-or-nothing, as mesh expects.
  if (out.normals.size() != out.positions.size()) out.normals.clear();
  if (out.positions.empty()) {
    error = "no triangle meshes in " + path;
    return false;
  }
  return true;
}

}  // namespace assets
