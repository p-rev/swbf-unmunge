#pragma once

#include "magic_number.hpp"
#include "string_helpers.hpp"
#include "type_pun.hpp"

#include <gsl/gsl>

#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <type_traits>

template<Magic_number type_mn>
class Ucfb_reader_strict;

//! \brief The class used for reading SWBF's game files.
//!
//! Each reader represents a non-owning view at a chunk from a "ucfb" file.
//! A reader holds only one piece of mutable state and that is the offset to the next
//! unread byte. When a reader is copied, this offset is also copied. It can however
//! be reset at anytime by calling Ucfb_reader::reset_head.
//!
//! The various `read_*` all advance the read head by the size of the value
//! they read. The read can be aligned or unaligned. When it is aligned the
//! read head is advanced to the next four byte margin after the read, else
//! it is only advanced by the size of the value.
//!
//! An inidividual instance of this class is not threadsafe, however because it
//! only represents an immutable view the multiple threads can safely hold different
//! readers all reading the same chunk.
class Ucfb_reader {
public:
   Ucfb_reader() = delete;

   //! \brief Creates a Ucfb_reader from a span of memory.
   //!
   //! \param bytes The span of memory holding the ucfb chunk. The size of the span must
   //! be at least 8.
   //!
   //! \exception std::runtime_error Thrown when the size of the chunk does not match the
   //! size of the span.
   Ucfb_reader(const gsl::span<const std::byte> bytes);

   template<Magic_number type_mn>
   Ucfb_reader(const Ucfb_reader_strict<type_mn>&) = delete;
   template<Magic_number type_mn>
   Ucfb_reader& operator=(const Ucfb_reader_strict<type_mn>&) = delete;

   template<Magic_number type_mn>
   Ucfb_reader(Ucfb_reader_strict<type_mn>&&) = delete;
   template<Magic_number type_mn>
   Ucfb_reader& operator=(Ucfb_reader_strict<type_mn>&&) = delete;

   //! \brief Reads a trivial value from the chunk.
   //!
   //! \tparam Type The type of the value to read. The type must be trivially copyable.
   //!
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A const reference to the value.
   //!
   //! \exception std::runtime_error Thrown when reading the value would go past the end
   //! of the chunk.
   template<typename Type>
   const Type& read_trivial(const bool unaligned = false)
   {
      static_assert(std::is_trivially_copyable_v<Type>,
                    "Type must be trivially copyable.");
      static_assert(!std::is_reference_v<Type>, "Type can not be a reference.");
      static_assert(!std::is_pointer_v<Type>, "Type can not be a pointer.");

      const auto cur_pos = _head;
      _head += sizeof(Type);

      check_head();

      if (!unaligned) align_head();

      return view_type_as<Type>(_data[cur_pos]);
   }

   //! \brief Reads a trivial unaligned value from the chunk.
   //!
   //! \tparam Type The type of the value to read. The type must be trivially copyable.
   //!
   //! \return A const reference to the value.
   //!
   //! \exception std::runtime_error Thrown when reading the value would go past the end
   //! of the chunk.
   template<typename Type>
   const Type& read_trivial_unaligned()
   {
      return read_trivial<Type>(true);
   }

   //! \brief Reads a variable-length array of trivial values from the chunk.
   //!
   //! \tparam Type The type of the values to read. The type must be trivially copyable.
   //!
   //! \param size The size of the array to read.
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A span of const Type.
   //!
   //! \exception std::runtime_error Thrown when reading the array would go past the end
   //! of the chunk.
   template<typename Type>
   auto read_array(typename const gsl::span<const Type>::index_type size,
                   const bool unaligned = false) -> gsl::span<const Type>
   {
      static_assert(std::is_trivially_copyable_v<Type>,
                    "Type must be trivially copyable.");
      static_assert(!std::is_reference_v<Type>, "Type can not be a reference.");
      static_assert(!std::is_pointer_v<Type>, "Type can not be a pointer.");

      const auto cur_pos = _head;
      _head += sizeof(Type) * size;

      check_head();

      if (!unaligned) align_head();

      return {&view_type_as<Type>(_data[cur_pos]), size};
   }

