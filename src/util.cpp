// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util.h>

#include <chainparamsbase.h>
#include <random.h>
#include <serialize.h>
#include <utilstrencodings.h>

#include <stdarg.h>
#include <ctype.h>

#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#ifndef WIN32
// for posix_fallocate
#ifdef __linux__

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#define _POSIX_C_SOURCE 200112L

#endif // __linux__

#include <algorithm>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#else

#ifdef _MSC_VER
#pragma warning(disable:4786)
#pragma warning(disable:4804)
#pragma warning(disable:4805)
#pragma warning(disable:4717)
#endif

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501

#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501

#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <io.h> /* for _commit */
#include <shlobj.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef HAVE_MALLOPT_ARENA_MAX
#include <malloc.h>
#endif

#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/algorithm/string/predicate.hpp> // for startswith() and endswith()
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/program_options/detail/config_file.hpp>
#include <boost/thread.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/filesystem.hpp>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/conf.h>

// Application startup time (used for uptime calculation)
const int64_t nStartupTime = GetTime();

const char * const BITCOIN_CONF_FILENAME = "qtumx.conf";
const char * const BITCOIN_PID_FILENAME = "qtumxd.pid";
const char * const DEFAULT_DEBUGLOGFILE = "debug.log";
const char * const DEFAULT_DEBUGVMLOGFILE = "vm.log";

ArgsManager gArgs;
bool fPrintToConsole = false;
bool fPrintToDebugLog = true;

bool fLogTimestamps = DEFAULT_LOGTIMESTAMPS;
bool fLogTimeMicros = DEFAULT_LOGTIMEMICROS;
bool fLogIPs = DEFAULT_LOGIPS;
std::atomic<bool> fReopenDebugLog(false);
CTranslationInterface translationInterface;

/** Log categories bitfield. */
std::atomic<uint32_t> logCategories(0);

/** Init OpenSSL library multithreading support */
static std::unique_ptr<CCriticalSection[]> ppmutexOpenSSL;
void locking_callback(int mode, int i, const char* file, int line) NO_THREAD_SAFETY_ANALYSIS
{
    if (mode & CRYPTO_LOCK) {
        ENTER_CRITICAL_SECTION(ppmutexOpenSSL[i]);
    } else {
        LEAVE_CRITICAL_SECTION(ppmutexOpenSSL[i]);
    }
}

// Singleton for wrapping OpenSSL setup/teardown.
class CInit
{
public:
    CInit()
    {
        // Init OpenSSL library multithreading support
        ppmutexOpenSSL.reset(new CCriticalSection[CRYPTO_num_locks()]);
        CRYPTO_set_locking_callback(locking_callback);

        // OpenSSL can optionally load a config file which lists optional loadable modules and engines.
        // We don't use them so we don't require the config. However some of our libs may call functions
        // which attempt to load the config file, possibly resulting in an exit() or crash if it is missing
        // or corrupt. Explicitly tell OpenSSL not to try to load the file. The result for our libs will be
        // that the config appears to have been loaded and there are no modules/engines available.
        OPENSSL_no_config();

#ifdef WIN32
        // Seed OpenSSL PRNG with current contents of the screen
        RAND_screen();
#endif

        // Seed OpenSSL PRNG with performance counter
        RandAddSeed();
    }
    ~CInit()
    {
        // Securely erase the memory used by the PRNG
        RAND_cleanup();
        // Shutdown OpenSSL library multithreading support
        CRYPTO_set_locking_callback(nullptr);
        // Clear the set of locks now to maintain symmetry with the constructor.
        ppmutexOpenSSL.reset();
    }
}
instance_of_cinit;

/**
 * LogPrintf() has been broken a couple of times now
 * by well-meaning people adding mutexes in the most straightforward way.
 * It breaks because it may be called by global destructors during shutdown.
 * Since the order of destruction of static/global objects is undefined,
 * defining a mutex as a global object doesn't work (the mutex gets
 * destroyed, and then some later destructor calls OutputDebugStringF,
 * maybe indirectly, and you get a core dump at shutdown trying to lock
 * the mutex).
 */

static boost::once_flag debugPrintInitFlag = BOOST_ONCE_INIT;

/**
 * We use boost::call_once() to make sure mutexDebugLog and
 * vMsgsBeforeOpenLog are initialized in a thread-safe manner.
 *
 * NOTE: fileout, mutexDebugLog and sometimes vMsgsBeforeOpenLog
 * are leaked on exit. This is ugly, but will be cleaned up by
 * the OS/libc. When the shutdown sequence is fully audited and
 * tested, explicit destruction of these objects can be implemented.
 */
