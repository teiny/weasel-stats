#include "StatsReport.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace weasel::stats {
namespace {

class Statement {
 public:
  Statement(WinSqlite& sqlite, sqlite3* database, const char* sql)
      : sqlite_(sqlite) {
    if (sqlite_.prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK) {
      statement_ = nullptr;
    }
  }

  ~Statement() {
    if (statement_) {
      sqlite_.finalize(statement_);
    }
  }

  sqlite3_stmt* get() const { return statement_; }

 private:
  WinSqlite& sqlite_;
  sqlite3_stmt* statement_ = nullptr;
};

class Database {
 public:
  explicit Database(WinSqlite& sqlite) : sqlite_(sqlite) {}
  ~Database() {
    if (database_) {
      sqlite_.close(database_);
    }
  }

  sqlite3** address() { return &database_; }
  sqlite3* get() const { return database_; }

 private:
  WinSqlite& sqlite_;
  sqlite3* database_ = nullptr;
};

struct Date {
  int year = 0;
  int month = 0;
  int day = 0;
};

struct Point {
  int key = 0;
  std::string label;
  std::string period;
  std::uint64_t value = 0;
  bool current = false;
};

struct Result {
  bool available = true;
  ReportGranularity granularity = ReportGranularity::kDay;
  int anchor = 0;
  int previous_anchor = 0;
  int next_anchor = 0;
  int weekday_offset = 0;
  std::string title;
  std::vector<std::string> devices;
  std::set<std::string> selected_devices;
  std::vector<Point> points;
};

bool IsLeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

Date Today() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  return {time.wYear, time.wMonth, time.wDay};
}

int DayKey(int year, int month, int day) {
  return year * 10000 + month * 100 + day;
}

int MonthKey(int year, int month) {
  return year * 100 + month;
}

Date ParseDay(int value) {
  return {value / 10000, value / 100 % 100, value % 100};
}

bool IsValidDate(const Date& date) {
  return date.year >= 1 && date.year <= 9999 && date.month >= 1 &&
         date.month <= 12 && date.day >= 1 &&
         date.day <= DaysInMonth(date.year, date.month);
}

bool IsValidMonth(int value) {
  const int year = value / 100;
  const int month = value % 100;
  return year >= 1 && year <= 9999 && month >= 1 && month <= 12;
}

int PreviousMonth(int value) {
  int year = value / 100;
  int month = value % 100;
  if (--month == 0) {
    month = 12;
    --year;
  }
  return MonthKey(year, month);
}

int NextMonth(int value) {
  int year = value / 100;
  int month = value % 100;
  if (++month == 13) {
    month = 1;
    ++year;
  }
  return MonthKey(year, month);
}

int MondayBasedWeekday(int year, int month, int day) {
  static constexpr int kOffsets[] = {0, 3, 2, 5, 0, 3,
                                     5, 1, 4, 6, 2, 4};
  if (month < 3) {
    --year;
  }
  const int sunday_based =
      (year + year / 4 - year / 100 + year / 400 +
       kOffsets[month - 1] + day) %
      7;
  return (sunday_based + 6) % 7;
}

std::string TwoDigits(int value) {
  std::ostringstream stream;
  stream << std::setw(2) << std::setfill('0') << value;
  return stream.str();
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream stream;
  stream << '"';
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        stream << "\\\"";
        break;
      case '\\':
        stream << "\\\\";
        break;
      case '\b':
        stream << "\\b";
        break;
      case '\f':
        stream << "\\f";
        break;
      case '\n':
        stream << "\\n";
        break;
      case '\r':
        stream << "\\r";
        break;
      case '\t':
        stream << "\\t";
        break;
      default:
        if (character < 0x20) {
          stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(character) << std::dec;
        } else {
          stream << static_cast<char>(character);
        }
    }
  }
  stream << '"';
  return stream.str();
}

