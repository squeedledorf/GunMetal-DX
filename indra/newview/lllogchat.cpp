/**
 * @file lllogchat.cpp
 * @brief LLLogChat class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "llfloaterconversationpreview.h"
#include "llagent.h"
#include "llagentui.h"
#include "llavatarnamecache.h"
#include "lllogchat.h"
#include "llregex.h"
#include "lltrans.h"
#include "llurlaction.h"
#include "llurlentry.h"
#include "llviewercontrol.h"

#include "llapp.h"
#include "lldiriterator.h"
#include "llfloaterimsessiontab.h"
#include "llinstantmessage.h"
#include "llsingleton.h" // for LLSingleton
#include "workqueue.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/regex.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/local_time_adjustor.hpp>

const S32 LOG_RECALL_SIZE = 20480;

const std::string LL_IM_TIME("time");
const std::string LL_IM_DATE_TIME("datetime");
const std::string LL_IM_TEXT("message");
const std::string LL_IM_FROM("from");
const std::string LL_IM_FROM_ID("from_id");
const std::string LL_TRANSCRIPT_FILE_EXTENSION("txt");

const std::string GROUP_CHAT_SUFFIX(" (group)");

const static char IM_SYMBOL_SEPARATOR(':');
const static std::string IM_SEPARATOR(std::string() + IM_SYMBOL_SEPARATOR + " ");
const static std::string NEW_LINE("\n");
const static std::string NEW_LINE_SPACE_PREFIX("\n ");
const static std::string TWO_SPACES("  ");
const static std::string MULTI_LINE_PREFIX(" ");

/**
 *  Chat log lines - timestamp and name are optional but message text is mandatory.
 *
 *  Typical plain text chat log lines:
 *
 *  SuperCar: You aren't the owner
 *  [2:59]  SuperCar: You aren't the owner
 *  [2009/11/20 3:00]  SuperCar: You aren't the owner
 *  Katar Ivercourt is Offline
 *  [3:00]  Katar Ivercourt is Offline
 *  [2009/11/20 3:01]  Corba ProductEngine is Offline
 *
 * Note: "You" was used as an avatar names in viewers of previous versions
 */
const static boost::regex TIMESTAMP_AND_STUFF("^(\\[\\d{4}/\\d{1,2}/\\d{1,2}\\s+\\d{1,2}:\\d{2}\\]\\s+|\\[\\d{1,2}:\\d{2}\\]\\s+)?(.*)$");
const static boost::regex TIMESTAMP("^(\\[\\d{4}/\\d{1,2}/\\d{1,2}\\s+\\d{1,2}:\\d{2}\\]|\\[\\d{1,2}:\\d{2}\\]).*");
// <FS:Ansariel> Seconds in timestamps (FSSecondsinChatTimestamps)
// Unlike TIMESTAMP_AND_STUFF, the timestamp part of TIMESTAMP_AND_STUFF_SEC is NOT optional.
const static boost::regex TIMESTAMP_AND_STUFF_SEC("^(\\[\\d{4}/\\d{1,2}/\\d{1,2}\\s+\\d{1,2}:\\d{2}:\\d{2}\\]\\s+|\\[\\d{1,2}:\\d{2}:\\d{2}\\]\\s+)(.*)$");
const static boost::regex TIMESTAMP_AND_SEC("^(\\[\\d{4}/\\d{1,2}/\\d{1,2}\\s+\\d{1,2}:\\d{2}:\\d{2}\\]|\\[\\d{1,2}:\\d{2}:\\d{2}\\]).*");
// </FS:Ansariel>

/**
 *  Regular expression suitable to match names like
 *  "You", "Second Life", "Igor ProductEngine", "Object", "Mega House"
 */
const static boost::regex NAME_AND_TEXT("([^:]+[:]{1})?(\\s*)(.*)");

/**
 * These are recognizers for matching the names of ad-hoc conferences when generating the log file name
 * On invited side, an ad-hoc is named like "<first name> <last name> Conference 2010/11/19 03:43 f0f4"
 * On initiating side, an ad-hoc is named like Ad-hoc Conference hash<hash>"
 * If the naming system for ad-hoc conferences are change in LLIMModel::LLIMSession::buildHistoryFileName()
 * then these definition need to be adjusted as well.
 */
const static boost::regex INBOUND_CONFERENCE("^[a-zA-Z]{1,31} [a-zA-Z]{1,31} Conference [0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2} [0-9a-f]{4}");
const static boost::regex OUTBOUND_CONFERENCE("^Ad-hoc Conference hash[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");

//is used to parse complex object names like "Xstreet SL Terminal v2.2.5 st"
const static std::string NAME_TEXT_DIVIDER(": ");

// is used for timestamps adjusting
const static char* DATE_FORMAT("%Y/%m/%d %H:%M");
const static char* TIME_FORMAT("%H:%M");

const static int IDX_TIMESTAMP = 1;
const static int IDX_STUFF = 2;
const static int IDX_NAME = 1;
const static int IDX_TEXT = 3;

using namespace boost::posix_time;
using namespace boost::gregorian;

void append_to_last_message(std::list<LLSD>& messages, const std::string& line)
{
    if (!messages.size()) return;

    std::string im_text = messages.back()[LL_IM_TEXT].asString();
    im_text.append(line);
    messages.back()[LL_IM_TEXT] = im_text;
}

const char* remove_utf8_bom(const char* buf)
{
    const char* start = buf;
    if (start[0] == (char)0xEF && start[1] == (char)0xBB && start[2] == (char)0xBF)
    {   // If string starts with the magic bytes, return pointer after it.
        start += 3;
    }
    return start;
}

class LLLogChatTimeScanner: public LLSingleton<LLLogChatTimeScanner>
{
    LLSINGLETON(LLLogChatTimeScanner);

public:
    date getTodayPacificDate()
    {
        typedef boost::date_time::local_adjustor<ptime, -8, no_dst> pst;
        typedef boost::date_time::local_adjustor<ptime, -7, no_dst> pdt;
        time_t t_time = time(NULL);
        ptime p_time = LLStringOps::getPacificDaylightTime()
            ? pdt::utc_to_local(from_time_t(t_time))
            : pst::utc_to_local(from_time_t(t_time));
        struct tm s_tm = to_tm(p_time);
        return date_from_tm(s_tm);
    }

    void checkAndCutOffDate(std::string& time_str)
    {
        // Cuts off the "%Y/%m/%d" from string for todays timestamps.
        // Assume that passed string has at least "%H:%M" time format.
        date log_date(not_a_date_time);
        date today(getTodayPacificDate());

        // Parse the passed date
        mDateStream.str(LLStringUtil::null);
        mDateStream << time_str;
        mDateStream >> log_date;
        mDateStream.clear();

        days zero_days(0);
        days days_alive = today - log_date;

        if ( days_alive == zero_days )
        {
            // Yep, today's so strip "%Y/%m/%d" info
            ptime stripped_time(not_a_date_time);

            mTimeStream.str(LLStringUtil::null);
            mTimeStream << time_str;
            mTimeStream >> stripped_time;
            mTimeStream.clear();

            time_str.clear();

            mTimeStream.str(LLStringUtil::null);
            mTimeStream << stripped_time;
            mTimeStream >> time_str;
            mTimeStream.clear();
        }

        LL_DEBUGS("LLChatLogParser")
            << " log_date: "
            << log_date
            << " today: "
            << today
            << " days alive: "
            << days_alive
            << " new time: "
            << time_str
            << LL_ENDL;
    }

    // <FS:Ansariel> Seconds in timestamps: "%Y/%m/%d %H:%M:%S" -> "%H:%M:%S" for today's lines
    void checkAndCutOffDateWithSec(std::string& time_str)
    {
        if (time_str.size() < 10) // not enough space for a date
        {
            return;
        }
        date log_date(not_a_date_time);
        date today(getTodayPacificDate());

        mDateStream.str(LLStringUtil::null);
        mDateStream << time_str;
        mDateStream >> log_date;
        mDateStream.clear();

        if (!log_date.is_not_a_date() && (today - log_date) == days(0))
        {
            size_t pos = time_str.find_first_of(' ');
            if (pos != std::string::npos)
            {
                time_str.erase(0, pos + 1);
                LLStringUtil::trim(time_str);
            }
        }
    }
    // </FS:Ansariel>


private:
    std::stringstream mDateStream;
    std::stringstream mTimeStream;
};

inline
LLLogChatTimeScanner::LLLogChatTimeScanner()
{
    // Note, date/time facets will be destroyed by string streams
    mDateStream.imbue(std::locale(mDateStream.getloc(), new date_input_facet(DATE_FORMAT)));
    mTimeStream.imbue(std::locale(mTimeStream.getloc(), new time_facet(TIME_FORMAT)));
    mTimeStream.imbue(std::locale(mTimeStream.getloc(), new time_input_facet(DATE_FORMAT)));
}

