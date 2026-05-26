#include "log_reader.h"

#include "log.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <unicode/regex.h>

#include <sys/sysinfo.h>
#include <syslog.h>
#include <systemd/sd-journal.h>
#include <wblib/exceptions.h>
#include <wblib/json_utils.h>
#include <wblib/mqtt.h>

using namespace WBMQTT;
using icu::RegexMatcher;
using icu::UnicodeString;

#define LOG(logger) ::logger.Log() << "[logs] "

namespace
{
    const auto DMESG_SERVICE = "dmesg";
    const uint32_t MAX_LOG_RECORDS = 100;

    std::string GetJsonTypeName(const Json::Value& value)
    {
        switch (value.type()) {
            case Json::nullValue:
                return "null";
            case Json::intValue:
            case Json::uintValue:
                return "integer";
            case Json::realValue:
                return "number";
            case Json::stringValue:
                return "string";
            case Json::booleanValue:
                return "boolean";
            case Json::arrayValue:
                return "array";
            case Json::objectValue:
                return "object";
        }
        return "unknown";
    }

    bool GetBoolParam(const Json::Value& value, const std::string& name, bool defaultValue)
    {
        if (value.isNull()) {
            return defaultValue;
        }
        if (!value.isBool()) {
            throw std::runtime_error("Invalid request parameter '" + name + "': expected boolean, got " +
                                     GetJsonTypeName(value));
        }
        return value.asBool();
    }

    std::string GetStringParam(const Json::Value& value, const std::string& name, const std::string& defaultValue)
    {
        if (value.isNull()) {
            return defaultValue;
        }
        if (!value.isString()) {
            throw std::runtime_error("Invalid request parameter '" + name + "': expected string, got " +
                                     GetJsonTypeName(value));
        }
        return value.asString();
    }

    uint32_t GetUIntParam(const Json::Value& value, const std::string& name, uint32_t defaultValue)
    {
        if (value.isNull()) {
            return defaultValue;
        }
        if (!value.isUInt()) {
            throw std::runtime_error("Invalid request parameter '" + name + "': expected unsigned integer, got " +
                                     GetJsonTypeName(value));
        }
        return value.asUInt();
    }

    int64_t GetInt64Param(const Json::Value& value, const std::string& name, int64_t defaultValue)
    {
        if (value.isNull()) {
            return defaultValue;
        }
        if (!value.isIntegral()) {
            throw std::runtime_error("Invalid request parameter '" + name + "': expected integer, got " +
                                     GetJsonTypeName(value));
        }
        if (value.isUInt64() && value.asUInt64() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("Invalid request parameter '" + name + "': integer is out of int64 range");
        }
        return value.asInt64();
    }

    std::string UnicodeToUtf8(const UnicodeString& value)
    {
        std::string result;
        value.toUTF8String(result);
        return result;
    }

    void SdThrowError(int res, const std::string& msg)
    {
        if (res < 0) {
            throw std::runtime_error(std::string(msg) + ": " + strerror(-res));
        }
    }

    std::vector<std::string> ExecCommand(const std::string& cmd)
    {
        auto deleter = [](FILE* fp) {
            if (fp)
                pclose(fp);
        };
        std::unique_ptr<FILE, decltype(deleter)> fd(popen(cmd.c_str(), "r"), deleter);
        if (!fd) {
            throw std::runtime_error("Cannot open pipe for '" + cmd + "'");
        }
        std::array<char, 256> buffer;
        std::string result;
        while (!feof(fd.get())) {
            auto bytes = fread(buffer.data(), 1, buffer.size(), fd.get());
            result.append(buffer.data(), bytes);
        }
        return StringSplit(result, '\n');
    }