const char* GranularityName(ReportGranularity granularity) {
  switch (granularity) {
    case ReportGranularity::kDay:
      return "day";
    case ReportGranularity::kMonth:
      return "month";
    case ReportGranularity::kYear:
      return "year";
  }
  return "day";
}

void AddValue(std::map<int, std::uint64_t>& values,
              int key,
              std::uint64_t value) {
  auto& current = values[key];
  if (value > (std::numeric_limits<std::uint64_t>::max)() - current) {
    current = (std::numeric_limits<std::uint64_t>::max)();
  } else {
    current += value;
  }
}

void DefinePeriods(Result& result, int earliest_day, const Date& today) {
  Date earliest = ParseDay(earliest_day);
  if (!IsValidDate(earliest) || earliest_day > DayKey(today.year, today.month,
                                                      today.day)) {
    earliest = today;
  }
  const int first_month = MonthKey(earliest.year, earliest.month);
  const int current_month = MonthKey(today.year, today.month);

  if (result.granularity == ReportGranularity::kDay) {
    result.anchor = IsValidMonth(result.anchor) ? result.anchor : current_month;
    result.anchor = (std::max)(first_month,
                               (std::min)(result.anchor, current_month));
    const int year = result.anchor / 100;
    const int month = result.anchor % 100;
    const int last_day = result.anchor == current_month
                             ? today.day
                             : DaysInMonth(year, month);
    result.title = std::to_string(year) + "年" + std::to_string(month) + "月";
    result.weekday_offset = MondayBasedWeekday(year, month, 1);
    result.previous_anchor =
        result.anchor > first_month ? PreviousMonth(result.anchor) : 0;
    result.next_anchor =
        result.anchor < current_month ? NextMonth(result.anchor) : 0;
    for (int day = 1; day <= last_day; ++day) {
      Point point;
      point.key = DayKey(year, month, day);
      point.label = std::to_string(day) + "日";
      point.period = std::to_string(year) + "-" + TwoDigits(month) + "-" +
                     TwoDigits(day);
      point.current = point.key == DayKey(today.year, today.month, today.day);
      result.points.push_back(std::move(point));
    }
    return;
  }

  const int first_year = earliest.year;
  if (result.granularity == ReportGranularity::kMonth) {
    result.anchor = result.anchor ? result.anchor : today.year;
    result.anchor =
        (std::max)(first_year, (std::min)(result.anchor, today.year));
    result.title = std::to_string(result.anchor) + "年";
    result.previous_anchor = result.anchor > first_year ? result.anchor - 1 : 0;
    result.next_anchor = result.anchor < today.year ? result.anchor + 1 : 0;
    const int last_month = result.anchor == today.year ? today.month : 12;
    for (int month = 1; month <= last_month; ++month) {
      Point point;
      point.key = MonthKey(result.anchor, month);
      point.label = std::to_string(month) + "月";
      point.period = std::to_string(result.anchor) + "-" + TwoDigits(month);
      point.current =
          result.anchor == today.year && month == today.month;
      result.points.push_back(std::move(point));
    }
    return;
  }

  result.anchor = result.anchor ? result.anchor : today.year;
  result.anchor =
      (std::max)(first_year, (std::min)(result.anchor, today.year));
  const int first_visible_year = (std::max)(first_year, result.anchor - 9);
  result.previous_anchor =
      first_visible_year > first_year ? first_visible_year - 1 : 0;
  result.next_anchor =
      result.anchor < today.year ? (std::min)(today.year, result.anchor + 10)
                                 : 0;
  result.title = first_visible_year == result.anchor
                     ? std::to_string(result.anchor) + "年"
                     : std::to_string(first_visible_year) + "—" +
                           std::to_string(result.anchor) + "年";
  for (int year = first_visible_year; year <= result.anchor; ++year) {
    Point point;
    point.key = year;
    point.label = std::to_string(year) + "年";
    point.period = std::to_string(year);
    point.current = year == today.year;
    result.points.push_back(std::move(point));
  }
}