static FILE* fileout = nullptr;
static boost::mutex* mutexDebugLog = nullptr;
static std::list<std::string>* vMsgsBeforeOpenLog;

/////////////////////////////////////////////////////////////////////// // qtum
static FILE* fileoutVM = nullptr;
///////////////////////////////////////////////////////////////////////

static int FileWriteStr(const std::string &str, FILE *fp)
{
    return fwrite(str.data(), 1, str.size(), fp);
}

static void DebugPrintInit()
{
    assert(mutexDebugLog == nullptr);
    mutexDebugLog = new boost::mutex();
    vMsgsBeforeOpenLog = new std::list<std::string>;
}

fs::path GetDebugLogPath()
{
    fs::path logfile(gArgs.GetArg("-debuglogfile", DEFAULT_DEBUGLOGFILE));
    if (logfile.is_absolute()) {
        return logfile;
    } else {
        return GetDataDir() / logfile;
    }
}

fs::path GetDebugVMLogPath()
{
    fs::path logfile(gArgs.GetArg("-debugvmlogfile", DEFAULT_DEBUGVMLOGFILE));
    if (logfile.is_absolute()) {
        return logfile;
    } else {
        return GetDataDir() / logfile;
    }
}

bool OpenDebugLog()
{
    boost::call_once(&DebugPrintInit, debugPrintInitFlag);
    boost::mutex::scoped_lock scoped_lock(*mutexDebugLog);

    assert(fileout == nullptr);
    assert(fileoutVM == nullptr); // qtum
    assert(vMsgsBeforeOpenLog);
    fs::path pathDebug = GetDebugLogPath();
    fs::path pathDebugVM = GetDebugVMLogPath(); // qtum
    fileout = fsbridge::fopen(pathDebug, "a");
    fileoutVM = fsbridge::fopen(pathDebugVM, "a"); // qtum
    if (!fileout || !fileoutVM) {
        return false;
    }

    if (fileout) {
        setbuf(fileout, nullptr); // unbuffered
        // dump buffered messages from before we opened the log
        while (!vMsgsBeforeOpenLog->empty()) {
            FileWriteStr(vMsgsBeforeOpenLog->front(), fileout);
            vMsgsBeforeOpenLog->pop_front();
        }
    }
    ///////////////////////////////////////////// // qtum
    if (fileoutVM) {
        setbuf(fileoutVM, nullptr); // unbuffered
        // dump buffered messages from before we opened the log
        while (!vMsgsBeforeOpenLog->empty()) {
            FileWriteStr(vMsgsBeforeOpenLog->front(), fileoutVM);
            vMsgsBeforeOpenLog->pop_front();
        }
    }
    /////////////////////////////////////////////

    delete vMsgsBeforeOpenLog;
    vMsgsBeforeOpenLog = nullptr;
    return true;
}

struct CLogCategoryDesc
{
    uint32_t flag;
    std::string category;
};

const CLogCategoryDesc LogCategories[] =
{
    {BCLog::NONE, "0"},
    {BCLog::NONE, "none"},
    {BCLog::NET, "net"},
    {BCLog::TOR, "tor"},
    {BCLog::MEMPOOL, "mempool"},
    {BCLog::HTTP, "http"},
    {BCLog::BENCH, "bench"},
    {BCLog::ZMQ, "zmq"},
    {BCLog::DB, "db"},
    {BCLog::RPC, "rpc"},
    {BCLog::ESTIMATEFEE, "estimatefee"},
    {BCLog::ADDRMAN, "addrman"},
    {BCLog::SELECTCOINS, "selectcoins"},
    {BCLog::REINDEX, "reindex"},
    {BCLog::CMPCTBLOCK, "cmpctblock"},
    {BCLog::RAND, "rand"},
    {BCLog::PRUNE, "prune"},
    {BCLog::PROXY, "proxy"},
    {BCLog::MEMPOOLREJ, "mempoolrej"},
    {BCLog::LIBEVENT, "libevent"},
    {BCLog::COINDB, "coindb"},
    {BCLog::QT, "qt"},
    {BCLog::LEVELDB, "leveldb"},
    {BCLog::COINSTAKE, "coinstake"},
    {BCLog::HTTPPOLL, "http-poll"},
    {BCLog::ALL, "1"},
    {BCLog::ALL, "all"},
};

