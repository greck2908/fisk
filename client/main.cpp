#include "Client.h"
#include "CompilerArgs.h"
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <string.h>

static void parsePath(const char *path, std::string *basename, std::string *dirname)
{
    size_t lastSlash = std::string::npos;
    for (size_t i=0; i<PATH_MAX && path[i]; ++i) {
        if (path[i] == '/' && path[i + 1])
            lastSlash = i;
    }
    if (basename) {
        *basename = lastSlash == std::string::npos ? path : path + lastSlash + 1;
    }
    if (dirname) {
        *dirname = lastSlash == std::string::npos ? std::string(".") : std::string(path, lastSlash + 1);
    }
}

static void runLocal(int argc, char **argv)
{
    const char *path = getenv("PATH");
    std::string exec;
    if (path) {
        std::string self, basename, dirname;
        char realPath[PATH_MAX];
        const char *begin = path, *end = 0;
        if (!realpath(argv[0], realPath))
            goto error;
        parsePath(realPath, 0, &dirname);
        parsePath(argv[0], &basename, 0);
        // printf("realPath %s dirname %s argv[0] %s basename %s\n", realPath, dirname.c_str(), argv[0], basename.c_str());
        self = dirname + basename;
        do {
            end = strchr(begin, ':');
            if (!end) {
                exec = begin;
            } else {
                exec.assign(begin, end - begin);
                begin = end + 1;
            }
            // printf("trying %s\n", exec.c_str());
            if (!exec.empty()) {
                if (exec[exec.size() - 1] != '/')
                    exec += '/';
                exec += basename;
                if (exec != self && !access(exec.c_str(), X_OK)) {
                    break;
                }
                exec.clear();
            }
        } while (end);

        // printf("Found %s for %s\n", exec.c_str(), self.c_str());
        // printf("GOT SELF %s\n", self.c_str());
        char **argvCopy = new char*[argc + 1];
        argvCopy[0] = strdup(exec.c_str());
        for (size_t i=1; i<argc; ++i) {
            argvCopy[i] = argv[i];
        }
        argvCopy[argc] = 0;
        ::execv(exec.c_str(), argvCopy);
        fprintf(stderr, "fisk: Failed to exec %s (%d %s)\n", exec.c_str(), errno, strerror(errno));
        _exit(1);
    }
error:
    fprintf(stderr, "Can't find executable for %s\n", argv[0]);
    exit(1);
}

int main(int argc, char **argv)
{
    std::vector<std::string> args(argc);
    for (size_t i=0; i<argc; ++i) {
        // printf("%zu: %s\n", i, argv[i]);
        args[i] = argv[i];
    }
    std::shared_ptr<CompilerArgs> compilerArgs = CompilerArgs::create(args);

    runLocal(argc, argv);
    return 0;
}
