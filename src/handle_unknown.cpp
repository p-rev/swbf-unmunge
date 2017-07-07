
#include"chunk_headers.hpp"
#include"file_saver.hpp"
#include"magic_number.hpp"
#include"string_helpers.hpp"

#include<atomic>
#include<string>
#include<optional>

namespace
{

using namespace std::literals;

std::string get_unique_chunk_name() noexcept
{
   static std::atomic_int64_t chunk_count{0};

   std::string result{"chunk_"s};

   result += std::to_string(chunk_count.fetch_add(1));
   result += ".munged"_sv;

   return result;
}

}

void handle_unknown(const chunks::Unknown& chunk,
                    File_saver& file_saver,
                    std::optional<std::string> file_name)
{
   const std::uint32_t size = chunk.size + sizeof(chunk);

   std::string file;
   file.resize(size + 8);

   reinterpret_cast<Magic_number&>(file[0]) = "ucfb"_mn;
   reinterpret_cast<std::uint32_t&>(file[4]) = size;

   std::memcpy(&file[8], &chunk, size);

   file_saver.save_file(std::move(file),
                        file_name ? *file_name : get_unique_chunk_name(),
                        "munged");
}