bool GetLogCategory(uint32_t *f, const std::string *str)
{
    if (f && str) {
        if (*str == "") {
            *f = BCLog::ALL;
            return true;
        }
        for (unsigned int i = 0; i < ARRAYLEN(LogCategories); i++) {
            if (LogCategories[i].category == *str) {
                *f = LogCategories[i].flag;
                return true;
            }
        }
    }
    return false;
}

std::string ListLogCategories()
{
    std::string ret;
    int outcount = 0;
    for (unsigned int i = 0; i < ARRAYLEN(LogCategories); i++) {
        // Omit the special cases.
        if (LogCategories[i].flag != BCLog::NONE && LogCategories[i].flag != BCLog::ALL) {
            if (outcount != 0) ret += ", ";
            ret += LogCategories[i].category;
            outcount++;
        }
    }
    return ret;
}

std::vector<CLogCategoryActive> ListActiveLogCategories()
{
    std::vector<CLogCategoryActive> ret;
    for (unsigned int i = 0; i < ARRAYLEN(LogCategories); i++) {
        // Omit the special cases.
        if (LogCategories[i].flag != BCLog::NONE && LogCategories[i].flag != BCLog::ALL) {
            CLogCategoryActive catActive;
            catActive.category = LogCategories[i].category;
            catActive.active = LogAcceptCategory(LogCategories[i].flag);
            ret.push_back(catActive);
        }
    }
    return ret;
}

/**
 * fStartedNewLine is a state variable held by the calling context that will
 * suppress printing of the timestamp when multiple calls are made that don't
 * end in a newline. Initialize it to true, and hold it, in the calling context.
 */
static std::string LogTimestampStr(const std::string &str, std::atomic_bool *fStartedNewLine)
{
    std::string strStamped;

    if (!fLogTimestamps)
        return str;

    if (*fStartedNewLine) {
        int64_t nTimeMicros = GetTimeMicros();
        strStamped = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nTimeMicros/1000000);
        if (fLogTimeMicros)
            strStamped += strprintf(".%06d", nTimeMicros%1000000);
        int64_t mocktime = GetMockTime();
        if (mocktime) {
            strStamped += " (mocktime: " + DateTimeStrFormat("%Y-%m-%d %H:%M:%S", mocktime) + ")";
        }
        strStamped += ' ' + str;
    } else
        strStamped = str;

    if (!str.empty() && str[str.size()-1] == '\n')
        *fStartedNewLine = true;
    else
        *fStartedNewLine = false;

    return strStamped;
}

int LogPrintStr(const std::string &str, bool useVMLog)
{
//////////////////////////////// // qtum
    FILE* file = fileout;
    if(useVMLog){
        file = fileoutVM;
    }
////////////////////////////////

    int ret = 0; // Returns total number of characters written
    static std::atomic_bool fStartedNewLine(true);

    std::string strTimestamped = LogTimestampStr(str, &fStartedNewLine);

    if (fPrintToConsole)
    {
        // print to console
        ret = fwrite(strTimestamped.data(), 1, strTimestamped.size(), stdout);
        fflush(stdout);
    }
    else if (fPrintToDebugLog)
    {
        boost::call_once(&DebugPrintInit, debugPrintInitFlag);
        boost::mutex::scoped_lock scoped_lock(*mutexDebugLog);

        // buffer if we haven't opened the log yet
        if (file == nullptr) {
            assert(vMsgsBeforeOpenLog);
            ret = strTimestamped.length();
            vMsgsBeforeOpenLog->push_back(strTimestamped);
        }
        else
        {
            // reopen the log file, if requested
            if (fReopenDebugLog) {
                fReopenDebugLog = false;
                fs::path pathDebug = GetDebugLogPath();
                if (fsbridge::freopen(pathDebug,"a",file) != nullptr)
                    setbuf(file, nullptr); // unbuffered
            }

            ret = FileWriteStr(strTimestamped, file);
        }
    }
    return ret;
}

/** A map that contains all the currently held directory locks. After
 * successful locking, these will be held here until the global destructor
 * cleans them up and thus automatically unlocks them, or ReleaseDirectoryLocks
 * is called.
 */
static std::map<std::string, std::unique_ptr<boost::interprocess::file_lock>> dir_locks;
/** Mutex to protect dir_locks. */
static std::mutex cs_dir_locks;