// Single ordered writer thread for chat log appends. Lines are formatted on the
// caller's thread; the thread only opens/appends/closes files, in enqueue order.
// It also runs the chat log reads (#371): a read queued after a line sees that line.
class LLChatLogWriter
{
public:
    LLChatLogWriter() : mThread([this] { run(); }) {}

    ~LLChatLogWriter()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQuit = true;
        }
        mWake.notify_all();
        mThread.join();
    }

    void enqueue(std::string&& path, std::string&& text)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQueue.push_back({ std::move(path), std::move(text), nullptr });
            ++mEnqueued;
        }
        mWake.notify_one();
    }

    // Jobs are short (a page of a log); they run between appends, in queue order.
    void enqueueJob(std::function<void()>&& job)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQueue.push_back({ std::string(), std::string(), std::move(job) });
            ++mEnqueued;
        }
        mWake.notify_one();
    }

    // Blocks until everything enqueued before the call is on disk.
    void flush()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        const U64 target = mEnqueued;
        mDone.wait(lock, [&] { return mWritten >= target; });
    }

private:
    struct Entry
    {
        std::string             mPath;
        std::string             mText;
        std::function<void()>   mJob;   // set: a read job instead of an append
    };

    void run()
    {
        std::deque<Entry> batch;
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;)
        {
            mWake.wait(lock, [this] { return mQuit || !mQueue.empty(); });
            if (mQueue.empty())
            {
                return; // quit requested and drained
            }
            batch.swap(mQueue);
            lock.unlock();

            llofstream file;
            std::string open_path;
            for (const auto& entry : batch)
            {
                if (entry.mJob)
                {
                    // the job must see every line queued before it
                    if (file.is_open())
                    {
                        file.close();
                    }
                    open_path.clear();
                    try
                    {
                        entry.mJob();
                    }
                    catch (const std::exception& e)
                    {
                        LL_WARNS() << "Chat log job threw: " << e.what() << LL_ENDL;
                    }
                    catch (...)
                    {
                        LL_WARNS() << "Chat log job threw a non-std exception" << LL_ENDL;
                    }
                    continue;
                }
                if (entry.mPath != open_path || !file.is_open())
                {
                    if (file.is_open())
                    {
                        file.close();
                    }
                    file.clear();
                    open_path = entry.mPath;
                    file.open(open_path.c_str(), std::ios_base::app);
                    if (!file.is_open())
                    {
                        LL_WARNS() << "Couldn't open chat history log! - " << open_path << LL_ENDL;
                        open_path.clear();
                        continue;
                    }
                }
                file << entry.mText;
            }
            if (file.is_open())
            {
                file.close();
            }
            const U64 n = batch.size();
            batch.clear();

            lock.lock();
            mWritten += n;
            mDone.notify_all();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mDone;
    std::deque<Entry> mQueue;
    U64 mEnqueued = 0;
    U64 mWritten = 0;
    bool mQuit = false;
    std::thread mThread;
};

static std::mutex sChatLogWriterMutex;
static std::unique_ptr<LLChatLogWriter> sChatLogWriter;

static LLChatLogWriter& chatLogWriter()
{
    std::lock_guard<std::mutex> lock(sChatLogWriterMutex);
    if (!sChatLogWriter)
    {
        sChatLogWriter = std::make_unique<LLChatLogWriter>();
    }
    return *sChatLogWriter;
}

//static
void LLLogChat::flushPendingWrites()
{
    LLChatLogWriter* writer = nullptr;
    {
        std::lock_guard<std::mutex> lock(sChatLogWriterMutex);
        writer = sChatLogWriter.get();
    }
    if (writer)
    {
        writer->flush();
    }
}

//static
void LLLogChat::queueLogJob(std::function<void()>&& job)
{
    chatLogWriter().enqueueJob(std::move(job));
}

LLLogChat::LLLogChat()
: mSaveHistorySignal(NULL) // only needed in preferences
{
    mHistoryThreadsMutex = new LLMutex();
}

LLLogChat::~LLLogChat()
{
    {
        std::lock_guard<std::mutex> lock(sChatLogWriterMutex);
        sChatLogWriter.reset(); // drains the queue and joins
    }
    delete mHistoryThreadsMutex;
    mHistoryThreadsMutex = NULL;

    if (mSaveHistorySignal)
    {
        mSaveHistorySignal->disconnect_all_slots();
        delete mSaveHistorySignal;
        mSaveHistorySignal = NULL;
    }
}


//static
std::string LLLogChat::makeLogFileName(std::string filename)
{
    /**
     * Testing for in bound and out bound ad-hoc file names
     * if it is then skip date stamping.
     **/

    // cheap literal prefilters so ordinary names never reach the regexes
    boost::match_results<std::string::const_iterator> matches;
    bool inboundConf = filename.find(" Conference ") != std::string::npos
        && ll_regex_match(filename, matches, INBOUND_CONFERENCE);
    bool outboundConf = filename.compare(0, 22, "Ad-hoc Conference hash") == 0
        && ll_regex_match(filename, matches, OUTBOUND_CONFERENCE);
    if (!(inboundConf || outboundConf))
    {
        if( gSavedPerAccountSettings.getBOOL("LogFileNamewithDate") )
        {
            time_t now;
            time(&now);
            char dbuffer[20];       /* Flawfinder: ignore */
            if (filename == "chat")
            {
                strftime(dbuffer, 20, "-%Y-%m-%d", localtime(&now));
            }
            else
            {
                strftime(dbuffer, 20, "-%Y-%m", localtime(&now));
            }
            filename += dbuffer;
        }
    }

    filename = cleanFileName(filename);
    filename = gDirUtilp->getExpandedFilename(LL_PATH_PER_ACCOUNT_CHAT_LOGS, filename);
    if (!filename.empty())
    {
        filename += '.' + LL_TRANSCRIPT_FILE_EXTENSION;
    }

    return filename;
}

//static
void LLLogChat::renameLogFile(const std::string& old_filename, const std::string& new_filename)
{
    flushPendingWrites();
    std::string new_name = cleanFileName(new_filename);
    std::string old_name = cleanFileName(old_filename);
    new_name = gDirUtilp->getExpandedFilename(LL_PATH_PER_ACCOUNT_CHAT_LOGS, new_name);
    old_name = gDirUtilp->getExpandedFilename(LL_PATH_PER_ACCOUNT_CHAT_LOGS, old_name);

    if (new_name.empty() || old_name.empty())
    {
        return;
    }

    new_name += '.' + LL_TRANSCRIPT_FILE_EXTENSION;
    old_name += '.' + LL_TRANSCRIPT_FILE_EXTENSION;

    if (!LLFile::isfile(new_name) && LLFile::isfile(old_name))
    {
        LLFile::rename(old_name, new_name);
    }
}

std::string LLLogChat::cleanFileName(std::string filename)
{
    std::string invalidChars = "\"\'\\/?*:.<>|[]{}~"; // Cannot match glob or illegal filename chars
    std::string::size_type position = filename.find_first_of(invalidChars);
    while (position != filename.npos)
    {
        filename[position] = '_';
        position = filename.find_first_of(invalidChars, position);
    }
    return filename;
}

std::string LLLogChat::timestamp2LogString(U32 timestamp, bool withdate, bool allow_seconds)
{
    std::string timeStr;
    if (withdate)
    {
        timeStr = "[" + LLTrans::getString ("TimeYear") + "]/["
                  + LLTrans::getString ("TimeMonth") + "]/["
                  + LLTrans::getString ("TimeDay") + "] ["
                  + LLTrans::getString ("TimeHour") + "]:["
                  + LLTrans::getString ("TimeMin") + "]";
    }
    else
    {
        timeStr = "[" + LLTrans::getString("TimeHour") + "]:["
                  + LLTrans::getString ("TimeMin")+"]";
    }
    // <FS> Seconds in timestamps
    static LLCachedControl<bool> seconds_in_timestamps(gSavedSettings, "FSSecondsinChatTimestamps", false);
    if (allow_seconds && seconds_in_timestamps)
    {
        timeStr += ":[" + LLTrans::getString("TimeSec") + "]";
    }
    // </FS>

    LLSD substitution;
    if (timestamp == 0)
    {
        substitution["datetime"] = (S32)time_corrected();
    }
    else
    {   // timestamp is correct utc already
        substitution["datetime"] = (S32)timestamp;
    }

    LLStringUtil::format (timeStr, substitution);
    return timeStr;
}


