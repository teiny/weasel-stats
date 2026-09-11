#pragma once

#include <cstddef>
#include <cstdint>

namespace weasel::stats {

constexpr std::uint32_t kProtocolMagic = 0x53545357;  // WSTS
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kServerIdCapacity = 64;
constexpr std::size_t kDeviceIdCapacity = 128;
constexpr std::size_t kCommitTextCapacity = 4096;

enum class MessageType : std::uint16_t {
  kCommit = 1,
  kCorrection = 2,
  kGetToday = 3,
  kShutdown = 4,
  kSync = 5,
};

enum class ResponseStatus : std::uint16_t {
  kOk = 0,
  kInvalidRequest = 1,
  kDatabaseUnavailable = 2,
  kUnsupportedPlatform = 3,
  kSyncIncomplete = 4,
};

enum class SummaryStatus : std::uint16_t {
  kUnavailable = 0,
  kValid = 1,
  kDisabled = 2,
};

#pragma pack(push, 1)
struct Request {
  std::uint32_t magic = kProtocolMagic;
  std::uint16_t version = kProtocolVersion;
  MessageType type = MessageType::kGetToday;
  std::uint32_t size = sizeof(Request);
  std::uint32_t day = 0;
  std::uint64_t sequence = 0;
  std::uint32_t backspace_count = 0;
  std::uint32_t deleted_ascii_letters = 0;
  std::uint32_t text_size = 0;
  char server_id[kServerIdCapacity] = {};
  char device_id[kDeviceIdCapacity] = {};
  char text[kCommitTextCapacity] = {};
};

struct Response {
  std::uint32_t magic = kProtocolMagic;
  std::uint16_t version = kProtocolVersion;
  ResponseStatus status = ResponseStatus::kInvalidRequest;
  std::uint32_t size = sizeof(Response);
  std::uint32_t day = 0;
  std::uint64_t revision = 0;
  std::uint64_t overview_units = 0;
  std::uint64_t han_characters = 0;
  std::uint64_t english_words = 0;
  std::uint64_t commits = 0;
  std::uint64_t backspaces = 0;
  std::uint64_t deleted_ascii_letters = 0;
};
#pragma pack(pop)

inline bool IsValid(const Request& request) {
  return request.magic == kProtocolMagic &&
         request.version == kProtocolVersion &&
         request.size == sizeof(Request) &&
         request.text_size < kCommitTextCapacity;
}

inline bool IsValid(const Response& response) {
  return response.magic == kProtocolMagic &&
         response.version == kProtocolVersion &&
         response.size == sizeof(Response);
}

}  // namespace weasel::stats
