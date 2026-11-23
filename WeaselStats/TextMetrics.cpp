#include "TextMetrics.h"

namespace weasel::stats {
namespace {

bool IsAsciiLetter(std::uint32_t code_point) {
  return (code_point >= 'A' && code_point <= 'Z') ||
         (code_point >= 'a' && code_point <= 'z');
}

bool IsHanCharacter(std::uint32_t code_point) {
  return (code_point >= 0x3400 && code_point <= 0x4DBF) ||
         (code_point >= 0x4E00 && code_point <= 0x9FFF) ||
         (code_point >= 0xF900 && code_point <= 0xFAFF) ||
         (code_point >= 0x20000 && code_point <= 0x2EE5F) ||
         (code_point >= 0x30000 && code_point <= 0x323AF);
}

std::uint32_t NextCodePoint(std::string_view text, std::size_t& offset) {
  const auto first = static_cast<unsigned char>(text[offset++]);
  if (first < 0x80) {
    return first;
  }

  std::uint32_t value = 0;
  std::size_t continuation_count = 0;
  if ((first & 0xE0) == 0xC0) {
    value = first & 0x1F;
    continuation_count = 1;
  } else if ((first & 0xF0) == 0xE0) {
    value = first & 0x0F;
    continuation_count = 2;
  } else if ((first & 0xF8) == 0xF0) {
    value = first & 0x07;
    continuation_count = 3;
  } else {
    return 0xFFFD;
  }

  if (offset + continuation_count > text.size()) {
    offset = text.size();
    return 0xFFFD;
  }
  for (std::size_t index = 0; index < continuation_count; ++index) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    if ((byte & 0xC0) != 0x80) {
      return 0xFFFD;
    }
    value = (value << 6) | (byte & 0x3F);
    ++offset;
  }
  return value;
}

}  // namespace

TextMetrics CountTextMetrics(std::string_view utf8_text) {
  TextMetrics result;
  bool in_english_word = false;
  std::size_t offset = 0;
  while (offset < utf8_text.size()) {
    const std::uint32_t code_point = NextCodePoint(utf8_text, offset);
    if (IsHanCharacter(code_point)) {
      ++result.han_characters;
    }
    const bool is_letter = IsAsciiLetter(code_point);
    if (is_letter && !in_english_word) {
      ++result.english_words;
    }
    in_english_word = is_letter;
  }
  return result;
}

}  // namespace weasel::stats
