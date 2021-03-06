
#include "file_saver.hpp"
#include "magic_number.hpp"
#include "string_helpers.hpp"
#include "swbf_fnv_hashes.hpp"
#include "ucfb_reader.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

using namespace std::literals;

namespace {

void write_bracketed_str(std::string_view what, std::string& to)
{
   to += '[';
   to += what;
   to += "]\n\n"_sv;
}

void write_property(std::pair<std::string_view, std::string_view> prop_value,
                    std::string& to)
{
   to += prop_value.first;
   to += " = \""_sv;
   to += prop_value.second;
   to += "\"\n"_sv;
}

auto get_properties(Ucfb_reader object)
   -> std::vector<std::pair<std::uint32_t, std::string_view>>
{
   std::vector<std::pair<std::uint32_t, std::string_view>> properties;
   properties.reserve(128);

   while (object) {
      auto property = object.read_child_strict<"PROP"_mn>();

      const auto hash = property.read_trivial<std::uint32_t>();
      const auto value = property.read_string();

      properties.emplace_back(hash, value);
   }

   return properties;
}

auto find_geometry_name(
   const std::vector<std::pair<std::uint32_t, std::string_view>>& properties)
   -> std::optional<std::string>
{
   constexpr auto geometry_name_hash = 0x47c86b4aui32;

   const auto result =
      std::find_if(std::cbegin(properties), std::cend(properties),
                   [](const auto& prop) { return prop.first == 0x47c86b4a; });

   if (result != std::cend(properties)) return std::string{result->second} += ".msh"_sv;

   return std::nullopt;
}
}

void handle_object(Ucfb_reader object, File_saver& file_saver, std::string_view type)
{
   std::string file_buffer;
   file_buffer.reserve(1024);

   write_bracketed_str(type, file_buffer);

   const auto class_name = object.read_child_strict<"BASE"_mn>().read_string();

   write_property({"ClassLabel"_sv, class_name}, file_buffer);

   const auto odf_name = object.read_child_strict<"TYPE"_mn>().read_string();

   const auto properties = get_properties(object);

   const auto geom_name = find_geometry_name(properties);

   if (geom_name) write_property({"GeometryName"_sv, *geom_name}, file_buffer);

   file_buffer += '\n';

   write_bracketed_str("Properties"_sv, file_buffer);

   for (const auto& property : properties) {
      write_property({lookup_fnv_hash(property.first), property.second}, file_buffer);
   }

   file_saver.save_file(file_buffer, "odf"_sv, odf_name, ".odf"_sv);
}