//static
void LLLogChat::saveHistory(const std::string& filename,
                            const std::string& from,
                            const LLUUID& from_id,
                            const std::string& line)
{
    LL_PROFILE_ZONE_SCOPED;
    std::string tmp_filename = filename;
    LLStringUtil::trim(tmp_filename);
    if (tmp_filename.empty())
    {
        std::string warn = "Chat history filename [" + filename + "] is empty!";
        LL_WARNS() << warn << LL_ENDL;
        llassert(tmp_filename.size());
        return;
    }

    std::string altered_line = line;

    // avoid costly regex calls
    if (line.find("/mention") != std::string::npos)
    {
        static const boost::regex mention_regex(APP_HEADER_REGEX "/agent/[\\da-f-]+/mention", boost::regex::perl | boost::regex::icase);

        // replace mention URL with [@username](URL)
        altered_line = boost::regex_replace(line, mention_regex, [](const boost::smatch& match) -> std::string
        {
            std::string url = match[0].str();
            std::string username = LLUrlAction::getURLLabel(url);
            return "[" + username + "](" + url + ")";
        });
    }

    LLSD item;

    if (gSavedPerAccountSettings.getBOOL("LogTimestamp"))
         item["time"] = LLLogChat::timestamp2LogString(0, gSavedPerAccountSettings.getBOOL("LogTimestampDate"));

    item["from_id"] = from_id;
    item["message"] = altered_line;

    //adding "Second Life:" for all system messages to make chat log history parsing more reliable
    if (from.empty() && from_id.isNull())
    {
        item["from"] = SYSTEM_FROM;
    }
    else
    {
        item["from"] = from;
    }

    std::ostringstream out;
    out << LLChatLogFormatter(item) << '\n';
    chatLogWriter().enqueue(LLLogChat::makeLogFileName(filename), out.str());

    LLLogChat::getInstance()->triggerHistorySignal();
}

// static
void LLLogChat::loadChatHistory(const std::string& file_name, std::list<LLSD>& messages, const LLSD& load_params, bool is_group)
{
    if (file_name.empty())
    {
        LL_WARNS("LLLogChat::loadChatHistory") << "Local history file name is empty!" << LL_ENDL;
        return ;
    }

    flushPendingWrites();

    bool load_all_history = load_params.has("load_all_history") ? load_params["load_all_history"].asBoolean() : false;

    // Stat the file to find it and get the last history entry time
    llstat stat_data;

    std::string log_file_name = LLLogChat::makeLogFileName(file_name);
    LL_DEBUGS("ChatHistory") << "First attempt to stat chat history file " << log_file_name << LL_ENDL;

    S32 no_stat = LLFile::stat(log_file_name, &stat_data);

    if (no_stat)
    {
        if (is_group)
        {
            std::string old_name(file_name);
            old_name.erase(old_name.size() - GROUP_CHAT_SUFFIX.size());     // trim off " (group)"
            log_file_name = LLLogChat::makeLogFileName(old_name);
            LL_DEBUGS("ChatHistory") << "Attempting to stat adjusted chat history file " << log_file_name << LL_ENDL;
            no_stat = LLFile::stat(log_file_name, &stat_data);
            if (!no_stat)
            {   // Found it without "(group)", copy to new naming style.  We already have the mod time in stat_data
                log_file_name = LLLogChat::makeLogFileName(file_name);
                LL_DEBUGS("ChatHistory") << "Attempt to stat copied history file " << log_file_name << LL_ENDL;
                LLFile::copy(LLLogChat::makeLogFileName(old_name), log_file_name);
            }
        }
        if (no_stat)
        {
            log_file_name = LLLogChat::oldLogFileName(file_name);
            LL_DEBUGS("ChatHistory") << "Attempt to stat old history file name " << log_file_name << LL_ENDL;
            no_stat = LLFile::stat(log_file_name, &stat_data);
            if (no_stat)
            {
                LL_DEBUGS("ChatHistory") << "No previous conversation log file found for " << file_name << LL_ENDL;
                return;                     //No previous conversation with this name.
            }
        }
    }

    // If we got here, we managed to stat the file.
    // Open the file to read in binary mode to prevent interpreting other characters as EOF
    LLFILE* fptr = LLFile::fopen(log_file_name, "rb");       /*Flawfinder: ignore*/
    if (!fptr)
    {   // Ok, this is strange but not really tragic in the big picture of things
        LL_WARNS("ChatHistory") << "Unable to read file " << log_file_name << " after stat was successful" << LL_ENDL;
        return;
    }

    auto save_num_messages = messages.size();

    char buffer[LOG_RECALL_SIZE];       /*Flawfinder: ignore*/
    char *bptr;
    size_t len;
    bool firstline = true;

    if (load_all_history || fseek(fptr, (LOG_RECALL_SIZE - 1) * -1  , SEEK_END))
    {   //We need to load the whole historyFile or it's smaller than recall size, so get it all.
        firstline = false;
        if (fseek(fptr, 0, SEEK_SET))
        {
            fclose(fptr);
            return;
        }
    }
    while (fgets(buffer, LOG_RECALL_SIZE, fptr)  && !feof(fptr))
    {
        len = strlen(buffer) - 1;       /*Flawfinder: ignore*/
        // backfill any end of line characters with nulls
        for (bptr = (buffer + len); (*bptr == '\n' || *bptr == '\r') && bptr>buffer; bptr--)    *bptr='\0';

        if (firstline)
        {
            firstline = false;
            continue;
        }

        std::string line(remove_utf8_bom(buffer));


        // fast heuristic test for a mention URL in a string
        // this is used to avoid costly regex calls
        if (line.find("/mention)") != std::string::npos)
        {
            // restore original mention URL from [@username](URL) format
            static const boost::regex altered_mention_regex("\\[@([^\\]]+)\\]\\((" APP_HEADER_REGEX "/agent/[\\da-f-]+/mention)\\)",
                                                            boost::regex::perl | boost::regex::icase);

            // $2 captures the URL part
            line = boost::regex_replace(line, altered_mention_regex, "$2");
        }

        //updated 1.23 plain text log format requires a space added before subsequent lines in a multilined message
        if (' ' == line[0])
        {
            line.erase(0, MULTI_LINE_PREFIX.length());
            append_to_last_message(messages, '\n' + line);
        }
        else if (0 == len && ('\n' == line[0] || '\r' == line[0]))
        {
            //to support old format's multilined messages with new lines used to divide paragraphs
            append_to_last_message(messages, line);
        }
        else
        {
            LLSD item;
            if (!LLChatLogParser::parse(line, item, load_params))
            {
                item[LL_IM_TEXT] = line;
            }
            messages.push_back(item);
        }
    }
    fclose(fptr);

    LL_DEBUGS("ChatHistory") << "Read " << (messages.size() - save_num_messages)
        << " messages of chat history from " << log_file_name
        << " file mod time " << (F64)stat_data.st_mtime << LL_ENDL;
}

bool LLLogChat::historyThreadsFinished(LLUUID session_id)
{
    LLMutexLock lock(historyThreadsMutex());
    bool finished = true;
    std::map<LLUUID,LLLoadHistoryThread *>::iterator it = mLoadHistoryThreads.find(session_id);
    if (it != mLoadHistoryThreads.end())
    {
        finished = it->second->isFinished();
    }
    if (!finished)
    {
        return false;
    }
    std::map<LLUUID,LLDeleteHistoryThread *>::iterator dit = mDeleteHistoryThreads.find(session_id);
    if (dit != mDeleteHistoryThreads.end())
    {
        finished = finished && dit->second->isFinished();
    }
    return finished;
}

LLLoadHistoryThread* LLLogChat::getLoadHistoryThread(LLUUID session_id)
{
    LLMutexLock lock(historyThreadsMutex());
    std::map<LLUUID,LLLoadHistoryThread *>::iterator it = mLoadHistoryThreads.find(session_id);
    if (it != mLoadHistoryThreads.end())
    {
        return it->second;
    }
    return NULL;
}

LLDeleteHistoryThread* LLLogChat::getDeleteHistoryThread(LLUUID session_id)
{
    LLMutexLock lock(historyThreadsMutex());
    std::map<LLUUID,LLDeleteHistoryThread *>::iterator it = mDeleteHistoryThreads.find(session_id);
    if (it != mDeleteHistoryThreads.end())
    {
        return it->second;
    }
    return NULL;
}

bool LLLogChat::addLoadHistoryThread(LLUUID& session_id, LLLoadHistoryThread* lthread)
{
    LLMutexLock lock(historyThreadsMutex());
    std::map<LLUUID,LLLoadHistoryThread *>::const_iterator it = mLoadHistoryThreads.find(session_id);
    if (it != mLoadHistoryThreads.end())
    {
        return false;
    }
    mLoadHistoryThreads[session_id] = lthread;
    return true;
}

bool LLLogChat::addDeleteHistoryThread(LLUUID& session_id, LLDeleteHistoryThread* dthread)
{
    LLMutexLock lock(historyThreadsMutex());
    std::map<LLUUID,LLDeleteHistoryThread *>::const_iterator it = mDeleteHistoryThreads.find(session_id);
    if (it != mDeleteHistoryThreads.end())
    {
        return false;
    }
    mDeleteHistoryThreads[session_id] = dthread;
    return true;
}

