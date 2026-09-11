#pragma once

#include <cstdint>
#include <string_view>

namespace weasel::stats {

struct TextMetrics {
  std::uint64_t han_characters = 0;
  std::uint64_t english_words = 0;

  std::uint64_t overview_units() const {
    return han_characters + english_words;
  }
};

TextMetrics CountTextMetrics(std::string_view utf8_text);

}  // namespace weasel::stats