void ApplyValues(Result& result, const std::map<int, std::uint64_t>& values) {
  for (auto& point : result.points) {
    const auto found = values.find(point.key);
    if (found != values.end()) {
      point.value = found->second;
    }
  }
}

std::string Serialize(const Result& result) {
  std::ostringstream stream;
  stream << "{\"type\":\"report\",\"available\":"
         << (result.available ? "true" : "false") << ",\"granularity\":"
         << JsonEscape(GranularityName(result.granularity))
         << ",\"anchor\":" << result.anchor << ",\"previousAnchor\":"
         << result.previous_anchor << ",\"nextAnchor\":"
         << result.next_anchor << ",\"weekdayOffset\":"
         << result.weekday_offset << ",\"title\":" << JsonEscape(result.title)
         << ",\"allDevices\":"
         << (result.selected_devices.empty() || result.devices.empty()
                 ? "true"
                 : "false")
         << ",\"devices\":[";
  for (std::size_t index = 0; index < result.devices.size(); ++index) {
    if (index) {
      stream << ',';
    }
    const auto& device = result.devices[index];
    stream << "{\"id\":" << JsonEscape(device) << ",\"selected\":"
           << (result.selected_devices.count(device) ? "true" : "false")
           << '}';
  }
  stream << "],\"points\":[";
  for (std::size_t index = 0; index < result.points.size(); ++index) {
    if (index) {
      stream << ',';
    }
    const auto& point = result.points[index];
    stream << "{\"key\":" << point.key << ",\"label\":"
           << JsonEscape(point.label) << ",\"period\":"
           << JsonEscape(point.period) << ",\"value\":" << point.value
           << ",\"current\":" << (point.current ? "true" : "false")
           << '}';
  }
  stream << "]}";
  return stream.str();
}

}  // namespace

