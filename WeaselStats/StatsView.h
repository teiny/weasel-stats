#pragma once

#include <Windows.h>

#include <filesystem>

namespace weasel::stats {

int RunStatsView(HINSTANCE instance,
                 const std::filesystem::path& data_directory) noexcept;

}  // namespace weasel::stats
