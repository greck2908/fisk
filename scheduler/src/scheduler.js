#!/usr/bin/env node

const path = require("path");
const os = require("os");
const option = require("@jhanssen/options")("fisk/scheduler");
const Server = require("./server");
const common = require('../../common')(option);
const Environments = require("./environments");
const server = new Server(option);

const slaves = {};

function distribute(conf)
{
    let ips;
    if (conf && conf.slave) {
        if (conf.pendingEnvironments)
            return;
        ips = [ conf.slave.ip ];
    } else {
        ips = Object.keys(slaves);
    }
    let hashes;
    if (conf && conf.hash) {
        hashes = [ conf.hash ];
    } else {
        hashes = Object.keys(Environments.environments);
    }
    console.log("distribute", ips, hashes);
    for (let h=0; h<hashes.length; ++h) {
        let hash = hashes[h];
        for (let i=0; i<ips.length; ++i) {
            let ip = ips[i];
            let slave = slaves[ip];
            if (!slave.pendingEnvironments && slave.environments && slave.environments.indexOf(hash) === -1) {
                console.log("sending", hash, "to", ip);
                Environments.environment(hash).send(slave);
                slave.pendingEnvironments = true;
                break;
            }
        }
    }
}

server.express.get("/slaves", (req, res, next) => {
    let ret = [];
    for (let ip in slaves) {
        let s = slaves[ip];
        ret.push({
            architecture: s.architecture,
            ip: s.ip,
            name: s.name,
            slots: s.slots,
            activeClients: s.activeClients,
            jobsScheduled: s.jobsScheduled,
            jobsPerformed: s.jobsPerformed,
            hostname: s.hostname,
            name: s.name,
            created: s.created,
            environments: s.environments
        });
    }
    res.send(ret);
});

server.on("slave", function(slave) {
    console.log("slave connected", slave.ip, slave.environments);
    slave.activeClients = 0;
    slave.pendingEnvironments = false;
    slaves[slave.ip] = slave;
    distribute({slave: slave});

    slave.on('environments', function(message) {
        slaves[slave.ip].environments = message.environments;
        slave.pendingEnvironments = false;
        distribute({slave: slave});
    });

    slave.on("error", function(msg) {
        console.error(`slave error '${msg}' from ${slave.ip}`);
    });
    slave.on("close", function() {
        delete slaves[slave.ip];
        slave.removeAllListeners();
    });

    slave.on("jobFinished", function(job) {
        ++slave.jobsPerformed;
        console.log("slave", slave.ip, "performed a job", job);
    });
});

server.on("compile", function(compile) {
    let file;
    let slave;
    compile.on("job", function(request) {
        console.log("request", request.environment);
        if (!Environments.hasEnvironment(request.environment)) {
            compile.send({ type: "needsEnvironment" });
            return;
        }

        function score(s) { if (!s) return -Infinity; return s.slots - s.activeClients; }
        for (let ip in slaves) {
            let s = slaves[ip];
            console.log(Object.keys(s));
            console.log(s.environments, request.environment, score(s), score(slave), s.slots, s.activeClients);
            if (s.environments.indexOf(request.environment) !== -1 && score(s) > score(slave)) {
                slave = s;
            }
        }
        if (slave) {
            console.log("Got best", slave.ip, score(slave));
            ++slave.activeClients;
            compile.send("slave", { ip: slave.ip, hostname: slave.hostname, port: slave.port });
        } else {
            compile.send("slave", {});
        }
    });
    compile.on("error", msg => {
        if (slave) {
            --slave.activeClients;
            slave = undefined;
        }
        console.error(`compile error '${msg}' from ${compile.ip}`);
    });
    compile.on("close", event => {
        console.log("CLIENT DISAPPEARED");
        compile.removeAllListeners();
        if (slave) {
            --slave.activeClients;
            slave = undefined;
        }
    });
});

server.on("uploadEnvironment", upload => {
    let file;
    let hash;
    upload.on("environment", environment => {
        file = Environments.prepare(environment);
        console.log("Got environment message", environment, typeof file);
        if (!file) {
            // we already have this environment
            console.error("already got environment", environment.message);
            upload.send({ error: "already got environment" });
            upload.close();
        } else {
            hash = environment.hash;
        }
    });
    upload.on("environmentdata", environment => {
        if (!file) {
            console.error("no pending file");
            upload.send({ error: "no pending file" });
            upload.close();
        }
        console.log("Got environmentdata message", environment.data.length, environment.last);
        file.save(environment.data).then(() => {
            if (environment.last) {
                file.close();
                upload.close();
                Environments.complete(file);
                file = undefined;
                // send any new environments to slaves
                distribute({hash: hash});
            }
        }).catch(err => {
            console.log("file error", err);
            file = undefined;
        });
    });
    upload.on("error", msg => {
        console.error(`upload error '${msg}' from ${upload.ip}`);
        if (file) {
            file.discard();
            file = undefined;
        }
    });
    upload.on("close", () => {
        upload.removeAllListeners();
        if (file) {
            file.discard();
            file = undefined;
        }
    });
});

server.on("error", err => {
    console.error(`error '${err.message}' from ${err.ip}`);
});

Environments.load(option("env-dir", path.join(common.cacheDir(), "environments"))).then(() => {
    server.listen();
}).catch(e => {
    console.error(e);
    process.exit();
});
