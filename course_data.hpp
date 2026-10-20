#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "vendor/json.hpp"

namespace courses {
inline constexpr int CacheSeconds = 300;
using Json = nlohmann::json;

struct Course {
    std::string uid, title, location, teacher;
    int64_t start = 0, end = 0;
};

inline int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string trim(std::string s) {
    auto space = [](unsigned char c) { return std::isspace(c); };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
    return s;
}

inline std::string singleLine(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
    return trim(s);
}

inline std::string unescape(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            result += c == 'n' || c == 'N' ? '\n' : c;
        } else
            result += s[i];
    }
    return result;
}

inline int number(const std::string& s, size_t pos, size_t count) {
    int value = 0;
    for (size_t i = pos; i < pos + count; ++i) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9')
            throw std::runtime_error("日历时间格式无效");
        value = value * 10 + s[i] - '0';
    }
    return value;
}

// 将墙上时间转换为 Unix 秒，完全不依赖 Windows 当前时区。
inline int64_t parseTime(const std::string& s, const std::string& params = "") {
    if ((s.size() != 15 && s.size() != 16) || s[8] != 'T' || (s.size() == 16 && s[15] != 'Z'))
        throw std::runtime_error("仅支持课程的日期时间事件");
    int year = number(s, 0, 4), month = number(s, 4, 2), day = number(s, 6, 2);
    int hour = number(s, 9, 2), minute = number(s, 11, 2), second = number(s, 13, 2);
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > lengths[month - 1] + (month == 2 && leap) || hour > 23 || minute > 59 || second > 59)
        throw std::runtime_error("课程日期时间超出范围");
    int offset = s.size() == 16 ? 0 : 8 * 3600;
    auto tz = params.find("TZID=");
    if (tz != std::string::npos && s.size() != 16) {
        auto name = params.substr(tz + 5);
        name = name.substr(0, name.find(';'));
        name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
        if (name == "UTC" || name == "Etc/UTC")
            offset = 0;
        else if (name != "Asia/Shanghai" && name != "Asia/Chongqing")
            throw std::runtime_error("订阅使用了不支持的时区：" + name);
    }
    int64_t days = 0;
    for (int y = 1970; y < year; ++y)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int m = 1; m < month; ++m)
        days += lengths[m - 1] + (m == 2 && leap);
    days += day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second - offset;
}

