#ifndef CLIENT_H
#define CLIENT_H

#include "Config.h"
#include <assert.h>
#include <condition_variable>
#include <cstdarg>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <semaphore.h>
#include <set>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

class Watchdog;
struct CompilerArgs;
class SchedulerWebSocket;
namespace Client {
struct Data
{
    ~Data() {}

    int argc { 0 };
    char **argv { 0 };
    bool maintainSemaphores { false };
    std::string compiler; // this is the next one on the path and the one we will exec if we run locally
    std::string resolvedCompiler; // this one resolves g++ to gcc and is used for generating hash
    std::string slaveCompiler; // this is the one that actually will exist on the slave
    std::string hash;
    int exitCode { 0 };
    std::set<sem_t *> semaphores;

    std::shared_ptr<CompilerArgs> compilerArgs;
    Watchdog *watchdog { 0 };
};
Data &data();

extern const unsigned long long started;

std::mutex &mutex();
bool findCompiler(const std::string &preresolved);
void parsePath(const char *path, std::string *basename, std::string *dirname);
inline void parsePath(const std::string &path, std::string *basename, std::string *dirname)
{
    return parsePath(path.c_str(), basename, dirname);
}
class Slot
{
public:
    enum Type {
        DesiredCompile,
        Compile,
        Cpp
    };

    Slot(Type type, sem_t *sem);
    ~Slot();
    static constexpr const char *typeToString(Type type)
    {
        return (type == Compile ? "/fisk.compile" : (type == DesiredCompile ? "/fisk.desiredCompile" : "/fisk.cpp"));
    }
    static size_t slots(Type type)
    {
        switch (type) {
        case Compile:
            return Config::compileSlots;
        case Cpp:
            return Config::cppSlots;
        case DesiredCompile:
            return Config::desiredCompileSlots;
        }
        assert(0);
        return 0;
    }
private:
    Slot(const Slot &) = delete;
    Slot &operator=(const Slot &) = delete;

    const Type mType;
    sem_t *mSemaphore;
};

std::unique_ptr<Slot> tryAcquireSlot(Slot::Type type);
std::unique_ptr<Slot> acquireSlot(Slot::Type type);
[[noreturn]] void runLocal(std::unique_ptr<Slot> &&slot);
unsigned long long mono();
bool setFlag(int fd, int flag);
bool recursiveMkdir(const std::string &path, mode_t mode = S_IRWXU);
bool recursiveRmdir(const std::string &path);
std::string realpath(const std::string &path);

class Preprocessed
{
public:
    ~Preprocessed();
    void wait();

    std::string stdOut, stdErr;
    int exitStatus { -1 };
    unsigned long long duration { 0 };
    unsigned long long slotDuration { 0 };
    std::string depFile;
private:
    std::mutex mMutex;
    std::condition_variable mCond;
    std::thread mThread;
    bool mDone { false };
    bool mJoined { false };
    friend std::unique_ptr<Preprocessed> preprocess(const std::string &compiler, const std::shared_ptr<CompilerArgs> &args);
};
std::unique_ptr<Preprocessed> preprocess(const std::string &compiler, const std::shared_ptr<CompilerArgs> &args);

template <size_t StaticBufSize = 4096>
static std::string vformat(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    char buffer[StaticBufSize];
    const size_t size = ::vsnprintf(buffer, StaticBufSize, format, args);
    assert(size >= 0);
    std::string ret;
    if (size < StaticBufSize) {
        ret.assign(buffer, size);
    } else {
        ret.resize(size);
        ::vsnprintf(&ret[0], size+1, format, copy);
    }
    va_end(copy);
    return ret;
}

template <size_t StaticBufSize = 4096>
inline std::string format(const char *fmt, ...) __attribute__ ((__format__ (__printf__, 1, 2)));

template <size_t StaticBufSize>
inline std::string format(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string ret = vformat<StaticBufSize>(fmt, args);
    va_end(args);
    return ret;
}

inline std::string sha1(const std::string &str)
{
    std::string res(SHA_DIGEST_LENGTH, ' ');
    SHA1(reinterpret_cast<const unsigned char *>(str.c_str()), str.size(), reinterpret_cast<unsigned char *>(&res[0]));
    return res;
}

std::string base64(const std::string &src);
inline std::string toHex(const std::string &src)
{
    size_t s = src.size();
    std::string ret(s * 2, ' ');
    const unsigned char *in = reinterpret_cast<const unsigned char *>(src.c_str());
    const unsigned char hex[] = "0123456789ABCDEF";
    unsigned char *out = reinterpret_cast<unsigned char *>(&ret[0]);
    while (s--) {
        assert(in);
        assert(out);
        *out++ = hex[(*in) >> 4];
        assert(isprint(hex[(*in) >> 4]));

        assert(out);
        *out++ = hex[(*in) & 0x0F];
        assert(isprint(hex[(*in) & 0x0F]));
        ++in;
    }

    return ret;
}

inline std::vector<std::string> split(const std::string &str, const std::string &delim)
{
    std::vector<std::string> ret;
    size_t start = 0U;
    size_t end = str.find(delim);
    while (end != std::string::npos) {
        ret.push_back(str.substr(start, end - start));
        start = end + delim.length();
        end = str.find(delim, start);
    }
    return ret;
}

enum FileType {
    File,
    Directory,
    Symlink,
    Invalid
};

inline FileType fileType(const std::string &path, struct stat *st = 0)
{
    struct stat dummy;
    struct stat &stat = st ? *st : dummy;
    memset(&stat, 0, sizeof(struct stat));
    if (lstat(path.c_str(), &stat)) {
        printf("ERR [%s] %d %s\n", path.c_str(), errno, strerror(errno));
        return Invalid;
    }

    if (S_ISLNK(stat.st_mode))
        return Symlink;
    if (S_ISDIR(stat.st_mode))
        return Directory;
    if (S_ISREG(stat.st_mode))
        return File;
    printf("BAD MODE %d\n", stat.st_mode);
    return Invalid;
}

std::string environmentHash(const std::string &compiler);
std::string findExecutablePath(const char *argv0);
bool uploadEnvironment(SchedulerWebSocket *schedulerWebSocket, const std::string &tarball);
std::string prepareEnvironmentForUpload();
}

#endif /* CLIENT_H */