bool LockDirectory(const fs::path& directory, const std::string lockfile_name, bool probe_only, bool try_lock)
{
    std::lock_guard<std::mutex> ulock(cs_dir_locks);
    fs::path pathLockFile = directory / lockfile_name;

    // If a lock for this directory already exists in the map, don't try to re-lock it
    if (dir_locks.count(pathLockFile.string())) {
        return true;
    }

    // Create empty lock file if it doesn't exist.
    FILE* file = fsbridge::fopen(pathLockFile, "a");
    if (file) fclose(file);

    try {
        auto lock = MakeUnique<boost::interprocess::file_lock>(pathLockFile.string().c_str());
        if (try_lock && !lock->try_lock()) {
            return false;
        }
        if (!probe_only) {
            // Lock successful and we're not just probing, put it into the map
            dir_locks.emplace(pathLockFile.string(), std::move(lock));
        }
    } catch (const boost::interprocess::interprocess_exception& e) {
        return error("Error while attempting to lock directory %s: %s", directory.string(), e.what());
    }
    return true;
}

void ReleaseDirectoryLocks()
{
    std::lock_guard<std::mutex> ulock(cs_dir_locks);
    dir_locks.clear();
}

/** Interpret string as boolean, for argument parsing */
static bool InterpretBool(const std::string& strValue)
{
    if (strValue.empty())
        return true;
    return (atoi(strValue) != 0);
}

/** Turn -noX into -X=0 */
static void InterpretNegativeSetting(std::string& strKey, std::string& strValue)
{
    if (strKey.length()>3 && strKey[0]=='-' && strKey[1]=='n' && strKey[2]=='o')
    {
        strKey = "-" + strKey.substr(3);
        strValue = InterpretBool(strValue) ? "0" : "1";
    }
}

void ArgsManager::ParseParameters(int argc, const char* const argv[])
{
    LOCK(cs_args);
    mapArgs.clear();
    mapMultiArgs.clear();

    for (int i = 1; i < argc; i++)
    {
        std::string str(argv[i]);
        std::string strValue;
        size_t is_index = str.find('=');
        if (is_index != std::string::npos)
        {
            strValue = str.substr(is_index+1);
            str = str.substr(0, is_index);
        }
#ifdef WIN32
        boost::to_lower(str);
        if (boost::algorithm::starts_with(str, "/"))
            str = "-" + str.substr(1);
#endif

        if (str[0] != '-')
            break;

        // Interpret --foo as -foo.
        // If both --foo and -foo are set, the last takes effect.
        if (str.length() > 1 && str[1] == '-')
            str = str.substr(1);
        InterpretNegativeSetting(str, strValue);

        mapArgs[str] = strValue;
        mapMultiArgs[str].push_back(strValue);
    }
}

std::vector<std::string> ArgsManager::GetArgs(const std::string& strArg) const
{
    LOCK(cs_args);
    auto it = mapMultiArgs.find(strArg);
    if (it != mapMultiArgs.end()) return it->second;
    return {};
}

bool ArgsManager::IsArgSet(const std::string& strArg) const
{
    LOCK(cs_args);
    return mapArgs.count(strArg);
}

std::string ArgsManager::GetArg(const std::string& strArg, const std::string& strDefault) const
{
    LOCK(cs_args);
    auto it = mapArgs.find(strArg);
    if (it != mapArgs.end()) return it->second;
    return strDefault;
}

int64_t ArgsManager::GetArg(const std::string& strArg, int64_t nDefault) const
{
    LOCK(cs_args);
    auto it = mapArgs.find(strArg);
    if (it != mapArgs.end()) return atoi64(it->second);
    return nDefault;
}

bool ArgsManager::GetBoolArg(const std::string& strArg, bool fDefault) const
{
    LOCK(cs_args);
    auto it = mapArgs.find(strArg);
    if (it != mapArgs.end()) return InterpretBool(it->second);
    return fDefault;
}

bool ArgsManager::SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    if (IsArgSet(strArg)) return false;
    ForceSetArg(strArg, strValue);
    return true;
}

bool ArgsManager::SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}

void ArgsManager::ForceSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    mapArgs[strArg] = strValue;
    mapMultiArgs[strArg] = {strValue};
}