   //! \brief Reads an unaligned variable-length array of trivial values from the chunk.
   //!
   //! \tparam Type The type of the values to read. The type must be trivially copyable.
   //!
   //! \param size The size of the array to read.
   //!
   //! \return A span of const Type.
   //!
   //! \exception std::runtime_error Thrown when reading the array would go past the end
   //! of the chunk.
   template<typename Type>
   auto read_array_unaligned(typename const gsl::span<const Type>::index_type size)
      -> gsl::span<const Type>
   {
      return read_array<Type>(size, true);
   }

   //! \brief Reads a null-terminated string from a chunk.
   //!
   //! \tparam Char_type The char type of the values to read. Defaults to `char`.
   //!
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A string_view to the string.
   //!
   //! \exception std::runtime_error Thrown when reading the string would go past the end
   //! of the chunk.
   template<typename Char_type = char>
   auto read_string(const bool unaligned = false) -> std::basic_string_view<Char_type>
   {
      const Char_type* const string = reinterpret_cast<const Char_type*>(_data + _head);
      const auto string_length = cstring_length(string, _size - _head);

      _head += (string_length + 1) * sizeof(Char_type);

      check_head();

      if (!unaligned) align_head();

      return {string, string_length};
   }

   //! \brief Reads an unaligned null-terminated string from a chunk.
   //!
   //! \tparam Char_type The char type of the values to read. Defaults to `char`.
   //!
   //! \return A string_view to the string.
   //!
   //! \exception std::runtime_error Thrown when reading the string would go past the end
   //! of the chunk.
   template<typename Char_type = char>
   auto read_string_unaligned() -> std::basic_string_view<Char_type>
   {
      return read_string<Char_type>(true);
   }

   //! \brief Reads a child chunk.
   //!
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A new Ucfb_reader for the child chunk.
   //!
   //! \exception std::runtime_error Thrown when reading the child would go past the end
   //! of the current chunk.
   Ucfb_reader read_child(const bool unaligned = false);

   //! \brief Reads an unaligned child chunk.
   //!
   //! \return A new Ucfb_reader for the child chunk.
   //!
   //! \exception std::runtime_error Thrown when reading the child would go past the end
   //! of the current chunk.
   Ucfb_reader read_child_unaligned()
   {
      return read_child(true);
   }

   //! \brief Attempts to read a child chunk without the possibility of throwing an
   //! exception.
   //!
   //! \param <unnamed> std tag type for specifying the noexcept function.
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A std::optional<Ucfb_reader> for the child chunk. If the read failed
   //! (because it would overflow the chunk) nullopt is returned instead.
   auto read_child(const std::nothrow_t, const bool unaligned = false) noexcept
      -> std::optional<Ucfb_reader>;

   //! \brief Attempts to read an unaligned child chunk without the possibility of
   //! throwing an exception.
   //!
   //! \param <unnamed> std tag type for specifying the noexcept function.
   //!
   //! \return A std::optional<Ucfb_reader> for the child chunk. If the read failed
   //! (because it would overflow the chunk) nullopt is returned instead.
   auto read_child_unaligned(const std::nothrow_t) noexcept -> std::optional<Ucfb_reader>
   {
      return read_child(std::nothrow, true);
   }

   //! \brief Reads a child if it's magic number matches an expected
   //! magic number.
   //!
   //! \tparam type_mn The expected Magic_number of the child chunk.
   //!
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A new Ucfb_reader_strict<type_mn> for the child chunk.
   //!
   //! \exception std::runtime_error Thrown when the magic number of the child and the
   //! expected magic number do not match. If this happens the read head is not moved.
   //! \exception std::runtime_error Thrown when reading the child would go past the
   //! end of the current chunk.
   template<Magic_number type_mn>
   auto read_child_strict(const bool unaligned = false) -> Ucfb_reader_strict<type_mn>
   {
      return {read_child_strict(type_mn, unaligned),
              Ucfb_reader_strict<type_mn>::Unchecked_tag{}};
   }

   //! \brief Reads an unaligned child if it's magic number matches an expected
   //! magic number.
   //!
   //! \tparam type_mn The expected Magic_number of the child chunk.
   //!
   //! \return A new Ucfb_reader_strict<type_mn> for the child chunk.
   //!
   //! \exception std::runtime_error Thrown when the magic number of the child and the
   //! expected magic number do not match. If this happens the read head is not moved.
   //! \exception std::runtime_error Thrown when reading the child would go past the end
   //! of the current chunk.
   template<Magic_number type_mn>
   auto read_child_strict_unaligned() -> Ucfb_reader_strict<type_mn>
   {
      return read_child_strict<type_mn>(true);
   }

