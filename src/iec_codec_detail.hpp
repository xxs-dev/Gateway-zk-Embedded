#pragma once

// Internal helpers shared by the IEC 101/104 and IEC 103 codecs. Not a public header.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/iec_codec.hpp"

namespace edge_gateway {
namespace iec_detail {

inline std::uint32_t readUnsignedLe(const std::vector<std::uint8_t>& bytes, std::size_t offset, int width) {
    if (width < 0 || width > 4 || offset + static_cast<std::size_t>(width) > bytes.size()) {
        throw std::runtime_error("iec field out of range");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < width; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

inline int readLe(const std::vector<std::uint8_t>& bytes, std::size_t offset, int width) {
    const auto value = readUnsignedLe(bytes, offset, width);
    if (width < 4) {
        return static_cast<int>(value);
    }
    std::int32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline void appendLe(std::vector<std::uint8_t>& bytes, std::uint32_t value, int width) {
    for (int i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

inline std::int16_t readInt16Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto raw = static_cast<std::uint16_t>(readUnsignedLe(bytes, offset, 2));
    std::int16_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

inline std::int32_t readInt32Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto raw = readUnsignedLe(bytes, offset, 4);
    std::int32_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

inline float readFloatLe(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto raw = readUnsignedLe(bytes, offset, 4);
    float value = 0.0F;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

inline std::int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Time tags (CP56Time2a, CP24Time2a, IEC 103 CP32Time2a) are wall-clock times of the zone set by
// protocol.iec.timestampZone: "local" = the gateway's own time zone (default), "utc" = UTC.
struct TimeTagContext {
    std::int64_t nowMs = 0;  // resolves tags without a date (CP24, CP32)
    bool utc = false;
};

inline bool timeTagsUtc(const IecProtocolConfig& config) {
    return config.timestampZone == "utc";
}

inline TimeTagContext timeTagContext(const IecProtocolConfig& config, std::int64_t nowMs) {
    TimeTagContext context;
    context.nowMs = nowMs > 0 ? nowMs : systemNowMs();
    context.utc = timeTagsUtc(config);
    return context;
}

inline std::tm brokenDownTime(std::int64_t unixMs, bool utc) {
    const auto seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm value{};
#ifdef _WIN32
    if (utc) gmtime_s(&value, &seconds); else localtime_s(&value, &seconds);
#else
    if (utc) gmtime_r(&seconds, &value); else localtime_r(&seconds, &value);
#endif
    return value;
}

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
inline std::int64_t daysFromCivil(int year, int month, int day) {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yearOfEra = year - era * 400;
    const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + dayOfEra - 719468;
}

// Wall time of the time-tag zone -> Unix seconds; -1 when it cannot be represented.
// local: summerTime (SU of IEC 60870-5-4, 1/0) selects daylight-saving or standard time, which
// decides the repeated hour at the end of DST. When that combination does not exist for the
// gateway zone (zone without DST, or SU contradicting the date) the zone's own rule applies.
// utc: there is no summer time; SU is ignored.
inline std::int64_t wallTimeToUnixSeconds(std::tm value, bool utc, int summerTime) {
    if (utc) {
        return daysFromCivil(value.tm_year + 1900, value.tm_mon + 1, value.tm_mday) * 86400 +
            value.tm_hour * 3600 + value.tm_min * 60 + value.tm_sec;
    }
    if (summerTime >= 0) {
        std::tm attempt = value;
        attempt.tm_isdst = summerTime;
        const auto seconds = std::mktime(&attempt);
        if (seconds != static_cast<std::time_t>(-1)) {
            const auto check = brokenDownTime(static_cast<std::int64_t>(seconds) * 1000, false);
            if ((check.tm_isdst > 0) == (summerTime > 0) && check.tm_hour == value.tm_hour &&
                check.tm_min == value.tm_min && check.tm_mday == value.tm_mday) {
                return static_cast<std::int64_t>(seconds);
            }
        }
    }
    value.tm_isdst = -1;
    const auto seconds = std::mktime(&value);
    return seconds == static_cast<std::time_t>(-1) ? -1 : static_cast<std::int64_t>(seconds);
}

// SU is set when the local zone is in daylight-saving time; never for UTC.
inline void appendCp56Time2a(std::vector<std::uint8_t>& bytes, std::int64_t unixTimeMs, bool utc) {
    const auto tm = brokenDownTime(unixTimeMs, utc);
    const auto msOfMinute = static_cast<std::uint32_t>(tm.tm_sec * 1000 + static_cast<int>(((unixTimeMs % 1000) + 1000) % 1000));
    appendLe(bytes, msOfMinute, 2);
    bytes.push_back(static_cast<std::uint8_t>(tm.tm_min & 0x3F));
    bytes.push_back(static_cast<std::uint8_t>((tm.tm_hour & 0x1F) | (!utc && tm.tm_isdst > 0 ? 0x80 : 0x00)));
    bytes.push_back(static_cast<std::uint8_t>(((tm.tm_wday == 0 ? 7 : tm.tm_wday) << 5) | (tm.tm_mday & 0x1F)));
    bytes.push_back(static_cast<std::uint8_t>((tm.tm_mon + 1) & 0x0F));
    bytes.push_back(static_cast<std::uint8_t>((tm.tm_year % 100) & 0x7F));
}

// Returns 0 when the tag is flagged invalid (IV) or out of range.
inline std::int64_t decodeCp56Time2a(const std::vector<std::uint8_t>& bytes, std::size_t offset, bool utc) {
    if (offset + 7 > bytes.size()) {
        throw std::runtime_error("IEC CP56Time2a is truncated");
    }
    const int msOfMinute = readLe(bytes, offset, 2);
    const int minute = bytes[offset + 2] & 0x3F;
    const int hour = bytes[offset + 3] & 0x1F;
    const int day = bytes[offset + 4] & 0x1F;
    const int month = bytes[offset + 5] & 0x0F;
    if ((bytes[offset + 2] & 0x80U) != 0 || msOfMinute > 59999 || minute > 59 || hour > 23 ||
        day < 1 || month < 1 || month > 12) {
        return 0;
    }
    std::tm tmValue{};
    tmValue.tm_sec = msOfMinute / 1000;
    tmValue.tm_min = minute;
    tmValue.tm_hour = hour;
    tmValue.tm_mday = day;
    tmValue.tm_mon = month - 1;
    tmValue.tm_year = (bytes[offset + 6] & 0x7F) + 100;
    const int summerTime = (bytes[offset + 3] & 0x80U) != 0 ? 1 : 0;
    const auto seconds = wallTimeToUnixSeconds(tmValue, utc, summerTime);
    if (seconds < 0) {
        return 0;
    }
    return seconds * 1000 + (msOfMinute % 1000);
}

// CP24Time2a (ms + minute): the most recent instant not later than now (+1 min skew).
inline std::int64_t decodeCp24Time2a(const std::vector<std::uint8_t>& bytes, std::size_t offset, const TimeTagContext& context) {
    if (offset + 3 > bytes.size()) {
        throw std::runtime_error("IEC CP24Time2a is truncated");
    }
    const int msOfMinute = readLe(bytes, offset, 2);
    const int minute = bytes[offset + 2] & 0x3F;
    if ((bytes[offset + 2] & 0x80U) != 0 || msOfMinute > 59999 || minute > 59) {
        return 0;
    }
    const auto nowMs = context.nowMs;
    const auto wall = brokenDownTime(nowMs, context.utc);
    const auto startOfHour = nowMs - (static_cast<std::int64_t>(wall.tm_min) * 60000 +
        static_cast<std::int64_t>(wall.tm_sec) * 1000 + nowMs % 1000);
    auto candidate = startOfHour + static_cast<std::int64_t>(minute) * 60000 + msOfMinute;
    if (candidate > nowMs + 60000) {
        candidate -= 3600000;
    }
    return candidate;
}

// IEC 103 CP32Time2a (ms + minute + hour): resolved to the day, in the time-tag zone, closest to
// now. Its SU bit is not used: the day is taken from the current time, so only the repeated
// hour at the end of DST could be ambiguous.
inline std::int64_t decodeCp32Time2a(const std::vector<std::uint8_t>& bytes, std::size_t offset, const TimeTagContext& context) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("IEC CP32Time2a is truncated");
    }
    const int msOfMinute = readLe(bytes, offset, 2);
    const int minute = bytes[offset + 2] & 0x3F;
    const int hour = bytes[offset + 3] & 0x1F;
    if ((bytes[offset + 2] & 0x80U) != 0 || msOfMinute > 59999 || minute > 59 || hour > 23) {
        return 0;
    }
    const auto nowMs = context.nowMs;
    const auto wall = brokenDownTime(nowMs, context.utc);
    const auto startOfDay = nowMs - (static_cast<std::int64_t>(wall.tm_hour) * 3600000 +
        static_cast<std::int64_t>(wall.tm_min) * 60000 + static_cast<std::int64_t>(wall.tm_sec) * 1000 +
        nowMs % 1000);
    auto candidate = startOfDay + static_cast<std::int64_t>(hour) * 3600000 +
        static_cast<std::int64_t>(minute) * 60000 + msOfMinute;
    constexpr std::int64_t kHalfDay = 12LL * 3600000;
    if (candidate > nowMs + kHalfDay) {
        candidate -= 2 * kHalfDay;
    } else if (candidate < nowMs - kHalfDay) {
        candidate += 2 * kHalfDay;
    }
    return candidate;
}

inline void markBad(IecDataValue& value, const std::string& reason) {
    value.quality = 0;
    if (value.qualityMsg.empty()) {
        value.qualityMsg = reason;
    } else {
        value.qualityMsg += "; " + reason;
    }
}

// Quality descriptor bits shared by SIQ, DIQ, QDS, SEP and QDP: BL 0x10, SB 0x20, NT 0x40,
// IV 0x80. QDS additionally has OV 0x01 (overflow). The gateway quality model is binary, so
// every flag that makes the value not a current, valid measurement maps to quality 0 and the
// flags are named in qualityMsg.
inline std::string qualityFlagNames(std::uint8_t descriptor, bool hasOverflow) {
    std::string names;
    const auto add = [&names](const char* name) {
        if (!names.empty()) names += "|";
        names += name;
    };
    if ((descriptor & 0x80U) != 0) add("IV");
    if ((descriptor & 0x40U) != 0) add("NT");
    if ((descriptor & 0x20U) != 0) add("SB");
    if ((descriptor & 0x10U) != 0) add("BL");
    if (hasOverflow && (descriptor & 0x01U) != 0) add("OV");
    return names;
}

inline void applyQualityDescriptor(IecDataValue& value, std::uint8_t descriptor, bool hasOverflow) {
    const auto names = qualityFlagNames(descriptor, hasOverflow);
    if (!names.empty()) {
        markBad(value, "IEC quality " + names);
    }
}

}  // namespace iec_detail
}  // namespace edge_gateway