    // Input string example:
    // -1 e932c72aeb0b44c6a093b94797460151 Tue 2021-04-06 07:35:01 UTC—Tue
    // 2021-04-06 07:44:15 UTC
    Json::Value GetBootRec(const std::string& str)
    {
        Json::Value res;
        std::istringstream ss(str);
        ss.exceptions(std::ios_base::failbit | std::ios_base::badbit);
        int bootId;
        ss >> bootId;
        std::string hash;
        ss >> hash;
        res["hash"] = hash;
        tm t = {};
        const auto* timeFormat = "%a %Y-%m-%d %H:%M:%S UTC";
        ss >> std::get_time(&t, timeFormat);
        res["start"] = Json::Value::Int64(mktime(&t));
        if (bootId != 0) {
            ss.seekg(3,
                     std::ios_base::cur); // pass '—' U+2014 (0xe2, 0x80, 0x94) EM DASH
            ss >> std::get_time(&t, timeFormat);
            res["end"] = Json::Value::Int64(mktime(&t));
        }
        return res;
    }

    Json::Value GetBoots()
    {
        Json::Value res;
        auto boots = ExecCommand("TZ=UTC journalctl --list-boots");
        std::reverse(boots.begin(), boots.end());
        for (const auto& boot: boots) {
            try {
                res.append(GetBootRec(boot));
            } catch (const std::exception& e) {
                LOG(Warn) << "Failed to parse boot string '" + boot + "'";
            }
        }
        return res;
    }

    Json::Value GetServices()
    {
        const char* servicePostfix = ".service";
        const auto servicePostfixLen = strlen(servicePostfix);
        Json::Value res(Json::arrayValue);
        auto services = ExecCommand("systemctl list-units --type=service --state=loaded --no-pager --plain");
        for (const auto& service: services) {
            auto pos = service.find(servicePostfix);
            if (pos != std::string::npos) {
                res.append(service.substr(0, pos + servicePostfixLen));
            }
        }
        res.append(DMESG_SERVICE);
        return res;
    }

    const char* GetData(sd_journal* j, const std::string& fieldName)
    {
        const char* d;
        size_t l;
        int r = sd_journal_get_data(j, fieldName.c_str(), (const void**)&d, &l);
        if (r == 0 && l > fieldName.size() + 1) {
            return d + fieldName.size() + 1;
        }
        return nullptr;
    }

    struct TLoadParams
    {
        bool Backward = true;
        std::string Service;
        uint32_t MaxEntries = MAX_LOG_RECORDS;
        std::chrono::microseconds From = std::chrono::microseconds::zero();
        std::string Cursor;
        std::string Boot;
        std::vector<int> Levels;
        UnicodeString Pattern;
        bool CaseSensitive = true;
        bool RegEx = false;
        std::unique_ptr<RegexMatcher> Matcher;
    };

    TLoadParams ParseLoadParams(const Json::Value& params)
    {
        if (!params.isObject()) {
            throw std::runtime_error("Invalid request: expected object, got " + GetJsonTypeName(params));
        }

        TLoadParams loadParams;
        loadParams.Service = GetStringParam(params["service"], "service", "");
        loadParams.MaxEntries = std::min(MAX_LOG_RECORDS, GetUIntParam(params["limit"], "limit", MAX_LOG_RECORDS));
        loadParams.Boot = GetStringParam(params["boot"], "boot", "");

        std::set<int> levels;
        if (!params["levels"].isNull()) {
            const auto& levelsParam = params["levels"];
            if (!levelsParam.isArray()) {
                throw std::runtime_error("Invalid request parameter 'levels': expected array, got " +
                                         GetJsonTypeName(levelsParam));
            }
            for (const auto& lv: levelsParam) {
                if (!lv.isInt()) {
                    throw std::runtime_error("Invalid request parameter 'levels': expected array of integers");
                }
                int l = lv.asInt();
                if (l >= LOG_EMERG && l <= LOG_DEBUG && levels.insert(l).second) {
                    loadParams.Levels.push_back(l);
                }
            }
        }

        if (!params["time"].isNull()) {
            loadParams.From = std::chrono::microseconds(GetInt64Param(params["time"], "time", 0) * 1000000);
        }

        if (!params["cursor"].isNull()) {
            const auto cursor = params["cursor"];
            if (!cursor.isObject()) {
                throw std::runtime_error("Invalid request parameter 'cursor': expected object, got " +
                                         GetJsonTypeName(cursor));
            }
            loadParams.Cursor = GetStringParam(cursor["id"], "cursor.id", "");
            auto direction = GetStringParam(cursor["direction"], "cursor.direction", "backward");
            if (direction != "backward" && direction != "forward") {
                throw std::runtime_error(
                    "Invalid request parameter 'cursor.direction': expected 'backward' or 'forward'");
            }
            loadParams.Backward = (direction == "backward");
        }

        loadParams.Pattern = UnicodeString::fromUTF8(GetStringParam(params["pattern"], "pattern", ""));
        loadParams.CaseSensitive = GetBoolParam(params["case-sensitive"], "case-sensitive", true);
        loadParams.RegEx = GetBoolParam(params["regex"], "regex", false);
        if (loadParams.RegEx && !loadParams.Pattern.isEmpty()) {
            UErrorCode status = U_ZERO_ERROR;
            loadParams.Matcher =
                std::make_unique<RegexMatcher>(loadParams.Pattern,
                                               UnicodeString(),
                                               (loadParams.CaseSensitive ? 0 : UREGEX_CASE_INSENSITIVE),
                                               status);
            if (U_FAILURE(status)) {
                throw std::runtime_error("Could not create a RegexMatcher object for pattern '" +
                                         UnicodeToUtf8(loadParams.Pattern) + "'");
            }
        }

        return loadParams;
    }

