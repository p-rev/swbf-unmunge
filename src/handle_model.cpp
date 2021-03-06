
#include "bit_flags.hpp"
#include "glm_pod_wrappers.hpp"
#include "magic_number.hpp"
#include "math_helpers.hpp"
#include "msh_builder.hpp"
#include "string_helpers.hpp"
#include "type_pun.hpp"
#include "ucfb_reader.hpp"
#include "vbuf_reader.hpp"

#include "tbb/task_group.h"

#include <array>
#include <functional>
#include <limits>
#include <tuple>
#include <vector>

namespace {

enum class Material_flags : std::uint32_t {
   normal = 1,
   hardedged = 2,
   transparent = 4,
   glossmap = 8,
   glow = 16,
   bumpmap = 32,
   additive = 64,
   specular = 128,
   env_map = 256,
   vertex_lighting = 512,
   wireframe = 2048, // Name based off msh flags, may produce some other effect.
   doublesided = 65536,

   scrolling = 16777216,
   energy = 33554432,
   animated = 67108864,

   attached_light = 134217728,
};

enum class Material_flags_swbf1 : std::uint32_t {
   normal = 1,
   hardedged = 2,
   transparent = 4,
   specular = 48,
   additive = 128,
   glow = 256,
   detail = 512,
   scroll = 1024,
   reflection = 4096,
   camouflage = 8192,
   refraction = 16384
};

struct Material_info {
   Material_flags flags;
   std::uint32_t diffuse_colour;
   std::uint32_t specular_colour;
   std::uint32_t specular_intensity;
   std::uint32_t params[2];

   // There is a null-terminated string as the last member declaring the name of the
   // attached-light, this string is always present even if the attached_light flag is
   // unset.
};

static_assert(std::is_pod_v<Material_info>);
static_assert(sizeof(Material_info) == 24);

struct Model_info {