void LLLogChat::cleanupHistoryThreads()
{
    LLMutexLock lock(historyThreadsMutex());
    std::vector<LLUUID> uuids;
    std::map<LLUUID,LLLoadHistoryThread *>::iterator lit = mLoadHistoryThreads.begin();
    for (; lit != mLoadHistoryThreads.end(); lit++)
    {
        if (lit->second->isFinished() && mDeleteHistoryThreads[lit->first]->isFinished())
        {
            delete lit->second;
            delete mDeleteHistoryThreads[lit->first];
            uuids.push_back(lit->first);
        }
    }
    std::vector<LLUUID>::iterator uuid_it = uuids.begin();
    for ( ;uuid_it != uuids.end(); uuid_it++)
    {
        mLoadHistoryThreads.erase(*uuid_it);
        mDeleteHistoryThreads.erase(*uuid_it);
    }
}

LLMutex* LLLogChat::historyThreadsMutex()
{
    return mHistoryThreadsMutex;
}

void LLLogChat::triggerHistorySignal()
{
    if (NULL != mSaveHistorySignal)
    {
        (*mSaveHistorySignal)();
    }
}

// static
std::string LLLogChat::oldLogFileName(std::string filename)
{
    // get Users log directory
    std::string directory = gDirUtilp->getPerAccountChatLogsDir();

    // add final OS dependent delimiter
    directory += gDirUtilp->getDirDelimiter();

    // lest make sure the file name has no invalid characters before making the pattern
    filename = cleanFileName(filename);

    // create search pattern
    std::string pattern = filename + ( filename == "chat" ? "-???\?-?\?-??.txt" : "-???\?-??.txt");

    std::vector<std::string> allfiles;
    LLDirIterator iter(directory, pattern);
    std::string scanResult;

    while (iter.next(scanResult))
    {
        allfiles.push_back(scanResult);
    }

    if (allfiles.size() == 0)  // if no result from date search, return generic filename
    {
        scanResult = directory + filename + '.' + LL_TRANSCRIPT_FILE_EXTENSION;
    }
    else
    {
        sort(allfiles.begin(), allfiles.end());
        scanResult = directory + allfiles.back();
        // this file is now the most recent version of the file.
    }

    return scanResult;
}

bool LLLogChat::transcriptFilesExist()
{
    std::string pattern = "*." + LL_TRANSCRIPT_FILE_EXTENSION;
    // get Users log directory
    std::string dirname = gDirUtilp->getPerAccountChatLogsDir();

    // add final OS dependent delimiter
    dirname += gDirUtilp->getDirDelimiter();

    LLDirIterator iter(dirname, pattern);
    std::string filename;
    while (iter.next(filename))
    {
        std::string fullname = gDirUtilp->add(dirname, filename);
        if (isTranscriptFileFound(fullname))
        {
            return true;
        }
    }
    return false;
}
// static
void LLLogChat::findTranscriptFiles(std::string pattern, std::vector<std::string>& list_of_transcriptions)
{
    // get Users log directory
    std::string dirname = gDirUtilp->getPerAccountChatLogsDir();

    // add final OS dependent delimiter
    dirname += gDirUtilp->getDirDelimiter();

    LLDirIterator iter(dirname, pattern);
    std::string filename;
    while (iter.next(filename))
    {
        std::string fullname = gDirUtilp->add(dirname, filename);
        if (isTranscriptFileFound(fullname))
        {
            list_of_transcriptions.push_back(fullname);
        }
    }
}

// static
void LLLogChat::getListOfTranscriptFiles(std::vector<std::string>& list_of_transcriptions)
{
    // create search pattern
    std::string pattern = "*." + LL_TRANSCRIPT_FILE_EXTENSION;
    findTranscriptFiles(pattern, list_of_transcriptions);
}

// static
void LLLogChat::getListOfTranscriptBackupFiles(std::vector<std::string>& list_of_transcriptions)
{
    // create search pattern
    std::string pattern = "*." + LL_TRANSCRIPT_FILE_EXTENSION + ".backup*";
    findTranscriptFiles(pattern, list_of_transcriptions);
}

boost::signals2::connection LLLogChat::setSaveHistorySignal(const save_history_signal_t::slot_type& cb)
{
    if (NULL == mSaveHistorySignal)
    {
        mSaveHistorySignal = new save_history_signal_t();
    }

    return mSaveHistorySignal->connect(cb);
}

//static
bool LLLogChat::moveTranscripts(const std::string originDirectory,
                                const std::string targetDirectory,
                                std::vector<std::string>& listOfFilesToMove,
                                std::vector<std::string>& listOfFilesMoved)
{
    flushPendingWrites();
    std::string newFullPath;
    bool movedAllTranscripts = true;
    std::string backupFileName;
    unsigned backupFileCount;

    for (const std::string& fullpath : listOfFilesToMove)
    {
        backupFileCount = 0;
        newFullPath = targetDirectory + fullpath.substr(originDirectory.length(), std::string::npos);

        //The target directory contains that file already, so lets store it
        if(LLFile::isfile(newFullPath))
        {
            backupFileName = newFullPath + ".backup";

            //If needed store backup file as .backup1 etc.
            while(LLFile::isfile(backupFileName))
            {
                ++backupFileCount;
                backupFileName = newFullPath + ".backup" + std::to_string(backupFileCount);
            }

            //Rename the file to its backup name so it is not overwritten
            LLFile::rename(newFullPath, backupFileName);
        }

        S32 retry_count = 0;
        while (retry_count < 5)
        {
            //success is zero
            if (LLFile::rename(fullpath, newFullPath) != 0)
            {
                retry_count++;
                S32 result = errno;
                LL_WARNS("LLLogChat::moveTranscripts") << "Problem renaming " << fullpath << " - errorcode: "
                    << result << " attempt " << retry_count << LL_ENDL;

                ms_sleep(100);
            }
            else
            {
                listOfFilesMoved.push_back(newFullPath);

                if (retry_count)
                {
                    LL_WARNS("LLLogChat::moveTranscripts") << "Successfully renamed " << fullpath << LL_ENDL;
                }
                break;
            }
        }
    }

    if(listOfFilesMoved.size() != listOfFilesToMove.size())
    {
        movedAllTranscripts = false;
    }

    return movedAllTranscripts;
}

//static
bool LLLogChat::moveTranscripts(const std::string currentDirectory,
    const std::string newDirectory,
    std::vector<std::string>& listOfFilesToMove)
{
    std::vector<std::string> listOfFilesMoved;
    return moveTranscripts(currentDirectory, newDirectory, listOfFilesToMove, listOfFilesMoved);
}

//static
void LLLogChat::deleteTranscripts()
{
    flushPendingWrites();
    std::vector<std::string> list_of_transcriptions;
    getListOfTranscriptFiles(list_of_transcriptions);
    getListOfTranscriptBackupFiles(list_of_transcriptions);

    for (const std::string& fullpath : list_of_transcriptions)
    {
        S32 retry_count = 0;
        while (retry_count < 5)
        {
            if (0 != LLFile::remove(fullpath))
            {
                retry_count++;
                S32 result = errno;
                LL_WARNS("LLLogChat::deleteTranscripts") << "Problem removing " << fullpath << " - errorcode: "
                    << result << " attempt " << retry_count << LL_ENDL;

                if(retry_count >= 5)
                {
                    LL_WARNS("LLLogChat::deleteTranscripts") << "Failed to remove " << fullpath << LL_ENDL;
                    return;
                }

                ms_sleep(100);
            }
            else
            {
                if (retry_count)
                {
                    LL_WARNS("LLLogChat::deleteTranscripts") << "Successfully removed " << fullpath << LL_ENDL;
                }
                break;
            }
        }
    }

    LLFloaterIMSessionTab::processChatHistoryStyleUpdate(true);
}

// static
bool LLLogChat::isTranscriptExist(const LLUUID& avatar_id, bool is_group)
{
    LLAvatarName avatar_name;
    LLAvatarNameCache::get(avatar_id, &avatar_name);
    std::string avatar_user_name = avatar_name.getAccountName();
    if(!is_group)
    {
        std::replace(avatar_user_name.begin(), avatar_user_name.end(), '.', '_');
        return isTranscriptFileFound(makeLogFileName(avatar_user_name));
    }
    else
    {
        std::string file_name;
        gCacheName->getGroupName(avatar_id, file_name);
        file_name = makeLogFileName(file_name + GROUP_CHAT_SUFFIX);
        return isTranscriptFileFound(file_name);
    }
    return false;
}

bool LLLogChat::isNearbyTranscriptExist()
{
    return isTranscriptFileFound(makeLogFileName("chat"));;
}

bool LLLogChat::isAdHocTranscriptExist(std::string file_name)
{
    return isTranscriptFileFound(makeLogFileName(file_name));;
}

