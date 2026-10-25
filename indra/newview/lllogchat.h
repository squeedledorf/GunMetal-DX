/**
 * @file lllogchat.h
 * @brief LLFloaterChat class definition
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

#ifndef LL_LLLOGCHAT_H
#define LL_LLLOGCHAT_H
#include "llthread.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class LLChat;

class LLActionThread : public LLThread
{
public:
    LLActionThread(const std::string& name);
    ~LLActionThread();

    void waitFinished();
    bool isFinished() { return mFinished; }
protected:
    void setFinished();
private:
    bool mFinished;
    LLMutex mMutex;
    LLCondition mRunCondition;
};

class LLLoadHistoryThread : public LLActionThread
{
private:
    const std::string& mFileName;
    std::list<LLSD>* mMessages;
    LLSD mLoadParams;
    bool mNewLoad;
public:
    LLLoadHistoryThread(const std::string& file_name, std::list<LLSD>* messages, const LLSD& load_params);
    ~LLLoadHistoryThread();
    //void setHistoryParams(const std::string& file_name, const LLSD& load_params);
    virtual void loadHistory(const std::string& file_name, std::list<LLSD>* messages, const LLSD& load_params);
    virtual void run();

    typedef boost::signals2::signal<void (std::list<LLSD>* messages,const std::string& file_name)> load_end_signal_t;
    load_end_signal_t * mLoadEndSignal;
    boost::signals2::connection setLoadEndSignal(const load_end_signal_t::slot_type& cb);
    void removeLoadEndSignal(const load_end_signal_t::slot_type& cb);
};

class LLDeleteHistoryThread : public LLActionThread
{
private:
    std::list<LLSD>* mMessages;
    LLLoadHistoryThread* mLoadThread;
public:
    LLDeleteHistoryThread(std::list<LLSD>* messages, LLLoadHistoryThread* loadThread);
    ~LLDeleteHistoryThread();

    virtual void run();
    static void deleteHistory();
};

// #371: paged chat log reading. A conversation's history on disk is read a page at a time on
// the chat log thread -- the thread that appends to the logs, so a read sees every line queued
// before it -- and handed back to the main thread. Nothing here blocks the main thread.

// Where a message sits in a conversation's log chain (its current log file, then older ones)
struct LLChatLogCursor
{
    S32 mFile = 0;      // index in the chain, 0 = the newest file
    S64 mOffset = -1;   // byte offset in that file; -1 = the end of the history

    static LLChatLogCursor end() { return LLChatLogCursor(); }
    bool isEnd() const { return mFile == 0 && mOffset < 0; }
    bool operator==(const LLChatLogCursor& other) const { return mFile == other.mFile && mOffset == other.mOffset; }
    bool operator!=(const LLChatLogCursor& other) const { return !(*this == other); }
};

// One message read back from a log: what LLChatLogParser makes of it, as plain data
struct LLChatLogEntry
{
    std::string     mFrom;
    std::string     mText;
    std::string     mTime;          // display time, today's date cut off
    std::string     mDateTime;      // full date-time from the log, empty when the line had none
    LLUUID          mFromID;
    bool            mHasFromID = false; // the log line named the sender (system and old "You" lines)
    LLChatLogCursor mBegin;         // where the message starts: older messages are read before it
    LLChatLogCursor mEnd;           // where the next message starts
};

struct LLChatLogPage
{
    std::vector<LLChatLogEntry> mMessages;  // oldest first
    bool mAtStart = true;   // nothing older on disk
    bool mAtEnd = true;     // the newest message is the last one before the session started
};

// The on-disk history of one conversation, frozen at the moment the conversation started:
// lines it logs afterwards belong to the live session and are never read back as history.
class LLChatLogSource : public std::enable_shared_from_this<LLChatLogSource>
{
public:
    typedef std::shared_ptr<LLChatLogSource> ptr_t;
    typedef std::function<void(const LLChatLogPage&)> page_callback_t;
    typedef std::function<void(bool found, const LLChatLogCursor& where)> find_callback_t;

    // Main thread. Queues the lookup of the log files and the snapshot of where the history
    // ends ahead of any line this conversation logs from now on. Null when logs are not shown.
    static ptr_t create(const std::string& file_name, bool is_group);

    // Main thread. Up to count messages ending right before `before` (LLChatLogCursor::end() =
    // the newest), or starting right at `after`. The callback runs on the main thread, never
    // inline, and not at all once the viewer is quitting.
    void loadPage(const LLChatLogCursor& before, S32 count, page_callback_t cb);
    void loadPageAfter(const LLChatLogCursor& after, S32 count, page_callback_t cb);

    // Main thread. The nearest message before `before` (searching back) or at/after `after`
    // (searching forward) whose sender or text contains `text`. Runs on the general pool; a
    // newer find, or cancelFind(), abandons it (its callback never runs).
    void find(const std::string& text, bool case_insensitive, bool search_back, const LLChatLogCursor& from, find_callback_t cb);
    void cancelFind();

    // Main thread: the conversation's history moved or went away (transcripts deleted or moved)
    bool isStale() const { return mStale; }
    void setStale() { mStale = true; }

    struct LogFile
    {
        std::string mPath;
        S64         mEnd = 0;   // bytes of history in it
    };
    // What the chat log thread worked out; a copy of it travels with a find
    typedef std::vector<LogFile> chain_t;

    struct ParseContext
    {
        S32         mTodayYear = 0;
        S32         mTodayMonth = 0;
        S32         mTodayDay = 0;
        std::string mAgentName;
        LLUUID      mAgentID;
    };

    LLChatLogSource(const std::string& primary_path, const std::string& legacy_path,
                    const std::string& dir, const std::string& dated_pattern, const std::string& undated_name);

private:
    static ParseContext makeParseContext();
    void resolve();             // chat log thread
    void readBefore(const LLChatLogCursor& before, S32 count, const ParseContext& ctx, LLChatLogPage& page);  // chat log thread
    void readAfter(const LLChatLogCursor& after, S32 count, const ParseContext& ctx, LLChatLogPage& page);    // chat log thread
    static void deliver(const page_callback_t& cb, std::shared_ptr<LLChatLogPage> page);

    // set on the main thread before the source is shared, read-only afterwards
    const std::string   mPrimaryPath;   // the file this conversation logs to
    const std::string   mLegacyPath;    // group logs used to be named without " (group)"
    const std::string   mDir;           // chat log directory, with a trailing delimiter
    const std::string   mDatedPattern;  // older dated files (LogFileNamewithDate)
    const std::string   mUndatedName;

    // chat log thread only
    chain_t             mFiles;         // newest first
    bool                mResolved = false;

    // main thread only
    bool                mStale = false;
    std::shared_ptr<std::atomic<U32>> mFindGeneration;
};

class LLLogChat : public LLSingleton<LLLogChat>
{
    LLSINGLETON(LLLogChat);
    ~LLLogChat();
    friend class LLChatLogSource;
public:
    // status values for callback function
    enum ELogLineType {
        LOG_EMPTY,
        LOG_LINE,
        LOG_LLSD,
        LOG_END
    };

    // allow_seconds: honour FSSecondsinChatTimestamps (off for strings that end up in file names)
    static std::string timestamp2LogString(U32 timestamp, bool withdate, bool allow_seconds = true);
    static std::string makeLogFileName(std::string(filename));
    static void renameLogFile(const std::string& old_filename, const std::string& new_filename);
    /**
    *Add functions to get old and non date stamped file names when needed
    */
    static std::string oldLogFileName(std::string(filename));
    // block until all queued log appends are on disk (call before reading/renaming/deleting logs)
    static void flushPendingWrites();
    static void saveHistory(const std::string& filename,
                const std::string& from,
                const LLUUID& from_id,
                const std::string& line);
    static bool transcriptFilesExist();
    static void findTranscriptFiles(std::string pattern, std::vector<std::string>& list_of_transcriptions);
    static void getListOfTranscriptFiles(std::vector<std::string>& list);
    static void getListOfTranscriptBackupFiles(std::vector<std::string>& list_of_transcriptions);

    static void loadChatHistory(const std::string& file_name, std::list<LLSD>& messages, const LLSD& load_params = LLSD(), bool is_group = false);

    typedef boost::signals2::signal<void ()> save_history_signal_t;
    boost::signals2::connection setSaveHistorySignal(const save_history_signal_t::slot_type& cb);

    static bool moveTranscripts(const std::string currentDirectory,
                                    const std::string newDirectory,
                                    std::vector<std::string>& listOfFilesToMove,
                                    std::vector<std::string>& listOfFilesMoved);
    static bool moveTranscripts(const std::string currentDirectory,
        const std::string newDirectory,
        std::vector<std::string>& listOfFilesToMove);

    static void deleteTranscripts();
    static bool isTranscriptExist(const LLUUID& avatar_id, bool is_group=false);
    static bool isNearbyTranscriptExist();
    static bool isAdHocTranscriptExist(std::string file_name);
    static bool isTranscriptFileFound(std::string fullname);

    static std::string getGroupChatSuffix();

    bool historyThreadsFinished(LLUUID session_id);
    LLLoadHistoryThread* getLoadHistoryThread(LLUUID session_id);
    LLDeleteHistoryThread* getDeleteHistoryThread(LLUUID session_id);
    bool addLoadHistoryThread(LLUUID& session_id, LLLoadHistoryThread* lthread);
    bool addDeleteHistoryThread(LLUUID& session_id, LLDeleteHistoryThread* dthread);
    void cleanupHistoryThreads();

