#ifndef SCHEDULERWEBSOCKET_H
#define SCHEDULERWEBSOCKET_H

#include "WebSocket.h"
#include "Client.h"
#include "Watchdog.h"
#include <string>

class SchedulerWebSocket : public WebSocket
{
public:
    virtual void onConected() override
    {
        Client::data().watchdog->transition(Watchdog::ConnectedToScheduler);
    }
    virtual void onMessage(MessageType type, const void *data, size_t len) override
    {
        if (type == WebSocket::Text) {
            std::string err;
            json11::Json msg = json11::Json::parse(std::string(reinterpret_cast<const char *>(data), len), err, json11::JsonParse::COMMENTS);
            if (!err.empty()) {
                ERROR("Failed to parse json from scheduler: %s", err.c_str());
                Client::data().watchdog->stop();
                Client::runLocal(Client::acquireSlot(Client::Slot::Compile));
                return;
            }
            DEBUG("GOT JSON\n%s", msg.dump().c_str());
            const std::string type = msg["type"].string_value();
            if (type == "needsEnvironment") {
                needsEnvironment = true;
                done = true;
            } else if (type == "slave") {
                slaveIp = msg["ip"].string_value();
                slaveHostname = msg["hostname"].string_value();
                slavePort = msg["port"].int_value();
                jobId = msg["id"].int_value();
                Client::data().maintainSemaphores = msg["maintain_semaphores"].bool_value();
                DEBUG("type %d", msg["port"].type());
                DEBUG("Got here %s:%d", slaveIp.c_str(), slavePort);
                done = true;
            } else {
                ERROR("Unexpected message type: %s", type.c_str());
            }
            // } else {
            //     printf("Got binary message: %zu bytes\n", len);
        }
    }

    bool done { false };
    bool needsEnvironment { false };
    int jobId { 0 };
    uint16_t slavePort { 0 };
    std::string slaveIp, slaveHostname;
};


#endif /* SCHEDULERWEBSOCKET_H */