struct Property {
    std::string params, value;
};
inline std::vector<Course> parseIcs(const std::string& raw) {
    std::vector<std::string> lines;
    std::istringstream input(raw);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
            lines.back() += line.substr(1);
        else
            lines.push_back(line);
    }
    if (raw.find("BEGIN:VCALENDAR") == std::string::npos ||
        raw.find("END:VCALENDAR") == std::string::npos)
        throw std::runtime_error("订阅返回的不是完整 ICS 日历");
    std::map<std::string, Property> properties;
    std::map<std::string, Course> unique;
    bool inside = false, cancelled = false, recurring = false, isCourse = false;
    auto finish = [&] {
        if (cancelled || !isCourse)
            return;
        if (recurring)
            throw std::runtime_error("订阅含重复规则，需要使用服务端已展开的日历");
        auto start = properties.find("DTSTART"), end = properties.find("DTEND");
        if (start == properties.end() || end == properties.end())
            return;
        if (start->second.value.size() == 8)
            return; // 全天事件不是课程倒计时。
        Course course;
        course.start = parseTime(start->second.value, start->second.params);
        course.end = parseTime(end->second.value, end->second.params);
        if (course.end <= course.start)
            throw std::runtime_error("课程结束时间无效");
        course.uid = unescape(properties["UID"].value);
        course.title = singleLine(unescape(properties["SUMMARY"].value));
        if (course.title.empty())
            course.title = "未命名课程";
        course.location = singleLine(unescape(properties["LOCATION"].value));
        std::istringstream description(unescape(properties["DESCRIPTION"].value));
        std::string detail;
        while (std::getline(description, detail)) {
            auto colon = detail.find("：");
            if (colon == std::string::npos)
                continue;
            auto key = trim(detail.substr(0, colon));
            auto value = singleLine(detail.substr(colon + std::string("：").size()));
            if (key == "教师")
                course.teacher = value;
            if (key == "地点" && course.location.empty())
                course.location = value;
        }
        if (course.location.empty())
            course.location = "待定";
        auto key = course.uid.empty()
                       ? std::to_string(course.start) + course.title + course.location
                       : course.uid;
        unique[key] = std::move(course);
    };
    for (const auto& text : lines) {
        if (text == "BEGIN:VEVENT") {
            inside = true;
            cancelled = recurring = isCourse = false;
            properties.clear();
        } else if (text == "END:VEVENT" && inside) {
            finish();
            inside = false;
        } else if (inside) {
            auto colon = text.find(':');
            if (colon == std::string::npos)
                continue;
            auto field = text.substr(0, colon), value = text.substr(colon + 1);
            auto semi = field.find(';');
            auto key = field.substr(0, semi);
            if (key == "CATEGORIES") {
                std::istringstream categories(value);
                std::string category;
                while (std::getline(categories, category, ','))
                    if (trim(category) == "课程")
                        isCourse = true;
            }
            if (key == "STATUS" && value == "CANCELLED")
                cancelled = true;
            if (key == "RRULE" || key == "RDATE" || key == "RECURRENCE-ID")
                recurring = true;
            properties[key] = {semi == std::string::npos ? "" : field.substr(semi + 1), value};
        }
    }
    if (inside)
        throw std::runtime_error("ICS 事件不完整");
    std::vector<Course> result;
    for (auto& pair : unique)
        result.push_back(std::move(pair.second));
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    return result;
}

inline Json makeCache(const std::vector<Course>& list, int64_t fetched, const std::string& source) {
    Json events = Json::array();
    for (const auto& c : list)
        events.push_back({{"uid", c.uid},
                          {"title", c.title},
                          {"location", c.location},
                          {"teacher", c.teacher},
                          {"start", c.start},
                          {"end", c.end}});
    return {{"version", 1},
            {"source", source},
            {"fetched_at", fetched},
            {"expires_at", fetched + CacheSeconds},
            {"courses", events}};
}

inline std::vector<Course> readCache(const Json& cache, const std::string& source) {
    if (cache.at("version") != 1 || cache.at("source") != source)
        throw std::runtime_error("缓存版本或订阅地址不匹配");
    std::vector<Course> list;
    for (const auto& event : cache.at("courses")) {
        Course c{event.at("uid"),     event.at("title"), event.at("location"),
                 event.at("teacher"), event.at("start"), event.at("end")};
        if (c.end <= c.start)
            throw std::runtime_error("缓存课程时间无效");
        list.push_back(std::move(c));
    }
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.start < b.start; });
    return list;
}

struct Display {
    std::string first, title, subtitle;
};
inline Display display(const std::vector<Course>& list, int64_t at) {
    for (const auto& c : list) {
        if (c.end <= at)
            continue;
        bool ongoing = c.start <= at;
        int64_t remaining = (ongoing ? c.end : c.start) - at;
        char timer[48];
        if (remaining >= 3600)
            std::snprintf(timer, sizeof(timer), "%lld:%02lld:%02lld", (long long)(remaining / 3600),
                          (long long)(remaining / 60 % 60), (long long)(remaining % 60));
        else
            std::snprintf(timer, sizeof(timer), "%lld:%02lld", (long long)(remaining / 60),
                          (long long)(remaining % 60));
        return {std::string(ongoing ? "离下课 " : "离上课 ") + timer, c.title,
                c.location + (c.teacher.empty() ? "" : " · " + c.teacher)};
    }
    return {"近期无课程", "", ""};
}
} // namespace courses