// static
bool LLLogChat::isTranscriptFileFound(std::string fullname)
{
    flushPendingWrites();
    bool result = false;
    LLFILE * filep = LLFile::fopen(fullname, "rb");
    if (NULL != filep)
    {
        if (makeLogFileName("chat") == fullname)
        {
            LLFile::close(filep);
            return true;
        }
        char buffer[LOG_RECALL_SIZE];

        fseek(filep, 0, SEEK_END);          // seek to end of file
        S32 bytes_to_read = ftell(filep);   // get current file pointer
        fseek(filep, 0, SEEK_SET);          // seek back to beginning of file

        // limit the number characters to read from file
        if (bytes_to_read >= LOG_RECALL_SIZE)
        {
            bytes_to_read = LOG_RECALL_SIZE - 1;
        }

        if (bytes_to_read > 0 && NULL != fgets(buffer, bytes_to_read, filep))
        {
            //matching a timestamp
            boost::match_results<std::string::const_iterator> matches;
            std::string line(remove_utf8_bom(buffer));
            if (ll_regex_match(line, matches, TIMESTAMP) || ll_regex_match(line, matches, TIMESTAMP_AND_SEC)) // <FS:Ansariel> Seconds in timestamps
            {
                result = true;
            }
        }
        LLFile::close(filep);
    }
    return result;
}

std::string LLLogChat::getGroupChatSuffix()
{
    return GROUP_CHAT_SUFFIX;
}

//*TODO mark object's names in a special way so that they will be distinguishable form avatar name
//which are more strict by its nature (only firstname and secondname)
//Example, an object's name can be written like "Object <actual_object's_name>"
void LLChatLogFormatter::format(const LLSD& im, std::ostream& ostr) const
{
    if (!im.isMap())
    {
        LL_WARNS() << "invalid LLSD type of an instant message" << LL_ENDL;
        return;
    }

    if (im[LL_IM_TIME].isDefined())
    {
        std::string timestamp = im[LL_IM_TIME].asString();
        boost::trim(timestamp);
        ostr << '[' << timestamp << ']' << TWO_SPACES;
    }

    //*TODO mark object's names in a special way so that they will be distinguishable from avatar name
    //which are more strict by its nature (only firstname and secondname)
    //Example, an object's name can be written like "Object <actual_object's_name>"
    if (im[LL_IM_FROM].isDefined())
    {
        std::string from = im[LL_IM_FROM].asString();
        boost::trim(from);

        std::size_t found = from.find(IM_SYMBOL_SEPARATOR);
        std::size_t len = from.size();
        std::size_t start = 0;
        while (found != std::string::npos)
        {
            std::size_t sub_len = found - start;
            if (sub_len > 0)
            {
                ostr << from.substr(start, sub_len);
            }
            LLURI::encodeCharacter(ostr, IM_SYMBOL_SEPARATOR);
            start = found + 1;
            found = from.find(IM_SYMBOL_SEPARATOR, start);
        }
        if (start < len)
        {
            std::string str_end = from.substr(start, len - start);
            ostr << str_end;
        }
        if (len > 0)
        {
            ostr << IM_SEPARATOR;
        }
    }

    if (im[LL_IM_TEXT].isDefined())
    {
        std::string im_text = im[LL_IM_TEXT].asString();

        //multilined text will be saved with prepended spaces
        boost::replace_all(im_text, NEW_LINE, NEW_LINE_SPACE_PREFIX);
        ostr << im_text;
    }
}

bool LLChatLogParser::parse(std::string& raw, LLSD& im, const LLSD& parse_params)
{
    if (!raw.length()) return false;

    bool cut_off_todays_date = parse_params.has("cut_off_todays_date")  ? parse_params["cut_off_todays_date"].asBoolean()  : true;
    im = LLSD::emptyMap();

    //matching a timestamp
    boost::match_results<std::string::const_iterator> matches;
    // <FS:Ansariel> Seconds in timestamps: try the (mandatory) seconds form first, then the
    // original regex for a timestamp without seconds or no timestamp at all
    bool has_sec = false;
    if (ll_regex_match(raw, matches, TIMESTAMP_AND_STUFF_SEC))
    {
        has_sec = true;
    }
    else if (!ll_regex_match(raw, matches, TIMESTAMP_AND_STUFF))
    {
        return false;
    }
    // </FS:Ansariel>

    bool has_timestamp = matches[IDX_TIMESTAMP].matched;
    if (has_timestamp)
    {
        //timestamp was successfully parsed
        std::string timestamp = matches[IDX_TIMESTAMP];
        boost::trim(timestamp);
        timestamp.erase(0, 1);
        timestamp.erase(timestamp.length()-1, 1);

        im[LL_IM_DATE_TIME] = timestamp;    // Retain full date-time for merging chat histories

        if (cut_off_todays_date)
        {
            if (has_sec)
            {
                LLLogChatTimeScanner::instance().checkAndCutOffDateWithSec(timestamp);
            }
            else
            {
                LLLogChatTimeScanner::instance().checkAndCutOffDate(timestamp);
            }
        }

        im[LL_IM_TIME] = timestamp;
    }
    else
    {   //timestamp is optional
        im[LL_IM_DATE_TIME] = "";
        im[LL_IM_TIME] = "";
    }

    bool has_stuff = matches[IDX_STUFF].matched;
    if (!has_stuff)
    {
        return false;  //*TODO should return false or not?
    }

    //matching a name and a text
    std::string stuff = matches[IDX_STUFF];
    boost::match_results<std::string::const_iterator> name_and_text;
    if (!ll_regex_match(stuff, name_and_text, NAME_AND_TEXT)) return false;

    bool has_name = name_and_text[IDX_NAME].matched;
    std::string name = LLURI::unescape(name_and_text[IDX_NAME]);

    //we don't need a name/text separator
    if (has_name && name.length() && name[name.length()-1] == ':')
    {
        name.erase(name.length()-1, 1);
    }

    if (!has_name || name == SYSTEM_FROM)
    {
        //name is optional too
        im[LL_IM_FROM] = SYSTEM_FROM;
        im[LL_IM_FROM_ID] = LLUUID::null;
    }

    //possibly a case of complex object names consisting of 3+ words
    if (!has_name)
    {
        std::string::size_type divider_pos = stuff.find(NAME_TEXT_DIVIDER);
        if (divider_pos != std::string::npos && divider_pos < (stuff.length() - NAME_TEXT_DIVIDER.length()))
        {
            im[LL_IM_FROM] = LLURI::unescape(stuff.substr(0, divider_pos));
            im[LL_IM_TEXT] = stuff.substr(divider_pos + NAME_TEXT_DIVIDER.length());
            return true;
        }
    }

    if (!has_name)
    {
        //text is mandatory
        im[LL_IM_TEXT] = stuff;
        return true; //parse as a message from Second Life
    }

    bool has_text = name_and_text[IDX_TEXT].matched;
    if (!has_text) return false;

    //for parsing logs created in very old versions of a viewer
    if (name == "You")
    {
        if (parse_params.has("agent_name"))
        {
            // off the main thread: the caller looked the agent up (#371)
            im[LL_IM_FROM] = parse_params["agent_name"].asString();
            im[LL_IM_FROM_ID] = parse_params["agent_id"].asUUID();
        }
        else
        {
            std::string agent_name;
            LLAgentUI::buildFullname(agent_name);
            im[LL_IM_FROM] = agent_name;
            im[LL_IM_FROM_ID] = gAgentID;
        }
    }
    else
    {
        im[LL_IM_FROM] = name;
    }

    im[LL_IM_TEXT] = name_and_text[IDX_TEXT];
    return true;  //parsed name and message text, maybe have a timestamp too
}

LLDeleteHistoryThread::LLDeleteHistoryThread(std::list<LLSD>* messages, LLLoadHistoryThread* loadThread)
    : LLActionThread("delete chat history"),
    mMessages(messages),
    mLoadThread(loadThread)
{
}
LLDeleteHistoryThread::~LLDeleteHistoryThread()
{
}
void LLDeleteHistoryThread::run()
{
    if (mLoadThread != NULL)
    {
        mLoadThread->waitFinished();
    }
    if (NULL != mMessages)
    {
        delete mMessages;
    }
    mMessages = NULL;
    setFinished();
}

LLActionThread::LLActionThread(const std::string& name)
    : LLThread(name),
    mMutex(),
    mRunCondition(),
    mFinished(false)
{
}

LLActionThread::~LLActionThread()
{
}

void LLActionThread::waitFinished()
{
    mMutex.lock();
    if (!mFinished)
    {
        mMutex.unlock();
        mRunCondition.wait();
    }
    else
    {
        mMutex.unlock();
    }
}

void LLActionThread::setFinished()
{
    mMutex.lock();
    mFinished = true;
    mMutex.unlock();
    mRunCondition.signal();
}

LLLoadHistoryThread::LLLoadHistoryThread(const std::string& file_name, std::list<LLSD>* messages, const LLSD& load_params)
    : LLActionThread("load chat history"),
    mMessages(messages),
    mFileName(file_name),
    mLoadParams(load_params),
    mNewLoad(true),
    mLoadEndSignal(NULL)
{
}