    void ApplyJournalFilter(sd_journal* j, const TLoadParams& params)
    {
        if (!params.Service.empty()) {
            SdThrowError(sd_journal_add_match(j, ("_SYSTEMD_UNIT=" + params.Service).c_str(), 0),
                         "Adding match failed");
        }

        if (!params.Boot.empty()) {
            SdThrowError(sd_journal_add_match(j, ("_BOOT_ID=" + params.Boot).c_str(), 0), "Adding match failed");
        }

        for (const auto level: params.Levels) {
            SdThrowError(sd_journal_add_match(j, ("PRIORITY=" + std::to_string(level)).c_str(), 0),
                         "Adding match failed");
        }
    }

    bool HasSubstring(const UnicodeString& msg, const UnicodeString& pattern, bool caseSensitive)
    {
        if (caseSensitive) {
            return (msg.indexOf(pattern) >= 0);
        }
        return (UnicodeString(msg).foldCase().indexOf(UnicodeString(pattern).foldCase()) >= 0);
    }

    bool MatchesRegex(RegexMatcher& matcher, const UnicodeString& msg)
    {
        UErrorCode status = U_ZERO_ERROR;
        matcher.reset(msg);
        bool ok = matcher.find(status);
        if (U_FAILURE(status)) {
            throw std::runtime_error("Error searching for pattern '" + UnicodeToUtf8(matcher.pattern().pattern()) +
                                     "'");
        }
        return ok;
    }

    Json::Value ParseDmesgLog(const std::string& line, std::chrono::system_clock::time_point bootTime)
    {
        Json::Value entry;
        size_t p = 0;
        if (line[0] == '[') {
            auto sec = strtod(line.c_str() + 1, nullptr);
            auto t = bootTime + std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(sec * 1000));
            entry["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
            p = line.find(']');
            p = (p == std::string::npos) ? 0 : p + 1;
            if (line[p] == ' ') {
                ++p;
            }
        }
        entry["msg"] = line.substr(p);
        return entry;
    }

