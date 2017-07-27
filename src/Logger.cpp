/*
 * This file is part of the bitcoin-classic project
 * Copyright (C) 2017 Tom Zander <tomz@freedommail.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Logger.h"
#include "LogChannels_p.h"
#include "util.h"
#include "chainparamsbase.h"

#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#include <boost/thread.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string.hpp> // for to_lower() / split()
#include <boost/filesystem/fstream.hpp>

bool fDebug = false;

class Log::ManagerPrivate {
public:
    std::list<Channel*> channels;
    std::mutex lock;
    std::string lastTime, lastDateTime;
    std::map<short, std::string> sectionNames;
    std::map<std::string, short> categoryMapping;
    std::map<short, short> enabledSections;
};

Log::Manager::Manager()
    : d(new ManagerPrivate())
{
    d->sectionNames.emplace(Log::Validation, "Validation");
    d->sectionNames.emplace(Log::Bench, "Bench");
    d->sectionNames.emplace(Log::Prune, "Prune");
    d->sectionNames.emplace(Log::Net, "Net");
    d->sectionNames.emplace(Log::Addrman, "Addrman");
    d->sectionNames.emplace(Log::Proxy, "Proxy");
    d->sectionNames.emplace(Log::NWM, "NWM");
    d->sectionNames.emplace(Log::Tor, "Tor");
    d->sectionNames.emplace(Log::AdminServer, "AdminServer");
    d->sectionNames.emplace(Log::RPC, "RPC");
    d->sectionNames.emplace(Log::HTTP, "HTTP");
    d->sectionNames.emplace(Log::ZMQ, "ZMQ");
    d->sectionNames.emplace(Log::DB, "DB");
    d->sectionNames.emplace(Log::Coindb, "Coindb");
    d->sectionNames.emplace(Log::Wallet, "Wallet");
    d->sectionNames.emplace(Log::SelectCoins, "SelectCoins");
    d->sectionNames.emplace(Log::Internals, "Internals");
    d->sectionNames.emplace(Log::Mempool, "Mempool");
    d->sectionNames.emplace(Log::Random, "Random");
    d->sectionNames.emplace(8002, "UAHF");

    // this is purely to be backwards compatible with the old style where the section was a string.
    d->categoryMapping.emplace("bench", Log::Bench);
    d->categoryMapping.emplace("addrman", Log::Addrman);
    d->categoryMapping.emplace("blk", Log::ExpeditedBlocks);
    d->categoryMapping.emplace("coindb", Log::Coindb);
    d->categoryMapping.emplace("db", Log::DB);
    d->categoryMapping.emplace("estimatefee", 502);
    d->categoryMapping.emplace("http", Log::HTTP);
    d->categoryMapping.emplace("libevent", Log::LibEvent);
    d->categoryMapping.emplace("mempool", Log::Mempool);
    d->categoryMapping.emplace("mempoolrej", Log::MempoolRej);
    d->categoryMapping.emplace("net", Log::Net);
    d->categoryMapping.emplace("partitioncheck", Global);
    d->categoryMapping.emplace("proxy", Log::Proxy);
    d->categoryMapping.emplace("prune", Log::Prune);
    d->categoryMapping.emplace("rand", Log::Random);
    d->categoryMapping.emplace("rpc", Log::RPC);
    d->categoryMapping.emplace("selectcoins", Log::SelectCoins);
    d->categoryMapping.emplace("thin", Log::ThinBlocks);
    d->categoryMapping.emplace("tor", Log::Tor);
    d->categoryMapping.emplace("zmq", Log::ZMQ);
    d->categoryMapping.emplace("reindex", 604);

    if (AreBaseParamsConfigured()) // allow running outside of Bitcoin as an executable.
        parseConfig();
}

Log::Manager::~Manager()
{
    clearChannels();
    delete d;
}

bool Log::Manager::isEnabled(short section, Verbosity verbosity) const
{
    auto iter = d->enabledSections.find(section);
    if (iter != d->enabledSections.end())
        return iter->second <= verbosity;
    const short region = section - (section % 1000);
    iter = d->enabledSections.find(region);
    if (iter != d->enabledSections.end())
        return iter->second <= verbosity;
    return false;
}

short Log::Manager::section(const char *category)
{
    if (!category)
        return Log::Global;
    auto iter = d->categoryMapping.find(category);
    assert (iter != d->categoryMapping.end());
    return iter->second;
}

Log::Manager *Log::Manager::instance()
{
    static Log::Manager s_instance;
    return &s_instance;
}

void Log::Manager::log(Log::Item *item)
{
    bool logTimestamps = GetBoolArg("-logtimestamps", DEFAULT_LOGTIMESTAMPS);
    const int64_t timeMillis = GetTimeMillis();
    std::string newTime;
    std::string newDateTime;
    std::lock_guard<std::mutex> lock(d->lock);
    for (auto channel : d->channels) {
        std::string *timeStamp = nullptr;
        if (logTimestamps) {
            switch (channel->timeStampFormat()) {
            case Channel::NoTime: break;
            case Channel::DateTime:
                if (newDateTime.empty()) {
                    newDateTime = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", timeMillis/1000);
                    if (channel->showSubSecondPrecision() && newDateTime == d->lastDateTime) {
                        std::ostringstream millis;
                        millis << std::setw(3) << timeMillis % 1000;
                        newDateTime = "               ." + millis.str();
                    } else {
                        d->lastDateTime = newDateTime;
                    }
                }
                timeStamp = &newDateTime;
                break;
            case Channel::TimeOnly:
                if (newTime.empty()) {
                    newTime = DateTimeStrFormat("%H:%M:%S", timeMillis/1000);
                    if (channel->showSubSecondPrecision() && newTime == d->lastTime) {
                        std::ostringstream millis;
                        millis << std::setw(3) << timeMillis % 1000;
                        newTime = "    ." + millis.str();
                    } else {
                        d->lastTime = newTime;
                    }
                }
                timeStamp = &newTime;
                break;
            }
        }
        try {
            channel->pushLog(timeMillis, timeStamp, item->d->stream.str(), item->d->filename, item->d->lineNum, item->d->methodName, item->d->section, item->d->verbosity);
        } catch (...) {}
    }
}

void Log::Manager::reopenLogFiles()
{
    std::lock_guard<std::mutex> lock(d->lock);
    for (auto channel : d->channels) {
        channel->reopenLogFiles();
    }
}

void Log::Manager::loadDefaultTestSetup()
{
    clearChannels();
    auto channel = new ConsoleLogChannel();
    channel->setPrintMethodName(true);
    channel->setTimeStampFormat(Channel::TimeOnly);
    channel->setPrintSection(true);
    d->channels.push_back(channel);

    d->enabledSections.clear();
    for (short i = 0; i <= 20000; i+=1000)
        d->enabledSections[i] = Log::DebugLevel;
}

void Log::Manager::parseConfig()
{
    std::lock_guard<std::mutex> lock(d->lock);
    d->enabledSections.clear();

    clearChannels();
    Log::Channel *channel = nullptr;

    bool loadedConsoleLog = false;
    // parse the config file on top of that.
    auto path = GetDataDir(false) / "logs.conf";
    if (boost::filesystem::exists(path)) {
        // default base level is Warning for all, unless changed in the file.
        for (short i = 0; i <= 20000; i+=1000)
            d->enabledSections[i] = Log::WarningLevel;

        boost::filesystem::ifstream is(path);
        std::string line;
        while (std::getline(is, line)) {
            boost::trim_left(line);
            if (line.empty() || line[0] == '#')
                continue;
            boost::to_lower(line);
            int comment = line.find('#');
            if (comment > 0) line = line.substr(0, comment);
            if (line.find("channel") == 0) {
                channel = nullptr;
                std::string type = line.substr(7);
                std::string cleaned = boost::trim_copy(type);
                if (type == cleaned) // we need some space between the channel and the type
                    continue;
                if (cleaned == "file") {
                    channel = new FileLogChannel();
                } else if (cleaned == "console") {
                    channel = new ConsoleLogChannel();
                    loadedConsoleLog = true;
                }
                if (channel)
                    d->channels.push_back(channel);
                continue;
            }
            if (line.find("option") == 0) {
                std::string type = line.substr(6);
                std::string cleaned = boost::trim_copy(type);
                if (type == cleaned) // we need some space between the option and the type
                    continue;
                if (channel && cleaned.find("linenumber") == 0) {
                    channel->setPrintLineNumber(InterpretBool(cleaned.substr(10)));
                } else if (channel && cleaned == "methodname") {
                    channel->setPrintMethodName(InterpretBool(cleaned.substr(10)));
                } else if (channel && cleaned == "filename") {
                    channel->setPrintFilename(InterpretBool(cleaned.substr(8)));
                } else if (channel && cleaned == "section") {
                    channel->setPrintSection(InterpretBool(cleaned.substr(7)));
                } else if (channel && cleaned.find("timestamp") == 0) {
                    cleaned = cleaned.substr(9);
                    std::vector<std::string> args;
                    boost::split(args, cleaned, boost::is_any_of(", \t"));
                    bool showDate = std::find(args.begin(), args.end(), "date") != args.end();
                    bool showTime = std::find(args.begin(), args.end(), "time") != args.end();
                    bool subSecond = std::find(args.begin(), args.end(), "millisecond") != args.end();

                    channel->setTimeStampFormat(showDate ? Channel::DateTime : (showTime ? Channel::TimeOnly : Channel::NoTime));
                    channel->setShowSubSecondPrecision(subSecond);
                }
                continue;
            }
            try {
                size_t offset;
                int section = std::stoi(line, &offset);
                std::string type = boost::trim_copy(line.substr(offset));
                short level = Log::CriticalLevel; // quiet is default
                if (type == "info")
                    level = WarningLevel;
                else if (type == "debug")
                    level = DebugLevel;
                else if (type == "silent")
                    level = FatalLevel;
                // else if (type != "quiet")
                d->enabledSections[section] = level;
            } catch (const std::exception &) {
                // unparsable line...
            }
        }
    } else {
        // default.
        d->channels.push_back(new FileLogChannel());
        d->enabledSections[0] = Log::WarningLevel;
        for (short i = 1000; i <= 20000; i+=1000)
            d->enabledSections[i] = Log::CriticalLevel;
    }

    // Parse the old fashioned way of enabling/disabling log sections.
    // Override settings from config file.
    // notice that with command-line args we can only do 'debug', no other levels.
    for (auto cat : mapMultiArgs["-debug"]) {
        if (cat.empty() || cat == "1") { // turns all on.
            for (short i = 0; i <= 20000; i+=1000)
                d->enabledSections[i] = Log::DebugLevel;
            break;
        }
        if (cat == "0") { // turns all off
            for (short i = 0; i <= 20000; i+=1000)
                d->enabledSections[i] = Log::CriticalLevel;
            break;
        }
        auto iter = d->categoryMapping.find(cat);
        if (iter == d->categoryMapping.end())
            continue; // silently ignore
        d->enabledSections[iter->second] = Log::DebugLevel;
    }

    if (!loadedConsoleLog && GetBoolArg("-printtoconsole", false))
        d->channels.push_back(new ConsoleLogChannel());
}

const std::string &Log::Manager::sectionString(short section)
{
    static std::string empty;
    ManagerPrivate *d = Log::Manager::instance()->d;
    auto result = d->sectionNames.find(section);
    if (result == d->sectionNames.end())
        return empty;
    return result->second;
}

void Log::Manager::clearChannels()
{
    for (auto channel : d->channels) {
        delete channel;
    }
    d->channels.clear();
}

/////////////////////////////////////////////////

Log::MessageLogger::MessageLogger()
    : m_line(0), m_file(0), m_method(0)
{
}

Log::MessageLogger::MessageLogger(const char *filename, int line, const char *function)
    : m_line(line), m_file(filename), m_method(function)
{
}


/////////////////////////////////////////////////

Log::Item::Item(const char *filename, int line, const char *function, short section, int verbosity)
    : d(new State(filename, line, function, section))
{
    d->space = true;
    d->on = Manager::instance()->isEnabled(section, static_cast<Log::Verbosity>(verbosity));
    d->verbosity = verbosity;
    d->ref = 1;
}

Log::Item::Item(int verbosity)
    : d(new State(nullptr, 0, nullptr, Log::Global))
{
    d->space = true;
    d->on = Manager::instance()->isEnabled(Log::Global,  static_cast<Log::Verbosity>(verbosity));
    d->verbosity = verbosity;
    d->ref = 1;
}

Log::Item::Item(const Log::Item &other)
    : d(other.d)
{
    d->ref++;
}

Log::Item::~Item()
{
    if (!--d->ref) {
        if (d->on)
            Log::Manager::instance()->log(this);
        delete d;
    }
}

short Log::Item::section() const
{
    return d->section;
}

Log::Item &Log::Item::operator<<(Log::StreamAlteration alteration)
{
    if (d->on) {
        switch (alteration) {
        case Log::Scientficic:
            d->stream << std::scientific;
            break;
        case Fixed:
            d->stream << std::fixed;
            break;
        case Hex:
            d->stream << std::hex;
            break;
        case Dec:
            d->stream << std::dec;
            break;
        case Oct:
            d->stream << std::oct;
            break;
        default:
            assert(false);
        }
    }

    return *this;
}

Log::Item operator<<(Log::Item item, const std::exception &ex) {
    if (item.isEnabled()) item << ex.what();
    // TODO configure if we want a stacktrace?
    return item.space();
}

Log::__Precision Log::precision(int amount)
{
    Log::__Precision answer { amount };
    return answer;
}