LLLoadHistoryThread::~LLLoadHistoryThread()
{
}

void LLLoadHistoryThread::run()
{
    if(mNewLoad)
    {
        loadHistory(mFileName, mMessages, mLoadParams);
        auto count = mMessages->size();
        LL_INFOS() << "mMessages->size(): " << count << LL_ENDL;
        setFinished();
    }
}

void LLLoadHistoryThread::loadHistory(const std::string& file_name, std::list<LLSD>* messages, const LLSD& load_params)
{
    if (file_name.empty())
    {
        LL_WARNS("LLLogChat::loadHistory") << "Session name is Empty!" << LL_ENDL;
        return ;
    }

    LLLogChat::flushPendingWrites();

    bool load_all_history = load_params.has("load_all_history") ? load_params["load_all_history"].asBoolean() : false;
    LLFILE* fptr = LLFile::fopen(LLLogChat::makeLogFileName(file_name), "rb");/*Flawfinder: ignore*/

    if (!fptr)
    {
        bool is_group = load_params.has("is_group") ? load_params["is_group"].asBoolean() : false;
        if (is_group)
        {
            std::string old_name(file_name);
            old_name.erase(old_name.size() - GROUP_CHAT_SUFFIX.size());
            fptr = LLFile::fopen(LLLogChat::makeLogFileName(old_name), "rb");
            if (fptr)
            {
                fclose(fptr);
                LLFile::copy(LLLogChat::makeLogFileName(old_name), LLLogChat::makeLogFileName(file_name));
            }
            fptr = LLFile::fopen(LLLogChat::makeLogFileName(file_name), "rb");
        }
        if (!fptr)
        {
            fptr = LLFile::fopen(LLLogChat::oldLogFileName(file_name), "rb");/*Flawfinder: ignore*/
            if (!fptr)
            {
                mNewLoad = false;
                (*mLoadEndSignal)(messages, file_name);
                return;                     //No previous conversation with this name.
            }
        }
    }

    char buffer[LOG_RECALL_SIZE];       /*Flawfinder: ignore*/

    char *bptr;
    size_t len;
    bool firstline = true;

    if (load_all_history || fseek(fptr, (LOG_RECALL_SIZE - 1) * -1  , SEEK_END))
    {   //We need to load the whole historyFile or it's smaller than recall size, so get it all.
        firstline = false;
        if (fseek(fptr, 0, SEEK_SET))
        {
            fclose(fptr);
            mNewLoad = false;
            (*mLoadEndSignal)(messages, file_name);
            return;
        }
    }


    while (fgets(buffer, LOG_RECALL_SIZE, fptr)  && !feof(fptr))
    {
        len = strlen(buffer) - 1;       /*Flawfinder: ignore*/

        for (bptr = (buffer + len); (*bptr == '\n' || *bptr == '\r') && bptr>buffer; bptr--)    *bptr='\0';


        if (firstline)
        {
            firstline = false;
            continue;
        }
        std::string line(remove_utf8_bom(buffer));

        //updated 1.23 plaint text log format requires a space added before subsequent lines in a multilined message
        if (' ' == line[0])
        {
            line.erase(0, MULTI_LINE_PREFIX.length());
            append_to_last_message(*messages, '\n' + line);
        }
        else if (0 == len && ('\n' == line[0] || '\r' == line[0]))
        {
            //to support old format's multilined messages with new lines used to divide paragraphs
            append_to_last_message(*messages, line);
        }
        else
        {
            LLSD item;
            if (!LLChatLogParser::parse(line, item, load_params))
            {
                item[LL_IM_TEXT] = line;
            }
            messages->push_back(item);
        }
    }

    fclose(fptr);
    mNewLoad = false;
    (*mLoadEndSignal)(messages, file_name);
}

boost::signals2::connection LLLoadHistoryThread::setLoadEndSignal(const load_end_signal_t::slot_type& cb)
{
    if (NULL == mLoadEndSignal)
    {
        mLoadEndSignal = new load_end_signal_t();
    }

    return mLoadEndSignal->connect(cb);
}

void LLLoadHistoryThread::removeLoadEndSignal(const load_end_signal_t::slot_type& cb)
{
    if (NULL != mLoadEndSignal)
    {
        mLoadEndSignal->disconnect_all_slots();
        delete mLoadEndSignal;
    }
    mLoadEndSignal = NULL;
}

//
// LLChatLogSource (#371): paged reads of a conversation's log, off the main thread
//

namespace
{
    const S64 LOG_READ_CHUNK = 16384;

    // as LLLogChat::loadChatHistory: restore mention URLs saved as [@username](URL)
    void restore_mention_urls(std::string& line)
    {
        if (line.find("/mention)") != std::string::npos)
        {
            static const boost::regex altered_mention_regex("\\[@([^\\]]+)\\]\\((" APP_HEADER_REGEX "/agent/[\\da-f-]+/mention)\\)",
                                                            boost::regex::perl | boost::regex::icase);
            line = boost::regex_replace(line, altered_mention_regex, "$2");
        }
    }

    void strip_utf8_bom(std::string& line)
    {
        if (line.size() >= 3 && line[0] == (char)0xEF && line[1] == (char)0xBB && line[2] == (char)0xBF)
        {
            line.erase(0, 3);
        }
    }

    // A line that continues the previous message: the 1.23 format prefixes them with a
    // space, older logs separated paragraphs with empty lines.
    bool is_continuation(const std::string& line)
    {
        return line.empty() || line[0] == ' ';
    }

    // The lines of [0, end) of a log file, newest first.
    class LogLinesBackward
    {
    public:
        LogLinesBackward(llifstream& file, S64 end) : mFile(file), mBufStart(end) {}

        bool prev(std::string& line, S64& line_begin)
        {
            while (mPos == 0 && mBufStart > 0)
            {
                if (!load())
                {
                    return false;
                }
            }
            if (mPos == 0)
            {
                return false;
            }
            for (;;)
            {
                size_t text_end = mPos;
                if (mBuf[text_end - 1] == '\n')
                {
                    --text_end;
                }
                size_t start = 0;
                bool found = false;
                if (text_end > 0)
                {
                    size_t nl = mBuf.rfind('\n', text_end - 1);
                    if (nl != std::string::npos)
                    {
                        start = nl + 1;
                        found = true;
                    }
                }
                if (!found && mBufStart > 0)
                {
                    // the line starts before what has been read
                    if (!load())
                    {
                        return false;
                    }
                    continue;
                }
                line.assign(mBuf, start, text_end - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                line_begin = mBufStart + (S64)start;
                if (line_begin == 0)
                {
                    strip_utf8_bom(line);
                }
                mPos = start;
                return true;
            }
        }

    private:
        bool load()
        {
            // keep only what has not been returned yet, so a long scan stays small
            mBuf.resize(mPos);
            const S64 n = llmin(LOG_READ_CHUNK, mBufStart);
            const S64 new_start = mBufStart - n;
            std::string chunk((size_t)n, '\0');
            mFile.clear();
            mFile.seekg((std::streamoff)new_start);
            mFile.read(&chunk[0], (std::streamsize)n);
            if (mFile.gcount() != (std::streamsize)n)
            {
                return false;
            }
            mBuf.insert(0, chunk);
            mPos += (size_t)n;
            mBufStart = new_start;
            return true;
        }

        llifstream& mFile;
        std::string mBuf;
        S64         mBufStart;  // file offset of mBuf[0]
        size_t      mPos = 0;   // end of the part of mBuf not returned yet
    };

    // The lines of [begin, end) of a log file, oldest first.
    class LogLinesForward
    {
    public:
        LogLinesForward(llifstream& file, S64 begin, S64 end) : mFile(file), mBufStart(begin), mEnd(end) {}

        bool next(std::string& line, S64& line_begin)
        {
            for (;;)
            {
                size_t nl = mBuf.find('\n', mPos);
                const bool at_end = mBufStart + (S64)mBuf.size() >= mEnd;
                if (nl == std::string::npos && !at_end)
                {
                    if (!load())
                    {
                        return false;
                    }
                    continue;
                }
                if (nl == std::string::npos)
                {
                    if (mPos >= mBuf.size())
                    {
                        return false;
                    }
                    nl = mBuf.size();   // last line, no terminator
                }
                line.assign(mBuf, mPos, nl - mPos);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                line_begin = mBufStart + (S64)mPos;
                if (line_begin == 0)
                {
                    strip_utf8_bom(line);
                }
                mPos = llmin(nl + 1, mBuf.size());
                return true;
            }
        }