    Json::Value GetDmesgLogs(const TLoadParams& params, std::chrono::system_clock::time_point bootTime)
    {
        Json::Value res(Json::arrayValue);

        auto logs = ExecCommand("dmesg --color=never --force-prefix");

        for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
            Json::Value entry(ParseDmesgLog(*it, bootTime));

            if (!params.Pattern.isEmpty()) {
                auto msg = UnicodeString::fromUTF8(entry["msg"].asString());
                if (params.RegEx) {
                    if (!MatchesRegex(*params.Matcher.get(), msg)) {
                        continue;
                    }
                } else {
                    if (!HasSubstring(msg, params.Pattern, params.CaseSensitive)) {
                        continue;
                    }
                }
            }

            res.append(entry);
        }
        return res;
    }

    // libwbmqtt1 log prefixes to syslog severity levels map
    const std::vector<std::pair<std::string, int>> LibWbMqttLogLevels = {{"ERROR:", LOG_ERR},
                                                                         {"WARNING:", LOG_WARNING},
                                                                         {"DEBUG:", LOG_DEBUG}};

    bool AddMsg(sd_journal* j,
                Json::Value& entry,
                const UnicodeString& pattern,
                bool caseSensitive,
                RegexMatcher* matcher)
    {
        const char* d = GetData(j, "MESSAGE");
        if (d == nullptr) {
            return false;
        }
        if (!pattern.isEmpty()) {
            auto msg = UnicodeString::fromUTF8(d);
            if (matcher != nullptr) {
                if (!MatchesRegex(*matcher, msg)) {
                    return false;
                }
            } else {
                if (!HasSubstring(msg, pattern, caseSensitive)) {
                    return false;
                }
            }
        }
        entry["msg"] = d;
        if (!entry.isMember("level")) {
            auto it = std::find_if(LibWbMqttLogLevels.begin(), LibWbMqttLogLevels.end(), [&](const auto& p) {
                return StringStartsWith(d, p.first);
            });
            if (it != LibWbMqttLogLevels.end()) {
                entry["level"] = it->second;
            }
        }
        return true;
    }

    void AddTimestamp(sd_journal* j, Json::Value& entry)
    {
        uint64_t ts;
        SdThrowError(sd_journal_get_realtime_usec(j, &ts), "Failed to read timestamp");
        // __REALTIME_TIMESTAMP is in microseconds, convert it to milliseconds
        entry["time"] = ts / 1000;
    }

    void AddPriority(sd_journal* j, Json::Value& entry)
    {
        const char* d = GetData(j, "PRIORITY");
        if (d == nullptr) {
            return;
        }
        auto level = atoi(d);
        // journald sets LOG_INFO priority for all unprefixed messages got fom
        // stderr/stdout They priority is set in ParseMsg according to a prefix.
        if (level != LOG_INFO && !entry.isMember("level")) {
            entry["level"] = level;
        }
    }

    void AddCursor(sd_journal* j, Json::Value& entry)
    {
        char* k = nullptr;
        SdThrowError(sd_journal_get_cursor(j, &k), "Failed to get cursor");
        entry["cursor"] = k;
        free(k);
    }

    void AddService(sd_journal* j, Json::Value& entry)
    {
        const char* d = GetData(j, "_SYSTEMD_UNIT");
        if (d == nullptr) {
            return;
        }
        std::string s(d);
        const std::string SERVICE_SUFFIX(".service");
        if (WBMQTT::StringHasSuffix(s, SERVICE_SUFFIX)) {
            entry["service"] = s.substr(0, s.length() - SERVICE_SUFFIX.length());
        } else {
            entry["service"] = s;
        }
    }

    Json::Value MakeJouralctlRequest(const TLoadParams& params, std::atomic_bool& cancelLoading)
    {
        Json::Value res(Json::arrayValue);
        sd_journal* j = nullptr;
        SdThrowError(sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY), "Failed to open journal");
        std::unique_ptr<sd_journal, decltype(&sd_journal_close)> journalPtr(j, &sd_journal_close);

        ApplyJournalFilter(j, params);

        auto moveFn = params.Backward ? sd_journal_previous : sd_journal_next;
        if (!params.Cursor.empty()) {
            SdThrowError(sd_journal_seek_cursor(j, params.Cursor.c_str()), "Failed to seek to tail of journal");
            moveFn(j); // Pass pointed by cursor record
        } else if (params.From.count() > 0) {
            SdThrowError(sd_journal_seek_realtime_usec(j, params.From.count()), "Failed to seek to tail of journal");
        } else {
            SdThrowError(sd_journal_seek_tail(j), "Failed to seek to tail of journal");
        }

        int r = moveFn(j);
        auto remainingEntries = params.MaxEntries;
        while (r > 0 && remainingEntries && !cancelLoading) {
            Json::Value item;
            if (AddMsg(j, item, params.Pattern, params.CaseSensitive, params.Matcher.get())) {
                AddTimestamp(j, item);
                AddCursor(j, item);
                AddPriority(j, item);
                if (params.Service.empty()) {
                    AddService(j, item);
                }
                res.append(item);
                --remainingEntries;
            }
            r = moveFn(j);
        }

        if (r < 0) {
            LOG(Error) << "Failed to get next journal entry: " << strerror(-r);
        }

        // Forward queries return rows in ascending order, but we want a descending
        // order
        if (!params.Backward) {
            std::reverse(res.begin(), res.end());
        }
        return res;
    }

    Json::Value GetJouralctlLogs(const TLoadParams& params, std::atomic_bool& cancelLoading)
    {
        Json::Value res(MakeJouralctlRequest(params, cancelLoading));
        if (res.size() > 2) {
            // cursor is needed only for the first and the last record
            std::for_each(++res.begin(), --res.end(), [](auto& item) { item.removeMember("cursor"); });
        }
        return res;
    }

    Json::Value GetLogs(const TLoadParams& params,
                        std::atomic_bool& cancelLoading,
                        std::chrono::system_clock::time_point bootTime)
    {
        if (params.Service == DMESG_SERVICE) {
            return GetDmesgLogs(params, bootTime);
        }
        return GetJouralctlLogs(params, cancelLoading);
    }

    std::chrono::system_clock::time_point GetBootTime()
    {
        auto time = std::chrono::system_clock::now();
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            time -= std::chrono::seconds(si.uptime);
        }
        return time;
    }
} // namespace

