#include "Client.h"
#include "CompilerArgs.h"
#include "Config.h"
#include "SlaveWebSocket.h"
#include "SchedulerWebSocket.h"
#include "Log.h"
#include "Select.h"
#include "Watchdog.h"
#include "WebSocket.h"
#include <json11.hpp>
#include <climits>
#include <cstdlib>
#include <regex>
#include <cstring>
#include <unistd.h>
#include <csignal>

static const unsigned long long milliseconds_since_epoch = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
static unsigned long long preprocessedDuration = 0;
static unsigned long long preprocessedSlotDuration = 0;
int main(int argc, char **argv)
{
    if (getenv("FISKC_INVOKED")) {
        fprintf(stderr, "Recursive invocation of fiskc detected.\n");
        return 1;
    }
    setenv("FISKC_INVOKED", "1", 1);
    // usleep(500 * 1000);
    // return 0;
    std::atexit([]() {
            const Client::Data &data = Client::data();
            for (sem_t *semaphore : data.semaphores) {
                if (!data.maintainSemaphores)
                    sem_post(semaphore);
                sem_close(semaphore);
            }
            if (Watchdog *watchdog = data.watchdog) {
                if (Log::minLogLevel <= Log::Warn) {
                    std::string str = Client::format("since epoch: %llu preprocess time: %llu (slot time: %llu)",
                                                     milliseconds_since_epoch, preprocessedDuration, preprocessedSlotDuration);
                    for (size_t i=Watchdog::ConnectedToScheduler; i<=Watchdog::Finished; ++i) {
                        str += Client::format(" %s: %llu (%llu)\n", Watchdog::stageName(static_cast<Watchdog::Stage>(i)),
                                              watchdog->timings[i] - watchdog->timings[i - 1],
                                              watchdog->timings[i] - Client::started);
                    }
                    Log::log(Log::Warn, str);
                }
            }
        });

    if (!Config::init(argc, argv)) {
        return 1;
    }
    if (Config::help) {
        Config::usage(stdout);
        return 0;
    }
    if (Config::dumpSemaphores) {
#ifdef __APPLE__
        fprintf(stderr, "sem_getvalue(2) is not functional on mac so this option doesn't work\n");
#else
        for (Client::Slot::Type type : { Client::Slot::Compile, Client::Slot::Cpp, Client::Slot::DesiredCompile }) {
            if (Client::Slot::slots(type) == std::numeric_limits<size_t>::max()) {
                continue;
            }
            sem_t *sem = sem_open(Client::Slot::typeToString(type), O_CREAT, 0666, Client::Slot::slots(type));
            if (!sem) {
                fprintf(stderr, "Failed to open semaphore %s slots: %zu: %d %s\n",
                        Client::Slot::typeToString(type), Client::Slot::slots(type),
                        errno, strerror(errno));
                continue;
            }
            int val = -1;
            sem_getvalue(sem, &val);
            printf("%s %d/%zu\n", Client::Slot::typeToString(type), val, Client::Slot::slots(type));
            sem_close(sem);
        }
#endif
        return 0;
    }
    if (Config::cleanSemaphores) {
        for (Client::Slot::Type type : { Client::Slot::Compile, Client::Slot::Cpp, Client::Slot::DesiredCompile }) {
            if (sem_unlink(Client::Slot::typeToString(type))) {
                if (Client::Slot::slots(type) != std::numeric_limits<size_t>::max()) {
                    fprintf(stderr, "Failed to unlink semaphore %s: %d %s\n",
                            Client::Slot::typeToString(type), errno, strerror(errno));
                }
            }
        }
        return 0;
    }

    Watchdog watchdog;
    Client::Data &data = Client::data();
    data.argv = argv;
    data.argc = argc;
    data.watchdog = &watchdog;
    auto signalHandler = [](int) {
        for (sem_t *semaphore : Client::data().semaphores) {
            sem_post(semaphore);
        }
        _exit(1);
    };
    for (int signal : { SIGINT, SIGHUP, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGALRM, SIGTERM }) {
        std::signal(signal, signalHandler);
    }

    std::string clientName = Config::name;

    Log::Level level = Log::Silent;
    const std::string logLevel = Config::logLevel;
    if (!logLevel.empty()) {
        bool ok;
        level = Log::stringToLevel(logLevel.c_str(), &ok);
        if (!ok) {
            fprintf(stderr, "Invalid log level: %s (\"Debug\", \"Warn\", \"Error\" or \"Silent\")\n", logLevel.c_str());
            return 1;
        }
    }
    if (Config::verbose)
        level = Log::Debug;
    std::string preresolved = Config::compiler;

    Log::init(level, Config::logFile, Config::logFileAppend ? Log::Append : Log::Overwrite);

    if (!Client::findCompiler(preresolved)) {
        ERROR("Can't find executable for %s", data.argv[0]);
        return 1;
    }
    DEBUG("Resolved compiler %s (%s) to \"%s\" \"%s\" \"%s\")",
          data.argv[0], preresolved.c_str(),
          data.compiler.c_str(), data.resolvedCompiler.c_str(),
          data.slaveCompiler.c_str());

    if (!Config::noDesire) {
        if (std::unique_ptr<Client::Slot> slot = Client::tryAcquireSlot(Client::Slot::DesiredCompile)) {
            Client::runLocal(std::move(slot));
            return 0;
        }
    }

    if (Config::disabled) {
        DEBUG("Have to run locally because we're disabled");
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    {
        std::vector<std::string> args(data.argc);
        for (int i=0; i<data.argc; ++i) {
            // printf("%zu: %s\n", i, argv[i]);
            args[i] = data.argv[i];
        }
        data.compilerArgs = CompilerArgs::create(args);
    }
    if (!data.compilerArgs) {
        DEBUG("Have to run locally");
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    SchedulerWebSocket schedulerWebsocket;

    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, 0);

    std::unique_ptr<Client::Preprocessed> preprocessed = Client::preprocess(data.compiler, data.compilerArgs);
    if (!preprocessed) {
        ERROR("Failed to preprocess");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    data.hash = Client::environmentHash(data.resolvedCompiler);
    std::map<std::string, std::string> headers;
    headers["x-fisk-environments"] = data.hash; // always a single one but fisk-slave sends multiple so we'll just keep it like this for now
    Client::parsePath(data.compilerArgs->sourceFile(), &headers["x-fisk-sourcefile"], 0);
    headers["x-fisk-client-name"] = Config::name;
    headers["x-fisk-config-version"] = std::to_string(Config::Version);
    {
        std::string slave = Config::slave;
        if (!slave.empty())
            headers["x-fisk-slave"] = std::move(slave);
    }
    {
        std::string hostname = Config::hostname;
        if (!hostname.empty())
            headers["x-fisk-client-hostname"] = std::move(hostname);
    }
    std::string url = Config::scheduler;
    if (url.find("://") == std::string::npos)
        url.insert(0, "ws://");

    std::regex regex(":[0-9]+$", std::regex_constants::ECMAScript);
    if (!std::regex_search(url, regex))
        url.append(":8097");

    if (!schedulerWebsocket.connect(url + "/compile", headers)) {
        DEBUG("Have to run locally because no server");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    {
        Select select;
        select.add(&watchdog);
        select.add(&schedulerWebsocket);

        DEBUG("Starting schedulerWebsocket");
        while (!schedulerWebsocket.done
               && schedulerWebsocket.state() >= SchedulerWebSocket::None
               && schedulerWebsocket.state() <= SchedulerWebSocket::ConnectedWebSocket) {
            select.exec();
        }
        DEBUG("Finished schedulerWebsocket");
        if (!schedulerWebsocket.done) {
            DEBUG("Have to run locally because no server 2");
            watchdog.stop();
            Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
            return 0; // unreachable
        }
    }

    if (data.maintainSemaphores) {
        for (Client::Slot::Type type : { Client::Slot::Compile, Client::Slot::Cpp, Client::Slot::DesiredCompile }) {
            if (Client::Slot::slots(type) != std::numeric_limits<size_t>::max() && sem_unlink(Client::Slot::typeToString(type))) {
                if (errno != ENOENT) {
                    ERROR("Failed to unlink semaphore %s: %d %s",
                          Client::Slot::typeToString(type), errno, strerror(errno));
                } else {
                    DEBUG("Semaphore %s didn't exist", Client::Slot::typeToString(type));
                }
            } else {
                DEBUG("Destroyed semaphore %s", Client::Slot::typeToString(type));
            }
        }
    }

    if (schedulerWebsocket.needsEnvironment) {
        watchdog.stop();
        const std::string tarball = Client::prepareEnvironmentForUpload();
        // printf("GOT TARBALL %s\n", tarball.c_str());
        if (!tarball.empty()) {
            Client::uploadEnvironment(&schedulerWebsocket, tarball);
        }
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0;
    }

    if ((schedulerWebsocket.slaveHostname.empty() && schedulerWebsocket.slaveIp.empty())
        || !schedulerWebsocket.slavePort) {
        DEBUG("Have to run locally because no slave");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    // usleep(1000 * 1000 * 16);
    watchdog.transition(Watchdog::AcquiredSlave);
    SlaveWebSocket slaveWebSocket;
    Select select;
    select.add(&slaveWebSocket);
    select.add(&watchdog);
    headers["x-fisk-job-id"] = std::to_string(schedulerWebsocket.jobId);
    headers["x-fisk-slave-ip"] = schedulerWebsocket.slaveIp;
    if (!slaveWebSocket.connect(Client::format("ws://%s:%d/compile",
                                               schedulerWebsocket.slaveHostname.empty() ? schedulerWebsocket.slaveIp.c_str() : schedulerWebsocket.slaveHostname.c_str(),
                                               schedulerWebsocket.slavePort), headers)) {
        DEBUG("Have to run locally because no slave connection");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    while (slaveWebSocket.state() < SchedulerWebSocket::ConnectedWebSocket)
        select.exec();
    watchdog.transition(Watchdog::ConnectedToSlave);

    DEBUG("Waiting for preprocessed");
    preprocessed->wait();
    watchdog.transition(Watchdog::PreprocessFinished);
    DEBUG("Preprocessed finished");
    preprocessedDuration = preprocessed->duration;
    preprocessedSlotDuration = preprocessed->slotDuration;

    if (preprocessed->exitStatus != 0) {
        ERROR("Failed to preprocess. Running locally");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    std::vector<std::string> args = data.compilerArgs->commandLine;
    args[0] = data.slaveCompiler;

    const bool wait = slaveWebSocket.handshakeResponseHeader("x-fisk-wait") == "true";
    json11::Json::object msg {
        { "commandLine", args },
        { "argv0", data.compiler },
        { "wait", wait },
        { "bytes", static_cast<int>(preprocessed->stdOut.size()) }
    };

    const std::string json = json11::Json(msg).dump();
    slaveWebSocket.wait = wait;
    slaveWebSocket.send(WebSocket::Text, json.c_str(), json.size());
    if (wait) {
        while ((slaveWebSocket.hasPendingSendData() || slaveWebSocket.wait) && slaveWebSocket.state() == SchedulerWebSocket::ConnectedWebSocket)
            select.exec();
        if (slaveWebSocket.state() != SchedulerWebSocket::ConnectedWebSocket) {
            DEBUG("Have to run locally because something went wrong with the slave");
            watchdog.stop();
            Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
            return 0; // unreachable
        }
    }

    assert(!slaveWebSocket.wait);
    slaveWebSocket.send(WebSocket::Binary, preprocessed->stdOut.c_str(), preprocessed->stdOut.size());

    while (slaveWebSocket.hasPendingSendData() && slaveWebSocket.state() == SchedulerWebSocket::ConnectedWebSocket)
        select.exec();
    if (slaveWebSocket.state() != SchedulerWebSocket::ConnectedWebSocket) {
        DEBUG("Have to run locally because something went wrong with the slave");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    watchdog.transition(Watchdog::UploadedJob);

    // usleep(1000 * 500);
    // return 0;
    while (!slaveWebSocket.done && slaveWebSocket.state() == SchedulerWebSocket::ConnectedWebSocket)
        select.exec();
    if (!slaveWebSocket.done) {
        DEBUG("Have to run locally because something went wrong with the slave, part deux");
        watchdog.stop();
        Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
        return 0; // unreachable
    }

    if (!preprocessed->stdErr.empty()) {
        fwrite(preprocessed->stdErr.c_str(), sizeof(char), preprocessed->stdErr.size(), stderr);
    }
    watchdog.transition(Watchdog::Finished);
    watchdog.stop();
    schedulerWebsocket.close("slaved");

    return data.exitCode;
}