    private:
        bool load()
        {
            mBuf.erase(0, mPos);
            mBufStart += (S64)mPos;
            mPos = 0;
            const S64 read_from = mBufStart + (S64)mBuf.size();
            const S64 n = llmin(LOG_READ_CHUNK, mEnd - read_from);
            if (n <= 0)
            {
                return false;
            }
            std::string chunk((size_t)n, '\0');
            mFile.clear();
            mFile.seekg((std::streamoff)read_from);
            mFile.read(&chunk[0], (std::streamsize)n);
            if (mFile.gcount() != (std::streamsize)n)
            {
                return false;
            }
            mBuf.append(chunk);
            return true;
        }

        llifstream& mFile;
        std::string mBuf;
        S64         mBufStart;  // file offset of mBuf[0]
        S64         mEnd;
        size_t      mPos = 0;
    };

    S64 file_size(const std::string& path)
    {
        llifstream file(path, std::ios::in | std::ios::binary);
        if (!file.is_open())
        {
            return -1;
        }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        return size < 0 ? -1 : (S64)size;
    }

    // "%Y/%m/%d %H:%M[:%S]" -> "%H:%M[:%S]" for today's lines, as LLLogChatTimeScanner does
    // on the main thread
    void cut_off_todays_date(std::string& time_str, const LLChatLogSource::ParseContext& ctx)
    {
        S32 year = 0, month = 0, day = 0;
        if (sscanf(time_str.c_str(), "%d/%d/%d", &year, &month, &day) == 3
            && year == ctx.mTodayYear && month == ctx.mTodayMonth && day == ctx.mTodayDay)
        {
            size_t pos = time_str.find(' ');
            if (pos != std::string::npos)
            {
                time_str.erase(0, pos + 1);
                LLStringUtil::trim(time_str);
            }
        }
    }

    // A message from its head line and its continuation lines (file order), assembled as
    // LLLogChat::loadChatHistory does
    void make_entry(std::string head, const std::vector<std::string>& continuation,
                    const LLChatLogSource::ParseContext& ctx, LLChatLogEntry& entry)
    {
        restore_mention_urls(head);

        LLSD params;
        params["cut_off_todays_date"] = false;
        params["agent_name"] = ctx.mAgentName;
        params["agent_id"] = ctx.mAgentID;

        LLSD item;
        if (!LLChatLogParser::parse(head, item, params))
        {
            item[LL_IM_TEXT] = head;
        }

        std::string text = item[LL_IM_TEXT].asString();
        for (std::string line : continuation)
        {
            restore_mention_urls(line);
            if (line.empty())
            {
                text += '\n';
            }
            else
            {
                text += '\n' + line.substr(MULTI_LINE_PREFIX.length());
            }
        }

        entry.mText = text;
        entry.mFrom = item[LL_IM_FROM].asString();
        entry.mHasFromID = item.has(LL_IM_FROM_ID);
        entry.mFromID = item[LL_IM_FROM_ID].asUUID();
        entry.mDateTime = item[LL_IM_DATE_TIME].asString();
        entry.mTime = item[LL_IM_TIME].asString();
        cut_off_todays_date(entry.mTime, ctx);
    }

    bool text_matches(const std::string& head, const std::vector<std::string>& continuation,
                      const LLWString& needle, bool case_insensitive)
    {
        std::string raw = head;
        for (const std::string& line : continuation)
        {
            raw += '\n';
            raw += line;
        }
        LLWString hay = utf8str_to_wstring(raw);
        if (case_insensitive)
        {
            LLWStringUtil::toLower(hay);
        }
        return hay.find(needle) != LLWString::npos;
    }

    // Scans a log chain for a message containing needle, from `from` back to the oldest
    // message or forward to the end of the history. Runs on the general pool.
    bool scan_chain(const LLChatLogSource::chain_t& chain, const LLWString& needle, bool case_insensitive,
                    bool search_back, const LLChatLogCursor& from,
                    const std::shared_ptr<std::atomic<U32>>& generation, U32 gen, LLChatLogCursor& where)
    {
        if (chain.empty())
        {
            return false;
        }
        std::string line;
        S64 line_begin = 0;
        if (search_back)
        {
            S32 file = from.isEnd() ? 0 : from.mFile;
            S64 end = from.isEnd() ? -1 : from.mOffset;
            for (; file < (S32)chain.size(); ++file, end = -1)
            {
                const S64 region_end = (end < 0 || end > chain[file].mEnd) ? chain[file].mEnd : end;
                llifstream stream(chain[file].mPath, std::ios::in | std::ios::binary);
                if (!stream.is_open() || region_end <= 0)
                {
                    continue;
                }
                LogLinesBackward lines(stream, region_end);
                std::vector<std::string> continuation;
                while (lines.prev(line, line_begin))
                {
                    if (generation->load() != gen)
                    {
                        return false;
                    }
                    if (is_continuation(line))
                    {
                        continuation.push_back(line);
                        continue;
                    }
                    std::reverse(continuation.begin(), continuation.end());
                    if (text_matches(line, continuation, needle, case_insensitive))
                    {
                        where.mFile = file;
                        where.mOffset = line_begin;
                        return true;
                    }
                    continuation.clear();
                }
            }
            return false;
        }

        if (from.isEnd())
        {
            return false;
        }
        S32 file = llmin(from.mFile, (S32)chain.size() - 1);
        S64 begin = llmax((S64)0, from.mOffset);
        for (; file >= 0; --file, begin = 0)
        {
            if (begin >= chain[file].mEnd)
            {
                continue;
            }
            llifstream stream(chain[file].mPath, std::ios::in | std::ios::binary);
            if (!stream.is_open())
            {
                continue;
            }
            LogLinesForward lines(stream, begin, chain[file].mEnd);
            bool have_head = false;
            std::string head;
            S64 head_begin = 0;
            std::vector<std::string> continuation;
            for (;;)
            {
                if (generation->load() != gen)
                {
                    return false;
                }
                const bool got = lines.next(line, line_begin);
                if (got && is_continuation(line))
                {
                    if (have_head)
                    {
                        continuation.push_back(line);
                    }
                    continue;
                }
                if (have_head && text_matches(head, continuation, needle, case_insensitive))
                {
                    where.mFile = file;
                    where.mOffset = head_begin;
                    return true;
                }
                if (!got)
                {
                    break;
                }
                head = line;
                head_begin = line_begin;
                have_head = true;
                continuation.clear();
            }
        }
        return false;
    }