private:
    static std::string cleanFileName(std::string filename);
    // runs a job on the chat log thread, after every log line queued before it (#371)
    static void queueLogJob(std::function<void()>&& job);

    LLMutex* historyThreadsMutex();
    void triggerHistorySignal();

    save_history_signal_t * mSaveHistorySignal;
    std::map<LLUUID,LLLoadHistoryThread *> mLoadHistoryThreads;
    std::map<LLUUID,LLDeleteHistoryThread *> mDeleteHistoryThreads;
    LLMutex* mHistoryThreadsMutex;
};

/**
 * Formatter for the plain text chat log files
 */
class LLChatLogFormatter
{
public:
    LLChatLogFormatter(const LLSD& im) : mIM(im) {}
    virtual ~LLChatLogFormatter() {};

    friend std::ostream& operator<<(std::ostream& str, const LLChatLogFormatter& formatter)
    {
        formatter.format(formatter.mIM, str);
        return str;
    }

protected:

    /**
     * Format an instant message to a stream
     * Timestamps and sender names are required
     * New lines of multilined messages are prepended with a space
     */
    void format(const LLSD& im, std::ostream& ostr) const;

    LLSD mIM;
};

/**
 * Parser for the plain text chat log files
 */
class LLChatLogParser
{
public:

     /**
     * Parse a line from the plain text chat log file
     * General plain text log format is like: "[timestamp]  [name]: [message]"
     * [timestamp] and [name] are optional
     * Examples of plain text chat log lines:
     * "[2009/11/20 2:53]  Igor ProductEngine: howdy"
     * "Igor ProductEngine: howdy"
     * "Dserduk ProductEngine is Online"
     *
     * parse_params: cut_off_todays_date (default true; needs the main thread), and
     * agent_name / agent_id standing in for the agent off the main thread (old "You" lines)
     *
     * @return false if failed to parse mandatory data - message text
     */
    static bool parse(std::string& raw, LLSD& im, const LLSD& parse_params = LLSD());

protected:
    LLChatLogParser();
    virtual ~LLChatLogParser() {};
};

extern const std::string GROUP_CHAT_SUFFIX;

// LLSD map lookup constants
extern const std::string LL_IM_TIME; //("time");
extern const std::string LL_IM_DATE_TIME; //("datetime");
extern const std::string LL_IM_TEXT; //("message");
extern const std::string LL_IM_FROM; //("from");
extern const std::string LL_IM_FROM_ID; //("from_id");
extern const std::string LL_TRANSCRIPT_FILE_EXTENSION; //("txt");

#endif