   //! \brief Reads a child if it's magic number matches an expected
   //! magic number.
   //!
   //! \tparam type_mn The expected Magic_number of the child chunk.
   //!
   //! \param unaligned If the read is unaligned or not.
   //!
   //! \return A std::optional<Ucfb_reader_strict<type_mn>> for the child chunk.
   //! If the magic number does not match std::nullopt is returned.
   //!
   //! \exception std::runtime_error Thrown when reading the child would go past the end
   //! of the current chunk.
   template<Magic_number type_mn>
   auto read_child_strict_optional(const bool unaligned = false)
      -> std::optional<Ucfb_reader_strict<type_mn>>
   {
      const auto child = read_child_strict_optional(type_mn, unaligned);

      if (child) {
         return {{*child, Ucfb_reader_strict<type_mn>::Unchecked_tag{}}};
      }

      return {};
   }

   //! \brief Reads an unaligned child if it's magic number matches an expected
   //! magic number.
   //!
   //! \tparam type_mn The expected Magic_number of the child chunk.
   //!
   //! \return A std::optional<Ucfb_reader_strict<type_mn>> for the child chunk.
   //! If the magic number does not match std::nullopt is returned.
   //!
   //! \exception std::runtime_error Thrown when reading the child would go past the end
   //! of the current chunk.
   template<Magic_number type_mn>
   auto read_child_strict_optional_unaligned()
      -> std::optional<Ucfb_reader_strict<type_mn>>
   {
      return read_child_strict_optional<type_mn>(true);
   }

   //! \brief Shifts the read head forward an amount of bytes.
   //!
   //! \param amount The amount to shift the head forward by.
   //! \param unaligned If the consume is unaligned or not.
   //!
   //! \exception std::runtime_error Thrown when the consume operation would go
   //! past the end of the chunk
   void consume(const std::size_t amount, const bool unaligned = false);

   //! \brief Shifts the read head forward an unaligned amount of bytes.
   //!
   //! \param amount The amount to shift the head forward by.
   //!
   //! \exception std::runtime_error Thrown when the consume operation would go
   //! past the end of the chunk
   void consume_unaligned(const std::size_t amount)
   {
      consume(amount, true);
   }

   //! \brief Tests if the end of the chunk has been reached or not.
   //!
   //! \return True if the end of the chunk has not been reached, false if it has.
   explicit operator bool() const noexcept;

   //! \brief Reset the read head.
   void reset_head() noexcept;

   //! \brief Gets the magic number of the chunk.
   //!
   //! \return The magic number of the chunk.
   Magic_number magic_number() const noexcept;

   //! \brief Gets size (in bytes) of the chunk.
   //!
   //! \return The size the chunk.
   std::size_t size() const noexcept;

private:
   // Special constructor for use by read_child, performs no error checking.
   Ucfb_reader(const Magic_number mn, const std::uint32_t size,
               const std::byte* const data);

   Ucfb_reader read_child_strict(const Magic_number child_mn, const bool unaligned);

   auto read_child_strict_optional(const Magic_number child_mn, const bool unaligned)
      -> std::optional<Ucfb_reader>;

   void check_head();

   void align_head() noexcept
   {
      const auto remainder = _head % 4;

      if (remainder != 0) _head += (4 - remainder);
   }

   const Magic_number _mn;
   const std::size_t _size;
   const std::byte* const _data;

   std::size_t _head = 0;
};

//! \brief A class used to restrict a reader to a specific magic number.
//!
//! \tparam type_mn The magic number to restrict the reader to.
template<Magic_number type_mn>
class Ucfb_reader_strict : public Ucfb_reader {
public:
   Ucfb_reader_strict() = delete;

   //! \brief Construct a strict reader.
   //!
   //! \param ucfb_reader The reader to construct the strict reader from.
   //! The magic number of the reader must match type_mn.
   explicit Ucfb_reader_strict(Ucfb_reader ucfb_reader) noexcept
      : Ucfb_reader{ucfb_reader}
   {
      Expects(type_mn == ucfb_reader.magic_number());
   }

private:
   friend class Ucfb_reader;

   struct Unchecked_tag {
   };

   Ucfb_reader_strict(Ucfb_reader ucfb_reader, Unchecked_tag) : Ucfb_reader{ucfb_reader}
   {
   }
};