    LL::WorkQueue::weak_t main_queue()
    {
        return LL::WorkQueue::getInstance("mainloop");
    }
}

LLChatLogSource::LLChatLogSource(const std::string& primary_path, const std::string& legacy_path,
                                 const std::string& dir, const std::string& dated_pattern, const std::string& undated_name)
:   mPrimaryPath(primary_path),
    mLegacyPath(legacy_path),
    mDir(dir),
    mDatedPattern(dated_pattern),
    mUndatedName(undated_name),
    mFindGeneration(std::make_shared<std::atomic<U32>>(0))
{
}

// static
LLChatLogSource::ptr_t LLChatLogSource::create(const std::string& file_name, bool is_group)
{
    std::string name = file_name;
    LLStringUtil::trim(name);
    if (name.empty())
    {
        return nullptr;
    }

    std::string legacy_path;
    if (is_group && file_name.size() > GROUP_CHAT_SUFFIX.size()
        && file_name.compare(file_name.size() - GROUP_CHAT_SUFFIX.size(), GROUP_CHAT_SUFFIX.size(), GROUP_CHAT_SUFFIX) == 0)
    {
        legacy_path = LLLogChat::makeLogFileName(file_name.substr(0, file_name.size() - GROUP_CHAT_SUFFIX.size()));
    }

    // older dated files of this conversation, as LLLogChat::oldLogFileName finds them
    const std::string clean_name = LLLogChat::cleanFileName(file_name);
    const std::string dir = gDirUtilp->getPerAccountChatLogsDir() + gDirUtilp->getDirDelimiter();
    const std::string pattern = clean_name + (clean_name == "chat" ? "-???\?-?\?-??.txt" : "-???\?-??.txt");

    ptr_t source = std::make_shared<LLChatLogSource>(LLLogChat::makeLogFileName(file_name), legacy_path, dir, pattern,
                                                     clean_name + '.' + LL_TRANSCRIPT_FILE_EXTENSION);
    // Freeze the history now, ahead of anything this conversation logs from here on
    LLLogChat::queueLogJob([source]() { source->resolve(); });
    return source;
}

// static
LLChatLogSource::ParseContext LLChatLogSource::makeParseContext()
{
    ParseContext ctx;
    date today = LLLogChatTimeScanner::instance().getTodayPacificDate();
    if (!today.is_special())
    {
        ctx.mTodayYear = (S32)today.year();
        ctx.mTodayMonth = (S32)today.month().as_number();
        ctx.mTodayDay = (S32)today.day();
    }
    LLAgentUI::buildFullname(ctx.mAgentName);
    ctx.mAgentID = gAgentID;
    return ctx;
}

void LLChatLogSource::resolve()
{
    if (mResolved)
    {
        return;
    }
    mResolved = true;

    // Group logs used to be named without " (group)": adopt the old file, as loadChatHistory does
    if (!mLegacyPath.empty() && !LLFile::isfile(mPrimaryPath) && LLFile::isfile(mLegacyPath))
    {
        LLFile::copy(mLegacyPath, mPrimaryPath);
    }

    std::vector<std::string> chain;
    if (LLFile::isfile(mPrimaryPath))
    {
        chain.push_back(mPrimaryPath);
    }

    // Older dated files (LogFileNamewithDate), newest first; an undated file predates them
    const std::string primary_name = mPrimaryPath.substr(mPrimaryPath.find_last_of("/\\") + 1);
    const bool primary_dated = primary_name != mUndatedName;
    std::vector<std::string> dated;
    {
        LLDirIterator iter(mDir, mDatedPattern);
        std::string name;
        while (iter.next(name))
        {
            if (name == primary_name || (primary_dated && name > primary_name))
            {
                continue;
            }
            dated.push_back(name);
        }
    }
    std::sort(dated.begin(), dated.end(), std::greater<std::string>());
    for (const std::string& name : dated)
    {
        chain.push_back(mDir + name);
    }
    if (primary_dated && LLFile::isfile(mDir + mUndatedName))
    {
        chain.push_back(mDir + mUndatedName);
    }

    for (const std::string& path : chain)
    {
        const S64 size = file_size(path);
        if (size > 0)
        {
            LogFile file;
            file.mPath = path;
            file.mEnd = size;
            mFiles.push_back(file);
        }
    }
}

void LLChatLogSource::readBefore(const LLChatLogCursor& before, S32 count, const ParseContext& ctx, LLChatLogPage& page)
{
    resolve();

    S32 file = before.isEnd() ? 0 : before.mFile;
    S64 end = before.isEnd() ? -1 : before.mOffset;
    page.mAtEnd = mFiles.empty() || (file == 0 && (end < 0 || end >= mFiles[0].mEnd));

    std::vector<LLChatLogEntry> found;  // newest first
    while (file >= 0 && file < (S32)mFiles.size() && (S32)found.size() < count)
    {
        const LogFile& log = mFiles[file];
        const S64 region_end = (end < 0 || end > log.mEnd) ? log.mEnd : end;
        llifstream stream(log.mPath, std::ios::in | std::ios::binary);
        if (!stream.is_open() || region_end <= 0)
        {
            ++file;
            end = -1;
            continue;
        }

        LogLinesBackward lines(stream, region_end);
        std::vector<std::string> continuation;  // newest first
        S64 next_begin = region_end;            // where the message after the current one starts
        std::string line;
        S64 line_begin = 0;
        bool file_done = false;
        while ((S32)found.size() < count)
        {
            if (!lines.prev(line, line_begin))
            {
                // lines before the file's first message belong to nothing: dropped, as LL does
                file_done = true;
                break;
            }
            if (is_continuation(line))
            {
                continuation.push_back(line);
                continue;
            }
            std::reverse(continuation.begin(), continuation.end());
            LLChatLogEntry entry;
            make_entry(line, continuation, ctx, entry);
            entry.mBegin.mFile = file;
            entry.mBegin.mOffset = line_begin;
            entry.mEnd.mFile = file;
            entry.mEnd.mOffset = next_begin;
            found.push_back(std::move(entry));
            continuation.clear();
            next_begin = line_begin;
            if (line_begin == 0)
            {
                file_done = true;
                break;
            }
        }
        if (!file_done)
        {
            break;  // page full, more in this file
        }
        ++file;
        end = -1;
    }

    // something older is left unless the oldest message starts the oldest file
    page.mAtStart = true;
    if (!found.empty())
    {
        const LLChatLogCursor& oldest = found.back().mBegin;
        page.mAtStart = oldest.mOffset <= 0 && oldest.mFile + 1 >= (S32)mFiles.size();
    }

    page.mMessages.assign(std::make_move_iterator(found.rbegin()), std::make_move_iterator(found.rend()));
}

void LLChatLogSource::readAfter(const LLChatLogCursor& after, S32 count, const ParseContext& ctx, LLChatLogPage& page)
{
    resolve();

    page.mAtStart = false;
    page.mAtEnd = true;
    if (after.isEnd() || mFiles.empty())
    {
        return;
    }

    S32 file = llmin(after.mFile, (S32)mFiles.size() - 1);
    S64 begin = llmax((S64)0, after.mOffset);
    while (file >= 0)
    {
        const LogFile& log = mFiles[file];
        if (begin >= log.mEnd)
        {
            --file;
            begin = 0;
            continue;
        }
        llifstream stream(log.mPath, std::ios::in | std::ios::binary);
        if (!stream.is_open())
        {
            --file;
            begin = 0;
            continue;
        }

        LogLinesForward lines(stream, begin, log.mEnd);
        bool have_head = false;
        std::string head;
        S64 head_begin = 0;
        std::vector<std::string> continuation;
        std::string line;
        S64 line_begin = 0;
        for (;;)
        {
            const bool got = lines.next(line, line_begin);
            if (got && is_continuation(line))
            {
                if (have_head)
                {
                    continuation.push_back(line);
                }
                continue;   // a continuation with no message before it is dropped
            }
            if (have_head)
            {
                LLChatLogEntry entry;
                make_entry(head, continuation, ctx, entry);
                entry.mBegin.mFile = file;
                entry.mBegin.mOffset = head_begin;
                entry.mEnd.mFile = file;
                entry.mEnd.mOffset = got ? line_begin : log.mEnd;
                page.mMessages.push_back(std::move(entry));
                continuation.clear();
                have_head = false;
            }
            if (!got)
            {
                break;
            }
            if ((S32)page.mMessages.size() >= count)
            {
                page.mAtEnd = false;    // this head starts the next page
                return;
            }
            head = line;
            head_begin = line_begin;
            have_head = true;
        }
        --file;
        begin = 0;
    }
}

// static
void LLChatLogSource::deliver(const page_callback_t& cb, std::shared_ptr<LLChatLogPage> page)
{
    if (!cb || LLApp::isExiting())
    {
        return;
    }
    cb(*page);
}

void LLChatLogSource::loadPage(const LLChatLogCursor& before, S32 count, page_callback_t cb)
{
    ptr_t self = shared_from_this();
    const ParseContext ctx = makeParseContext();
    LL::WorkQueue::weak_t main = main_queue();
    LLLogChat::queueLogJob([self, before, count, ctx, cb, main]()
        {
            auto page = std::make_shared<LLChatLogPage>();
            self->readBefore(before, llmax(1, count), ctx, *page);
            LL::WorkQueue::postMaybe(main, [cb, page]() { deliver(cb, page); });
        });
}

void LLChatLogSource::loadPageAfter(const LLChatLogCursor& after, S32 count, page_callback_t cb)
{
    ptr_t self = shared_from_this();
    const ParseContext ctx = makeParseContext();
    LL::WorkQueue::weak_t main = main_queue();
    LLLogChat::queueLogJob([self, after, count, ctx, cb, main]()
        {
            auto page = std::make_shared<LLChatLogPage>();
            self->readAfter(after, llmax(1, count), ctx, *page);
            LL::WorkQueue::postMaybe(main, [cb, page]() { deliver(cb, page); });
        });
}

void LLChatLogSource::cancelFind()
{
    ++(*mFindGeneration);
}

void LLChatLogSource::find(const std::string& text, bool case_insensitive, bool search_back, const LLChatLogCursor& from, find_callback_t cb)
{
    const U32 gen = ++(*mFindGeneration);
    std::shared_ptr<std::atomic<U32>> generation = mFindGeneration;
    LLWString needle = utf8str_to_wstring(text);
    if (case_insensitive)
    {
        LLWStringUtil::toLower(needle);
    }
    if (needle.empty())
    {
        return;
    }

    ptr_t self = shared_from_this();
    LL::WorkQueue::weak_t main = main_queue();
    LL::WorkQueue::weak_t general = LL::WorkQueue::getInstance("General");
    auto scan = [needle, case_insensitive, search_back, from, generation, gen, cb, main](const chain_t& chain)
        {
            LLChatLogCursor where;
            const bool found = scan_chain(chain, needle, case_insensitive, search_back, from, generation, gen, where);
            if (generation->load() != gen)
            {
                return;
            }
            LL::WorkQueue::postMaybe(main, [found, where, generation, gen, cb]()
                {
                    if (!LLApp::isExiting() && generation->load() == gen && cb)
                    {
                        cb(found, where);
                    }
                });
        };
    // The chain is known on the chat log thread; the scan may be long, so it runs on the
    // general pool (it only reads below the end of the history, which nothing writes)
    LLLogChat::queueLogJob([self, scan, general]()
        {
            self->resolve();
            const chain_t chain = self->mFiles;
            if (!LL::WorkQueue::postMaybe(general, [scan, chain]() { scan(chain); }))
            {
                scan(chain);
            }
        });
}
