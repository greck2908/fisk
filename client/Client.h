#ifndef CLIENT_H
#define CLIENT_H

#include <string>
#include <cstdarg>
#include <assert.h>
#include <sys/stat.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string.h>
#include <vector>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

struct CompilerArgs;
namespace Client {
std::mutex &mutex();
std::string findCompiler(int argc, char **argv, std::string *resolvedCompiler);
void parsePath(const char *path, std::string *basename, std::string *dirname);
class Slot
{
public:
    Slot(int fd, std::string &&path);
   ~Slot();
private:
    Slot(const Slot &) = delete;
    Slot &operator=(const Slot &) = delete;

    const int mFD;
    const std::string mPath;
};

enum AcquireSlotMode {
    Try,
    Wait
};
std::unique_ptr<Slot> acquireSlot(AcquireSlotMode mode);
int runLocal(const std::string &compiler, int argc, char **argv, std::unique_ptr<Slot> &&slot);
unsigned long long mono();
bool setFlag(int fd, int flag);
bool recursiveMkdir(const std::string &path, mode_t mode = S_IRWXU);

class Preprocessed
{
public:
    Preprocessed()
        : exitStatus(-1), mDone(false), mJoined(false)
    {}
    ~Preprocessed();
    void wait();

    std::string stdOut, stdErr;
    int exitStatus;
private:
    std::mutex mMutex;
    std::condition_variable mCond;
    std::thread mThread;
    bool mDone;
    bool mJoined;
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

inline std::string base64(const std::string &src)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *sink = BIO_new(BIO_s_mem());
    BIO_push(b64, sink);
    BIO_write(b64, &src[0], src.size());
    BIO_flush(b64);
    const char *encoded;
    const long len = BIO_get_mem_data(sink, &encoded);
    return std::string(encoded, len);
}

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
}

#endif /* CLIENT_H */