   std::array<glm::vec3, 2> vertex_box;
   std::array<glm::vec3, 2> visibility_box;
   std::uint32_t face_count{};
};

msh::Bbox create_bbox(const Model_info& model_info) noexcept
{
   msh::Bbox bbox;

   const auto& vertex_box = model_info.vertex_box;

   const auto sum = vertex_box[0] + vertex_box[1];
   bbox.centre = sum / 2.0f;

   const auto absolute = glm::abs(vertex_box[0] - vertex_box[1]);
   bbox.size = absolute / 2.0f;

   return bbox;
}

auto read_model_name(Ucfb_reader_strict<"NAME"_mn> name)
   -> std::pair<std::string, msh::Lod>
{
   const auto name_view = name.read_string();

   const auto suffix = name_view.substr(name_view.length() - 4, 4);
   const auto unsuffixed_name = std::string{name_view.substr(0, name_view.length() - 4)};

   if (suffix == "LOD1"_sv) {
      return {unsuffixed_name, msh::Lod::one};
   }
   else if (suffix == "LOD2"_sv) {
      return {unsuffixed_name, msh::Lod::two};
   }
   else if (suffix == "LOWD"_sv) {
      return {unsuffixed_name, msh::Lod::lowres};
   }

   return {std::string{name_view}, msh::Lod::zero};
}

Model_info read_model_info(Ucfb_reader_strict<"INFO"_mn> info)
{
   const auto size = info.size();

   if (size != 72 && size != 68) {
      throw std::runtime_error{"Unknow model info encountered."};
   }

   // swbfii has an array of four ints vs the array of three for swbf1
   if (size == 72) {
      info.read_trivial<std::array<std::int32_t, 4>>();
   }
   else {
      info.read_trivial<std::array<std::int32_t, 3>>();
   }

   const auto vertex_box = info.read_trivial<std::array<pod::Vec3, 2>>();
   const auto vis_box = info.read_trivial<std::array<pod::Vec3, 2>>();

   // read unknown int
   info.read_trivial<std::int32_t>();

   const auto face_count = info.read_trivial<std::uint32_t>();

   return {{vertex_box[0], vertex_box[1]}, {vis_box[0], vis_box[1]}, face_count};
}

void read_texture_name(Ucfb_reader_strict<"TNAM"_mn> texture_name,
                       std::array<std::string, 4>& out) noexcept
{
   const auto index = texture_name.read_trivial<std::uint32_t>();
   const auto name = texture_name.read_string();

   if (index < out.size()) {
      out[index] = name;
   }
}

auto read_vertex_strip_ps2(gsl::span<const std::uint16_t> indices, std::int64_t& pos)
   -> std::vector<std::uint16_t>
{
   if (pos + 1 >= indices.size()) throw std::out_of_range{"Index buffer invalid"};

   std::vector<std::uint16_t> strip;
   strip.reserve(32);

   strip.push_back(indices[pos] & ~(0x8000ui16));
   strip.push_back(indices[pos + 1] & ~(0x8000ui16));
   pos += 2;

   for (; pos < indices.size(); ++pos) {
      if ((indices[pos] & 0x8000ui16) == 0x8000ui16) break;

      strip.push_back(indices[pos]);
   }

   return strip;
}

auto read_index_buffer(Ucfb_reader_strict<"IBUF"_mn> index_buffer)
   -> std::vector<std::uint16_t>
{
   const auto indices_count = index_buffer.read_trivial<std::uint32_t>();
   const auto indices = index_buffer.read_array<std::uint16_t>(indices_count);

   return {std::cbegin(indices), std::cend(indices)};
}

auto read_strip_buffer(Ucfb_reader_strict<"STRP"_mn> strip_buffer,
                       const std::uint32_t index_count) -> std::vector<std::uint16_t>

{
   const auto indices = strip_buffer.read_array<std::uint16_t>(index_count);

   return {std::cbegin(indices), std::cend(indices)};
}

auto read_positions_buffer(Ucfb_reader_strict<"POSI"_mn> positions_buffer,
                           const std::uint32_t vertex_count,
                           const std::array<glm::vec3, 2>& vertex_box)
   -> std::vector<glm::vec3>
{
   static_assert(sizeof(std::array<std::uint16_t, 3>) == 6);

   const auto compressed_positions =
      positions_buffer.read_array<std::array<std::uint16_t, 3>>(vertex_count);

   std::vector<glm::vec3> positions;
   positions.reserve(vertex_count);

   constexpr std::array<float, 2> old_range = {0.0f, 65535.0f};
   const std::array<std::array<float, 2>, 3> new_ranges{
      {{vertex_box[0].x, vertex_box[1].x},
       {vertex_box[0].y, vertex_box[1].y},
       {vertex_box[0].z, vertex_box[1].z}}};

   for (const auto& compressed : compressed_positions) {
      glm::vec3 vert;
      vert.x = range_convert(static_cast<float>(compressed[0]), old_range, new_ranges[0]);
      vert.y = range_convert(static_cast<float>(compressed[1]), old_range, new_ranges[1]);
      vert.z = range_convert(static_cast<float>(compressed[2]), old_range, new_ranges[2]);

      positions.emplace_back(vert);
   }

   return positions;
}

auto read_normals_buffer(Ucfb_reader_strict<"NORM"_mn> normals_buffer,
                         const std::uint32_t vertex_count) -> std::vector<glm::vec3>
{
   static_assert(sizeof(std::array<std::int8_t, 3>) == 3);

   const auto compressed_normals =
      normals_buffer.read_array<std::array<std::int8_t, 3>>(vertex_count);

   std::vector<glm::vec3> normals;
   normals.reserve(vertex_count);

   for (const auto& compressed : compressed_normals) {
      normals.emplace_back(static_cast<float>(compressed[0]) / 127.f,
                           static_cast<float>(compressed[1]) / 127.f,
                           static_cast<float>(compressed[2]) / 127.f);
   }

   return normals;
}

auto read_uv_buffer(Ucfb_reader_strict<"TEX0"_mn> uv_buffer,
                    const std::uint32_t vertex_count) -> std::vector<glm::vec2>
{
   static_assert(sizeof(std::array<std::int16_t, 2>) == 4);

   const auto compressed_coords =
      uv_buffer.read_array<std::array<std::int16_t, 2>>(vertex_count);

   std::vector<glm::vec2> uv_coords;
   uv_coords.reserve(vertex_count);

   for (const auto& compressed : compressed_coords) {
      constexpr auto factor = 1.f / 2048.f;

      glm::vec2 uv{static_cast<float>(compressed[0]), static_cast<float>(compressed[1])};
      uv *= factor;

      uv.y = 1.f - glm::fract(uv.y);

      uv_coords.emplace_back(uv);
   }

   return uv_coords;
}

auto read_skin_buffer(Ucfb_reader_strict<"BONE"_mn> bone_buffer,
                      const std::uint32_t vertex_count) -> std::vector<msh::Skin_entry>
{
   const auto hardskin = bone_buffer.read_array<std::uint8_t>(vertex_count);

   std::vector<msh::Skin_entry> softskin;
   softskin.reserve(vertex_count);

   for (const auto& bone : hardskin) {
      softskin.emplace_back(
         msh::Skin_entry{glm::u8vec3{bone}, glm::vec3{1.0f, 0.0f, 0.0f}});
   }

   return softskin;
}

auto read_colour_buffer(Ucfb_reader_strict<"COL0"_mn> uv_buffer,
                        const std::uint32_t vertex_count) -> std::vector<glm::vec4>
{
   static_assert(sizeof(std::array<std::int16_t, 2>) == 4);

   const auto packed_colours = uv_buffer.read_array<std::uint32_t>(vertex_count);

   std::vector<glm::vec4> colours;
   colours.reserve(vertex_count);

   for (const auto& packed : packed_colours) {
      colours.emplace_back(glm::unpackSnorm4x8(packed).bgra());
   }

   return colours;
}

std::vector<std::uint8_t> read_bone_map(Ucfb_reader_strict<"BMAP"_mn> bone_map)
{
   const auto count = bone_map.read_trivial<std::uint32_t>();
   const auto bones = bone_map.read_array<std::uint8_t>(count);

   std::vector<std::uint8_t> result;
   result.resize(count);

   std::memcpy(result.data(), bones.data(), result.size());

   return result;
}

void read_material_swbf1(Ucfb_reader_strict<"MTRL"_mn> material, msh::Material& out)
{
   const auto flags = material.read_trivial<Material_flags_swbf1>();

   if (are_flags_set(flags, Material_flags_swbf1::hardedged)) {
      out.flags = set_flags(out.flags, msh::Render_flags::hardedged);
   }
   if (are_flags_set(flags, Material_flags_swbf1::transparent)) {
      out.flags = set_flags(out.flags, msh::Render_flags::transparent);
   }
   if (are_flags_set(flags, Material_flags_swbf1::specular)) {
      out.type_swbf1 = msh::Render_type_swbf1::specular;

      out.specular_value = static_cast<float>(material.read_trivial<std::int32_t>());
      out.specular_colour = glm::unpackUnorm4x8(material.read_trivial<std::uint32_t>());
   }
   if (are_flags_set(flags, Material_flags_swbf1::additive)) {
      out.flags = set_flags(out.flags, msh::Render_flags::additive);
   }
   if (are_flags_set(flags, Material_flags_swbf1::glow)) {
      out.type_swbf1 = msh::Render_type_swbf1::glow;
   }
   if (are_flags_set(flags, Material_flags_swbf1::detail)) {
      out.type_swbf1 = msh::Render_type_swbf1::detail;

      out.params[0] = static_cast<std::uint8_t>(
         range_convert(material.read_trivial<float>(), {-1.0f, 1.0f}, {-128.0f, 127.0f}));
      out.params[1] = static_cast<std::uint8_t>(
         range_convert(material.read_trivial<float>(), {-1.0f, 1.0f}, {-128.0f, 127.0f}));
   }
   if (are_flags_set(flags, Material_flags_swbf1::scroll)) {
      out.type_swbf1 = msh::Render_type_swbf1::scroll;

      out.params[0] = static_cast<std::uint8_t>(
         range_convert(material.read_trivial<float>(), {-1.0f, 1.0f}, {-128.0f, 127.0f}));
      out.params[1] = static_cast<std::uint8_t>(
         range_convert(material.read_trivial<float>(), {-1.0f, 1.0f}, {-128.0f, 127.0f}));
   }
   if (are_flags_set(flags, Material_flags_swbf1::reflection)) {
      out.type_swbf1 = msh::Render_type_swbf1::reflection;
   }
   if (are_flags_set(flags, Material_flags_swbf1::camouflage)) {
      out.type_swbf1 = msh::Render_type_swbf1::camouflage;
   }
   if (are_flags_set(flags, Material_flags_swbf1::refraction)) {
      out.type_swbf1 = msh::Render_type_swbf1::refraction;
   }
}

void read_material(Ucfb_reader_strict<"MTRL"_mn> material, msh::Material& out)
{
   // we can detect swbf1 vs swbf2 material information based off the size of
   // the chunk. swbf1 uses a varying sized chunk that never matches the size
   // of the swbfii one (which is a fixed size, save a trailing string whose purpose I
   // am unsure of).
   if (material.size() < sizeof(Material_info)) {
      return read_material_swbf1(material, out);
   }

   const auto info = material.read_trivial<Material_info>();

   out.diffuse_colour = glm::unpackUnorm4x8(info.diffuse_colour);
   out.specular_colour = glm::unpackUnorm4x8(info.specular_colour);
   out.specular_value = static_cast<float>(info.specular_intensity);

   out.params[0] = static_cast<std::uint8_t>(info.params[0]);
   out.params[1] = static_cast<std::uint8_t>(info.params[1]);

   const auto attached_light = material.read_string_unaligned();

   out.vertex_lighting = are_flags_set(info.flags, Material_flags::vertex_lighting);

   if (are_flags_set(info.flags, Material_flags::hardedged)) {
      out.flags = set_flags(out.flags, msh::Render_flags::hardedged);
   }
   if (are_flags_set(info.flags, Material_flags::transparent) &&
       !are_flags_set(info.flags, Material_flags::doublesided)) {
      out.flags = set_flags(out.flags, msh::Render_flags::transparent);
   }
   if (are_flags_set(info.flags, Material_flags::glow)) {
      out.flags = set_flags(out.flags, msh::Render_flags::glow);
   }
   if (are_flags_set(info.flags, Material_flags::bumpmap)) {
      out.type = msh::Render_type::bumpmap;
   }
   if (are_flags_set(info.flags, Material_flags::additive)) {
      out.flags = set_flags(out.flags, msh::Render_flags::additive);
   }
   if (are_flags_set(info.flags, Material_flags::specular)) {
      out.flags = set_flags(out.flags, msh::Render_flags::specular);
   }
   if (are_flags_set(info.flags, Material_flags::env_map)) {
      out.type = msh::Render_type::env_map;
   }
   if (are_flags_set(info.flags, Material_flags::wireframe)) {
      out.type = msh::Render_type::wireframe;
   }
   if (are_flags_set(info.flags, Material_flags::doublesided)) {
      out.flags = set_flags(out.flags, msh::Render_flags::doublesided);
   }
   if (are_flags_set(info.flags, Material_flags::scrolling)) {
      out.type = msh::Render_type::scrolling;
   }
   if (are_flags_set(info.flags, Material_flags::energy)) {
      out.type = msh::Render_type::energy;
   }
   if (are_flags_set(info.flags, Material_flags::animated)) {
      out.type = msh::Render_type::animated;
   }
   if (are_flags_set(info.flags, Material_flags::attached_light)) {
      out.attached_light = attached_light;
   }
}

void read_material_name(Ucfb_reader_strict<"MNAM"_mn> mnam, msh::Model& out)
{
   const auto name = mnam.read_string();

   out.material.name = name;
   out.name = name;
}

void read_render_type(Ucfb_reader_strict<"RTYP"_mn> render_type, msh::Material& out)
{
   const auto type = render_type.read_string();

   if (type == "Refraction"_sv) {
      out.type = msh::Render_type::refraction;
   }
   else if (type == "Bump"_sv) {
      if (out.type_swbf1 == msh::Render_type_swbf1::specular) {
         out.type_swbf1 = msh::Render_type_swbf1::bumpmap_specular;
      }
      else {
         out.type_swbf1 = msh::Render_type_swbf1::bumpmap;
      }
   }
   else if (type == "Water"_sv) {
      out.type_swbf1 = msh::Render_type_swbf1::water;
   }
}

void process_segment_pc(Ucfb_reader_strict<"segm"_mn> segment, const msh::Lod lod,
                        Model_info, msh::Builder& builder)
{
   msh::Model model{};
   model.lod = lod;

   std::vector<Ucfb_reader_strict<"VBUF"_mn>> vbufs;
   vbufs.reserve(8);

   while (segment) {
      const auto child = segment.read_child();

      if (child.magic_number() == "MTRL"_mn) {
         read_material(Ucfb_reader_strict<"MTRL"_mn>{child}, model.material);
      }
      else if (child.magic_number() == "RTYP"_mn) {
         read_render_type(Ucfb_reader_strict<"RTYP"_mn>{child}, model.material);
      }
      else if (child.magic_number() == "MNAM"_mn) {
         read_material_name(Ucfb_reader_strict<"MNAM"_mn>{child}, model);
      }
      else if (child.magic_number() == "TNAM"_mn) {
         read_texture_name(Ucfb_reader_strict<"TNAM"_mn>{child}, model.material.textures);
      }
      else if (child.magic_number() == "IBUF"_mn) {
         model.strips.emplace_back() =
            read_index_buffer(Ucfb_reader_strict<"IBUF"_mn>{child});
      }
      else if (child.magic_number() == "VBUF"_mn) {
         vbufs.emplace_back(Ucfb_reader_strict<"VBUF"_mn>{child});
      }
      else if (child.magic_number() == "BNAM"_mn) {
         model.parent = Ucfb_reader_strict<"BNAM"_mn>{child}.read_string();
      }
      else if (child.magic_number() == "BMAP"_mn) {
         model.bone_map = read_bone_map(Ucfb_reader_strict<"BMAP"_mn>{child});
      }
   }

   read_vbuf(vbufs, model, &model.pretransformed);

   builder.add_model(std::move(model));
}

void process_segment_xbox(Ucfb_reader_strict<"segm"_mn> segment, const msh::Lod lod,
                          Model_info info, msh::Builder& builder)
{
   msh::Model model{};
   model.lod = lod;

   while (segment) {
      const auto child = segment.read_child();

      if (child.magic_number() == "MTRL"_mn) {
         read_material(Ucfb_reader_strict<"MTRL"_mn>{child}, model.material);
      }
      else if (child.magic_number() == "RTYP"_mn) {
         read_render_type(Ucfb_reader_strict<"RTYP"_mn>{child}, model.material);
      }
      else if (child.magic_number() == "MNAM"_mn) {
         read_material_name(Ucfb_reader_strict<"MNAM"_mn>{child}, model);
      }
      else if (child.magic_number() == "TNAM"_mn) {
         read_texture_name(Ucfb_reader_strict<"TNAM"_mn>{child}, model.material.textures);
      }
      else if (child.magic_number() == "IBUF"_mn) {
         model.strips.emplace_back() =
            read_index_buffer(Ucfb_reader_strict<"IBUF"_mn>{child});
      }
      else if (child.magic_number() == "VBUF"_mn) {
         read_vbuf_xbox(Ucfb_reader_strict<"VBUF"_mn>{child}, model, info.vertex_box,
                        &model.pretransformed);
      }
      else if (child.magic_number() == "BNAM"_mn) {
         model.parent = Ucfb_reader_strict<"BNAM"_mn>{child}.read_string();
      }
      else if (child.magic_number() == "BMAP"_mn) {
         model.bone_map = read_bone_map(Ucfb_reader_strict<"BMAP"_mn>{child});
      }
   }

   builder.add_model(std::move(model));
}

void process_segment_ps2(Ucfb_reader_strict<"segm"_mn> segment, const msh::Lod lod,
                         Model_info model_info, msh::Builder& builder)
{
   msh::Model model{};
   model.lod = lod;

   auto info = segment.read_child_strict<"INFO"_mn>();
   const auto vertex_count = info.read_trivial<std::uint32_t>();
   const auto index_count = info.read_trivial<std::uint32_t>();

   while (segment) {
      const auto child = segment.read_child();

      if (child.magic_number() == "MTRL"_mn) {
         read_material(Ucfb_reader_strict<"MTRL"_mn>{child}, model.material);
      }
      else if (child.magic_number() == "RTYP"_mn) {
         auto rtyp = Ucfb_reader_strict<"RTYP"_mn>{child};

         model.material.type =
            static_cast<msh::Render_type>(rtyp.read_trivial<std::uint32_t>());
      }
      else if (child.magic_number() == "MNAM"_mn) {
         read_material_name(Ucfb_reader_strict<"MNAM"_mn>{child}, model);
      }
      else if (child.magic_number() == "TNAM"_mn) {
         read_texture_name(Ucfb_reader_strict<"TNAM"_mn>{child}, model.material.textures);
      }
      else if (child.magic_number() == "STRP"_mn) {
         model.strips.emplace_back() =
            read_strip_buffer(Ucfb_reader_strict<"STRP"_mn>{child}, index_count);
      }
      else if (child.magic_number() == "POSI"_mn) {
         model.positions = read_positions_buffer(Ucfb_reader_strict<"POSI"_mn>{child},
                                                 vertex_count, model_info.vertex_box);
      }
      else if (child.magic_number() == "NORM"_mn) {
         model.normals =
            read_normals_buffer(Ucfb_reader_strict<"NORM"_mn>{child}, vertex_count);
      }
      else if (child.magic_number() == "TEX0"_mn) {
         model.texture_coords =
            read_uv_buffer(Ucfb_reader_strict<"TEX0"_mn>{child}, vertex_count);
      }
      else if (child.magic_number() == "COL0"_mn) {
         model.colours =
            read_colour_buffer(Ucfb_reader_strict<"COL0"_mn>{child}, vertex_count);
      }
      else if (child.magic_number() == "BMAP"_mn) {
         model.bone_map = read_bone_map(Ucfb_reader_strict<"BMAP"_mn>{child});
         model.pretransformed = true;
      }
      else if (child.magic_number() == "BONE"_mn) {
         model.skin =
            read_skin_buffer(Ucfb_reader_strict<"BONE"_mn>{child}, vertex_count);
      }
      else if (child.magic_number() == "BNAM"_mn) {
         model.parent = Ucfb_reader_strict<"BNAM"_mn>{child}.read_string();
      }
   }

   builder.add_model(std::move(model));
}

template<typename Segm_processor>
void handle_model_impl(Segm_processor&& segm_processor, Ucfb_reader model,
                       msh::Builders_map& builders)
{
   auto [name, lod] = read_model_name(model.read_child_strict<"NAME"_mn>());

   model.read_child_strict_optional<"VRTX"_mn>();

   model.read_child_strict<"NODE"_mn>();
   const auto model_info = read_model_info(model.read_child_strict<"INFO"_mn>());

   auto& builder = builders[name];

   builder.set_bbox(create_bbox(model_info));

   while (model) {
      const auto child = model.read_child();

      if (child.magic_number() == "segm"_mn) {
         std::invoke(std::forward<Segm_processor>(segm_processor),
                     Ucfb_reader_strict<"segm"_mn>{child}, lod, model_info, builder);
      }
   }
}
}

void handle_model(Ucfb_reader model, msh::Builders_map& builders)
{
   handle_model_impl(process_segment_pc, model, builders);
}

void handle_model_xbox(Ucfb_reader model, msh::Builders_map& builders)
{
   handle_model_impl(process_segment_xbox, model, builders);
}

void handle_model_ps2(Ucfb_reader model, msh::Builders_map& builders)
{
   handle_model_impl(process_segment_ps2, model, builders);
}