bool StatsReport::BuildJson(const ReportQuery& query,
                            std::string& json) noexcept {
  try {
    const Date today = Today();
    Result result;
    result.granularity = query.granularity;
    result.anchor = query.anchor;
    result.selected_devices.insert(query.device_ids.begin(),
                                   query.device_ids.end());

    std::error_code error;
    const bool exists = std::filesystem::exists(database_path_, error);
    if (error ||
        (exists && !std::filesystem::is_regular_file(database_path_, error)) ||
        error) {
      result.available = false;
      DefinePeriods(result, DayKey(today.year, today.month, today.day), today);
      json = Serialize(result);
      return true;
    }
    if (!exists) {
      DefinePeriods(result, DayKey(today.year, today.month, today.day), today);
      json = Serialize(result);
      return true;
    }

    Database database(sqlite_);
    const std::string path = database_path_.u8string();
    if (!sqlite_.available() ||
        sqlite_.open_v2(path.c_str(), database.address(),
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      result.available = false;
      DefinePeriods(result, DayKey(today.year, today.month, today.day), today);
      json = Serialize(result);
      return true;
    }
    sqlite_.busy_timeout(database.get(), 100);

    sqlite3_int64 schema_version = 0;
    {
      Statement statement(sqlite_, database.get(), "PRAGMA user_version;");
      if (!statement.get() || sqlite_.step(statement.get()) != SQLITE_ROW) {
        result.available = false;
      } else {
        schema_version = sqlite_.column_int64(statement.get(), 0);
        result.available = schema_version == 2 || schema_version == 3;
      }
    }

    int earliest_day = DayKey(today.year, today.month, today.day);
    if (result.available) {
      Statement devices(
          sqlite_, database.get(),
          "SELECT DISTINCT device_id FROM daily_totals ORDER BY device_id;");
      if (!devices.get()) {
        result.available = false;
      } else {
        int step = SQLITE_ROW;
        while ((step = sqlite_.step(devices.get())) == SQLITE_ROW) {
          const unsigned char* value = sqlite_.column_text(devices.get(), 0);
          if (!value) {
            result.available = false;
            break;
          }
          result.devices.emplace_back(reinterpret_cast<const char*>(value));
        }
        if (step != SQLITE_DONE) {
          result.available = false;
        }
      }
    }

    if (result.available) {
      Statement earliest(sqlite_, database.get(),
                         "SELECT COALESCE(MIN(day),0) FROM daily_totals;");
      if (!earliest.get() || sqlite_.step(earliest.get()) != SQLITE_ROW) {
        result.available = false;
      } else {
        const sqlite3_int64 value = sqlite_.column_int64(earliest.get(), 0);
        if (value > 0 && value <= (std::numeric_limits<int>::max)()) {
          earliest_day = static_cast<int>(value);
        }
      }
    }

    if (result.available && !result.selected_devices.empty()) {
      std::set<std::string> available_devices;
      for (const auto& device : result.devices) {
        if (result.selected_devices.count(device)) {
          available_devices.insert(device);
        }
      }
      result.selected_devices.swap(available_devices);
    }

    DefinePeriods(result, earliest_day, today);
    if (!result.available || result.points.empty()) {
      json = Serialize(result);
      return true;
    }

    int first_day = 0;
    int last_day = 0;
    if (result.granularity == ReportGranularity::kDay) {
      const int year = result.anchor / 100;
      const int month = result.anchor % 100;
      first_day = DayKey(year, month, 1);
      last_day = result.points.back().key;
    } else if (result.granularity == ReportGranularity::kMonth) {
      first_day = DayKey(result.anchor, 1, 1);
      last_day = result.anchor == today.year
                     ? DayKey(today.year, today.month, today.day)
                     : DayKey(result.anchor, 12, 31);
    } else {
      first_day = DayKey(result.points.front().key, 1, 1);
      last_day = result.anchor == today.year
                     ? DayKey(today.year, today.month, today.day)
                     : DayKey(result.anchor, 12, 31);
    }

    Statement totals(
        sqlite_, database.get(),
        "SELECT device_id,day,han_characters,english_words "
        "FROM daily_totals WHERE day>=?1 AND day<=?2 ORDER BY day;");
    if (!totals.get() ||
        sqlite_.bind_int64(totals.get(), 1, first_day) != SQLITE_OK ||
        sqlite_.bind_int64(totals.get(), 2, last_day) != SQLITE_OK) {
      result.available = false;
      json = Serialize(result);
      return true;
    }

    std::map<int, std::uint64_t> values;
    int step = SQLITE_ROW;
    while ((step = sqlite_.step(totals.get())) == SQLITE_ROW) {
      const unsigned char* device_value = sqlite_.column_text(totals.get(), 0);
      const sqlite3_int64 day_value = sqlite_.column_int64(totals.get(), 1);
      const sqlite3_int64 han = sqlite_.column_int64(totals.get(), 2);
      const sqlite3_int64 english = sqlite_.column_int64(totals.get(), 3);
      if (!device_value || day_value <= 0 ||
          day_value > (std::numeric_limits<int>::max)() || han < 0 ||
          english < 0) {
        result.available = false;
        break;
      }
      const std::string device(reinterpret_cast<const char*>(device_value));
      if (!result.selected_devices.empty() &&
          !result.selected_devices.count(device)) {
        continue;
      }
      int key = static_cast<int>(day_value);
      if (result.granularity == ReportGranularity::kMonth) {
        key /= 100;
      } else if (result.granularity == ReportGranularity::kYear) {
        key /= 10000;
      }
      AddValue(values, key, static_cast<std::uint64_t>(han));
      AddValue(values, key, static_cast<std::uint64_t>(english));
    }
    if (step != SQLITE_DONE) {
      result.available = false;
    }
    if (result.available) {
      ApplyValues(result, values);
    }
    json = Serialize(result);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace weasel::stats