static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message) {
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option, const std::string &message) {
    return std::string(optIndent,' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent,' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

static std::string FormatException(const std::exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "bitcoin";
#endif
    if (pex)
        return strprintf(
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintExceptionContinue(const std::exception* pex, const char* pszThread)
{
    std::string message = FormatException(pex, pszThread);
    LogPrintf("\n\n************************\n%s\n", message);
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
}

fs::path GetDefaultDataDir()
{
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\QtumX
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\QtumX
    // Mac: ~/Library/Application Support/QtumX
    // Unix: ~/.qtum
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "QtumXBeta";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == nullptr || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    return pathRet / "Library/Application Support/QtumXBeta";
#else
    // Unix
    return pathRet / ".qtumxbeta";
#endif
#endif
}

static fs::path pathCached;
static fs::path pathCachedNetSpecific;
static CCriticalSection csPathCached;

const fs::path &GetDataDir(bool fNetSpecific)
{

    LOCK(csPathCached);

    fs::path &path = fNetSpecific ? pathCachedNetSpecific : pathCached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    // value so we don't have to do memory allocations after that.
    if (!path.empty())
        return path;

    if (gArgs.IsArgSet("-datadir")) {
        path = fs::system_complete(gArgs.GetArg("-datadir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }
    if (fNetSpecific)
        path /= BaseParams().DataDir();

    if (fs::create_directories(path)) {
        // This is the first run, create wallets subdirectory too
        fs::create_directories(path / "wallets");
    }

    return path;
}

void ClearDatadirCache()
{
    LOCK(csPathCached);

    pathCached = fs::path();
    pathCachedNetSpecific = fs::path();
}

fs::path GetConfigFile(const std::string& confPath)
{
    fs::path pathConfigFile(confPath);
    if (!pathConfigFile.is_complete())
        pathConfigFile = GetDataDir(false) / pathConfigFile;

    return pathConfigFile;
}

fs::path GetRemoteConfigFile(const std::string& chainId) {
    // check default download path
    fs::path localDir = GetDataDir(false) / ("chain_" + chainId);
    fs::path localPath = localDir / BITCOIN_CONF_FILENAME;
    if (fs::is_regular_file(localPath) && !fs::is_empty(localPath)) {
        return localPath;
    }

    // download conf file from https://qtumx.net/chain/{chainId}.conf
    static const std::string host = "qtumx.net";
    std::string path = "/chain/" + chainId + ".conf";
    std::string url = "https://" + host + path;
    fprintf(stdout, "Start blockchain %s with configuration: %s\n",
            chainId.c_str(),
            url.c_str());

    using boost::asio::ip::tcp;
    namespace ssl = boost::asio::ssl;
    typedef ssl::stream<tcp::socket> ssl_socket;
    boost::system::error_code ec;

    // Create a context and load root certificate
    ssl::context ctx(ssl::context::tlsv1_client);
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.set_verify_callback(ssl::rfc2818_verification(host));
    std::string const cert =  // COMODO UserTrust / AddTrust External Root
            "-----BEGIN CERTIFICATE-----\n"
            "MIIENjCCAx6gAwIBAgIBATANBgkqhkiG9w0BAQUFADBvMQswCQYDVQQGEwJTRTEU\n"
            "MBIGA1UEChMLQWRkVHJ1c3QgQUIxJjAkBgNVBAsTHUFkZFRydXN0IEV4dGVybmFs\n"
            "IFRUUCBOZXR3b3JrMSIwIAYDVQQDExlBZGRUcnVzdCBFeHRlcm5hbCBDQSBSb290\n"
            "MB4XDTAwMDUzMDEwNDgzOFoXDTIwMDUzMDEwNDgzOFowbzELMAkGA1UEBhMCU0Ux\n"
            "FDASBgNVBAoTC0FkZFRydXN0IEFCMSYwJAYDVQQLEx1BZGRUcnVzdCBFeHRlcm5h\n"
            "bCBUVFAgTmV0d29yazEiMCAGA1UEAxMZQWRkVHJ1c3QgRXh0ZXJuYWwgQ0EgUm9v\n"
            "dDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALf3GjPm8gAELTngTlvt\n"
            "H7xsD821+iO2zt6bETOXpClMfZOfvUq8k+0DGuOPz+VtUFrWlymUWoCwSXrbLpX9\n"
            "uMq/NzgtHj6RQa1wVsfwTz/oMp50ysiQVOnGXw94nZpAPA6sYapeFI+eh6FqUNzX\n"
            "mk6vBbOmcZSccbNQYArHE504B4YCqOmoaSYYkKtMsE8jqzpPhNjfzp/haW+710LX\n"
            "a0Tkx63ubUFfclpxCDezeWWkWaCUN/cALw3CknLa0Dhy2xSoRcRdKn23tNbE7qzN\n"
            "E0S3ySvdQwAl+mG5aWpYIxG3pzOPVnVZ9c0p10a3CitlttNCbxWyuHv77+ldU9U0\n"
            "WicCAwEAAaOB3DCB2TAdBgNVHQ4EFgQUrb2YejS0Jvf6xCZU7wO94CTLVBowCwYD\n"
            "VR0PBAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wgZkGA1UdIwSBkTCBjoAUrb2YejS0\n"
            "Jvf6xCZU7wO94CTLVBqhc6RxMG8xCzAJBgNVBAYTAlNFMRQwEgYDVQQKEwtBZGRU\n"
            "cnVzdCBBQjEmMCQGA1UECxMdQWRkVHJ1c3QgRXh0ZXJuYWwgVFRQIE5ldHdvcmsx\n"
            "IjAgBgNVBAMTGUFkZFRydXN0IEV4dGVybmFsIENBIFJvb3SCAQEwDQYJKoZIhvcN\n"
            "AQEFBQADggEBALCb4IUlwtYj4g+WBpKdQZic2YR5gdkeWxQHIzZlj7DYd7usQWxH\n"
            "YINRsPkyPef89iYTx4AWpb9a/IfPeHmJIZriTAcKhjW88t5RxNKWt9x+Tu5w/Rw5\n"
            "6wwCURQtjr0W4MHfRnXnJK3s9EK0hZNwEGe6nQY1ShjTK3rMUUKhemPR5ruhxSvC\n"
            "Nr4TDea9Y355e6cJDUCrat2PisP29owaQgVR1EX1n6diIWgVIEM8med8vSTYqZEX\n"
            "c4g/VhsxOBi0cQ+azcgOno4uG+GMmIPLHzHxREzGBHNJdmAPx/i9F4BrLunMTA5a\n"
            "mnkPIAou1Z5jJh5VkpTYghdae9C8x49OhgQ=\n"
            "-----END CERTIFICATE-----\n";
    ctx.add_certificate_authority(boost::asio::buffer(cert.data(), cert.size()));

    // Open a socket and connect it to the remote host
    boost::asio::io_service io_service;
    ssl_socket sock(io_service, ctx);

    // Get a list of endpoints corresponding to the server name
    tcp::resolver resolver(io_service);
    tcp::resolver::query query(host, "https");
    boost::asio::connect(sock.lowest_layer(), resolver.resolve(query));
    sock.lowest_layer().set_option(tcp::no_delay(true));

    // Set SNI Hostname (many hosts need this to handshake successfully)
    if (!SSL_set_tlsext_host_name(sock.native_handle(), host.c_str())) {
        throw boost::system::system_error(
                boost::system::error_code(static_cast<int>(::ERR_get_error()),
                        boost::asio::error::get_ssl_category()));
    }

    // Perform SSL handshake.
    sock.handshake(ssl_socket::client, ec);
    if (ec)
        throw boost::system::system_error(ec, "Please check your connection to " + url);

    // Set up an HTTP GET request message
    boost::asio::streambuf request;
    std::ostream request_stream(&request);
    request_stream << "GET " << path << " HTTP/1.0\r\n";
    request_stream << "Host: " << host << "\r\n";
    request_stream << "Accept: */*\r\n";
    request_stream << "Connection: close\r\n\r\n";

    // Send the HTTP request to the remote host
    boost::asio::write(sock, request);

    // Get the response
    boost::asio::streambuf response;
    std::stringstream ss;
    while (boost::asio::read(sock, response, boost::asio::transfer_at_least(1), ec)) {
        ss << &response;
    }

    // Gracefully close the stream
    sock.shutdown(ec);

    // Check that response is OK
    std::string http_version;
    ss >> http_version;
    if (!ss || http_version.substr(0, 5) != "HTTP/") {
        throw boost::system::system_error(
                boost::system::error_code(1, boost::system::system_category()),
                "Invalid http version response: " + http_version);
    }
    std::string status_code;
    ss >> status_code;
    if (status_code != "200") {
        throw boost::system::system_error(
                boost::system::error_code(1, boost::system::system_category()),
                "Response status code: " + status_code);
    }

    // Extract the body
    std::string res = ss.str();
    std::size_t pos = res.find("\r\n\r\n");
    if (pos == std::string::npos || (pos + 4) >= res.size()) {
        throw boost::system::system_error(
                boost::system::error_code(1, boost::system::system_category()),
                "Error extract body from response");
    }
    std::string body = res.substr(pos + 4);

    // Write the message to stdout and the default download path
    fprintf(stdout, "%s\n", body.c_str());
    fs::create_directories(localDir);
    fs::ofstream ofs(localPath);
    ofs << body;
    ofs.close();

    return localPath;
}

bool CheckChainId(const std::string& chainId) {
    for (size_t i = 0; i != chainId.size(); ++i) {
        if (!islower(chainId[i]) && !isdigit(chainId[i])) {
            return false;
        }
    }
    return true;
}

void ArgsManager::ReadRemoteConfigFile(const std::string& chainId) {
    std::string cId = chainId.empty() ? "qtumx" : chainId;
    gArgs.ForceSetArg("-chain", cId);

    if (!CheckChainId(cId)) {
        throw boost::system::system_error(
                boost::system::error_code(1, boost::system::system_category()),
                "Illegal chain id: " + cId);
    }

    ParseConfigFile(GetRemoteConfigFile(cId));
}

void ArgsManager::ReadConfigFile(const std::string& confPath)
{
    if (!ParseConfigFile(GetConfigFile(confPath))) {
        return;
    }

    // If datadir is changed in .conf file:
    ClearDatadirCache();
    if (!fs::is_directory(GetDataDir(false))) {
        throw std::runtime_error(strprintf("specified data directory \"%s\" does not exist.", gArgs.GetArg("-datadir", "").c_str()));
    }
}

bool ArgsManager::ParseConfigFile(const fs::path& path) {
    fs::ifstream streamConfig(path);
    if (!streamConfig.good())
        return false; // No bitcoin.conf file is OK

    {
        LOCK(cs_args);
        std::set<std::string> setOptions;
        setOptions.insert("*");

        for (boost::program_options::detail::config_file_iterator it(streamConfig, setOptions), end; it != end; ++it)
        {
            // Don't overwrite existing settings so command line settings override bitcoin.conf
            std::string strKey = std::string("-") + it->string_key;
            std::string strValue = it->value[0];
            InterpretNegativeSetting(strKey, strValue);
            if (mapArgs.count(strKey) == 0)
                mapArgs[strKey] = strValue;
            mapMultiArgs[strKey].push_back(strValue);
        }
    }

    return true;
}

#ifndef WIN32
fs::path GetPidFile()
{
    fs::path pathPidFile(gArgs.GetArg("-pid", BITCOIN_PID_FILENAME));
    if (!pathPidFile.is_complete()) pathPidFile = GetDataDir() / pathPidFile;
    return pathPidFile;
}

void CreatePidFile(const fs::path &path, pid_t pid)
{
    FILE* file = fsbridge::fopen(path, "w");
    if (file)
    {
        fprintf(file, "%d\n", pid);
        fclose(file);
    }
}
#endif

bool RenameOver(fs::path src, fs::path dest)
{
#ifdef WIN32
    return MoveFileExA(src.string().c_str(), dest.string().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = std::rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

/**
 * Ignores exceptions thrown by Boost's create_directories if the requested directory exists.
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * write to the parent directory.
 */
bool TryCreateDirectories(const fs::path& p)
{
    try
    {
        return fs::create_directories(p);
    } catch (const fs::filesystem_error&) {
        if (!fs::exists(p) || !fs::is_directory(p))
            throw;
    }

    // create_directories didn't create the directory, it had to have existed already
    return false;
}

void FileCommit(FILE *file)
{
    fflush(file); // harmless if redundantly called
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    FlushFileBuffers(hFile);
#else
    #if defined(__linux__) || defined(__NetBSD__)
    fdatasync(fileno(file));
    #elif defined(__APPLE__) && defined(F_FULLFSYNC)
    fcntl(fileno(file), F_FULLFSYNC, 0);
    #else
    fsync(fileno(file));
    #endif
#endif
}

bool TruncateFile(FILE *file, unsigned int length) {
#if defined(WIN32)
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

/**
 * this function tries to raise the file descriptor limit to the requested number.
 * It returns the actual file descriptor limit (which may be more or less than nMinFD)
 */
int RaiseFileDescriptorLimit(int nMinFD) {
#if defined(WIN32)
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) {
        if (limitFD.rlim_cur < (rlim_t)nMinFD) {
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return limitFD.rlim_cur;
    }
    return nMinFD; // getrlimit failed, assume it's fine
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * it is advisory, and the range specified in the arguments will never contain live data
 */
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length) {
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(MAC_OSX)
    // OSX specific version
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = (off_t)offset + length;
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), fst.fst_length);
#elif defined(__linux__)
    // Version using posix_fallocate
    off_t nEndPos = (off_t)offset + length;
    posix_fallocate(fileno(file), 0, nEndPos);
#else
    // Fallback version
    // TODO: just write one byte per block
    static const char buf[65536] = {};
    fseek(file, offset, SEEK_SET);
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway
        length -= now;
    }
#endif
}

void ShrinkDebugFile()
{
    // Amount of debug.log to save at end when shrinking (must fit in memory)
    constexpr size_t RECENT_DEBUG_HISTORY_SIZE = 10 * 1000000;
    // Scroll debug.log if it's getting too big
    fs::path pathLog = GetDebugLogPath();
    FILE* file = fsbridge::fopen(pathLog, "r");
    // If debug.log file is more than 10% bigger the RECENT_DEBUG_HISTORY_SIZE
    // trim it down by saving only the last RECENT_DEBUG_HISTORY_SIZE bytes
    if (file && fs::file_size(pathLog) > 11 * (RECENT_DEBUG_HISTORY_SIZE / 10))
    {
        // Restart the file with some of the end
        std::vector<char> vch(RECENT_DEBUG_HISTORY_SIZE, 0);
        fseek(file, -((long)vch.size()), SEEK_END);
        int nBytes = fread(vch.data(), 1, vch.size(), file);
        fclose(file);

        file = fsbridge::fopen(pathLog, "w");
        if (file)
        {
            fwrite(vch.data(), 1, nBytes, file);
            fclose(file);
        }
    }
    else if (file != nullptr)
        fclose(file);
}

#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    char pszPath[MAX_PATH] = "";

    if(SHGetSpecialFolderPathA(nullptr, pszPath, nFolder, fCreate))
    {
        return fs::path(pszPath);
    }

    LogPrintf("SHGetSpecialFolderPathA() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

void runCommand(const std::string& strCommand)
{
    if (strCommand.empty()) return;
    int nErr = ::system(strCommand.c_str());
    if (nErr)
        LogPrintf("runCommand error: system(%s) returned %d\n", strCommand, nErr);
}

void RenameThread(const char* name)
{
#if defined(PR_SET_NAME)
    // Only the first 15 characters are used (16 - NUL terminator)
    ::prctl(PR_SET_NAME, name, 0, 0, 0);
#elif (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
    pthread_set_name_np(pthread_self(), name);

#elif defined(MAC_OSX)
    pthread_setname_np(name);
#else
    // Prevent warnings for unused parameters...
    (void)name;
#endif
}

void SetupEnvironment()
{
#ifdef HAVE_MALLOPT_ARENA_MAX
    // glibc-specific: On 32-bit systems set the number of arenas to 1.
    // By default, since glibc 2.10, the C library will create up to two heap
    // arenas per core. This is known to cause excessive virtual address space
    // usage in our usage. Work around it by setting the maximum number of
    // arenas to 1.
    if (sizeof(void*) == 4) {
        mallopt(M_ARENA_MAX, 1);
    }
#endif
    // On most POSIX systems (e.g. Linux, but not BSD) the environment's locale
    // may be invalid, in which case the "C" locale is used as fallback.
#if !defined(WIN32) && !defined(MAC_OSX) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
    try {
        std::locale(""); // Raises a runtime error if current locale is invalid
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C", 1);
    }
#endif
    // The path locale is lazy initialized and to avoid deinitialization errors
    // in multithreading environments, it is set explicitly by the main thread.
    // A dummy locale is used to extract the internal default locale, used by
    // fs::path, which is then used to explicitly imbue the path.
    std::locale loc = fs::path::imbue(std::locale::classic());
    fs::path::imbue(loc);
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true;
}

int GetNumCores()
{
#if BOOST_VERSION >= 105600
    return boost::thread::physical_concurrency();
#else // Must fall back to hardware_concurrency, which unfortunately counts virtual cores
    return boost::thread::hardware_concurrency();
#endif
}

std::string CopyrightHolders(const std::string& strPrefix)
{
    std::string strCopyrightHolders = strPrefix + strprintf(_(COPYRIGHT_HOLDERS), _(COPYRIGHT_HOLDERS_SUBSTITUTION));

    // Check for untranslated substitution to make sure Bitcoin Core copyright is not removed by accident
    if (strprintf(COPYRIGHT_HOLDERS, COPYRIGHT_HOLDERS_SUBSTITUTION).find("QtumX Core") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The QtumX Core developers";
    }
    return strCopyrightHolders;
}

// Obtain the application startup time (used for uptime calculation)
int64_t GetStartupTime()
{
    return nStartupTime;
}

bool CheckHex(const std::string& str) {
    size_t data=0;
    if(str.size() > 2 && (str.compare(0, 2, "0x") == 0 || str.compare(0, 2, "0X") == 0)){
        data=2;
    }
    return str.size() > data && str.find_first_not_of("0123456789abcdefABCDEF", data) == std::string::npos;
}