TMQTTJournaldGateway::TMQTTJournaldGateway(PMqttClient mqttClient,
                                           PMqttRpcServer requestsRpcServer,
                                           PMqttRpcServer cancelRequestsRpcServer)
    : MqttClient(mqttClient),
      RequestsRpcServer(requestsRpcServer),
      CancelRequestsRpcServer(cancelRequestsRpcServer),
      Boots(GetBoots()),
      CancelLoading(false),
      BootTime(GetBootTime())
{
    RequestsRpcServer->RegisterMethod("logs",
                                      "List",
                                      std::bind(&TMQTTJournaldGateway::List, this, std::placeholders::_1));
    RequestsRpcServer->RegisterMethod("logs",
                                      "Load",
                                      std::bind(&TMQTTJournaldGateway::Load, this, std::placeholders::_1));
    CancelRequestsRpcServer->RegisterMethod("logs",
                                            "CancelLoad",
                                            std::bind(&TMQTTJournaldGateway::CancelLoad, this, std::placeholders::_1));
}

TMQTTJournaldGateway::~TMQTTJournaldGateway()
{
    CancelLoading = true;
}

Json::Value TMQTTJournaldGateway::List(const Json::Value& /*params*/)
{
    LOG(Debug) << "Run RPC List()";
    try {
        Json::Value res;
        res["boots"] = Boots;
        res["services"] = GetServices();
        return res;
    } catch (const std::exception& e) {
        LOG(Error) << e.what();
        throw;
    }
}

Json::Value TMQTTJournaldGateway::Load(const Json::Value& params)
{
    LOG(Debug) << "Run RPC Load()";
    CancelLoading = false;

    TLoadParams loadParams;
    try {
        loadParams = ParseLoadParams(params);
    } catch (const std::exception& e) {
        LOG(Debug) << e.what();
        throw;
    }

    try {
        return GetLogs(loadParams, CancelLoading, BootTime);
    } catch (const std::exception& e) {
        LOG(Error) << e.what();
        throw;
    }
}

Json::Value TMQTTJournaldGateway::CancelLoad(const Json::Value& /*params*/)
{
    LOG(Debug) << "Run RPC CancelLoad()";
    CancelLoading = true;
    return Json::Value();
}
