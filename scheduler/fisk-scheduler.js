#!/usr/bin/env node

const path = require("path");
const os = require("os");
const option = require("@jhanssen/options")("fisk/scheduler");
const Server = require("./server");
const common = require("../common")(option);
const Environments = require("./environments");
const server = new Server(option, common.Version);
const fs = require("fs-extra");
const bytes = require("bytes");
const crypto = require("crypto");
const Database = require("./database");

process.on("unhandledRejection", (reason, p) => {
    console.log("Unhandled Rejection at: Promise", p, "reason:", reason.stack);
});

const slaves = {};
const monitors = [];
let slaveCount = 0;
let activeJobs = 0;
let jobId = 0;
let db = new Database(path.join(common.cacheDir(), "db.json"));
let pendingUsers = {};
let compatibleHashes = {};

function slaveKey() {
    if (arguments.length == 1) {
        return arguments[0].ip + " " + arguments[0].port;
    } else {
        return arguments[0] + " " + arguments[1];
    }
}

function slaveToMonitorInfo(slave, type)
{
    return {
        type: type,
        ip: slave.ip,
        name: slave.name,
        hostname: slave.hostname,
        slots: slave.slots,
        port: slave.port,
        jobsPerformed: slave.jobsPerformed,
        compileSpeed: slave.jobsPerformed / slave.totalCompileSpeed || 0,
        uploadSpeed: slave.jobsPerformed / slave.totalUploadSpeed || 0,
        system: slave.system,
        created: slave.created,
        npmVersion: slave.npmVersion,
        environments: Object.keys(slave.environments)
    };
}

function insertSlave(slave) {
    slaves[slaveKey(slave)] = slave;
    ++slaveCount;
    if (monitors.length) {
        const info = slaveToMonitorInfo(slave, "slaveAdded");
        monitors.forEach(monitor => {
            monitor.send(info);
        });
    }
}

function forEachSlave(cb) {
    for (let key in slaves) {
        cb(slaves[key]);
    }
}

function removeSlave(slave) {
    --slaveCount;
    delete slaves[slaveKey(slave)];
    if (monitors.length) {
        const info = slaveToMonitorInfo(slave, "slaveRemoved");
        monitors.forEach(monitor => {
            monitor.send(info);
        });
    }

}

function findSlave(ip, port) {
    return slaves(slaveKey(ip, port));
}

function purgeEnvironmentsToMaxSize()
{
    return new Promise((resolve, reject) => {
        let maxSize = bytes.parse(option("max-cache-size"));
        if (!maxSize) {
            resolve(false);
            return;
        }
        const p = Environments._path;
        try {
            let purged = false;
            fs.readdirSync(p).map(file => {
                console.log("got file", file);
                let match = /^([^:]*):([^:]*):([^:]*).tar.gz$/.exec(file);
                if (!match)
                    return undefined;
                var abs = path.join(p, file);
                var stat;
                try {
                    stat = fs.statSync(abs);
                } catch (err) {
                    return undefined;
                }
                return {
                    path: abs,
                    hash: match[1],
                    size: stat.size,
                    created: stat.birthtimeMs
                };
            }).sort((a, b) => {
                // console.log(`comparing ${a.path} ${a.created} to ${b.path} ${b.created}`);
                return b.created - a.created;
            }).forEach(env => {
                if (!env)
                    return;
                if (maxSize >= env.size) {
                    maxSize -= env.size;
                    return;
                }
                purged = true;
                Environments.remove(env.hash);
                console.log("Should purge env", env.hash);
            });
            resolve(purged);
        } catch (err) {
            resolve(false);
            return;
        }
    });
}

function syncEnvironments(slave)
{
    if (!slave) {
        forEachSlave(syncEnvironments);
        return;
    }
    var needs = [];
    var unwanted = [];
    console.log("scheduler has", Object.keys(Environments.environments).sort());
    console.log("slave has", Object.keys(slave.environments).sort());
    for (let env in Environments.environments) {
        if (env in slave.environments) {
            slave.environments[env] = -1;
        } else {
            needs.push(env);
        }
    }
    for (let env in slave.environments) {
        if (slave.environments[env] != -1) {
            unwanted.push(env);
            delete slave.environments[env];
        } else {
            slave.environments[env] = true;
        }
    }
    console.log("unwanted", unwanted);
    console.log("needs", needs);
    if (unwanted.length) {
        slave.send({ type: "dropEnvironments", environments: unwanted });
    }
    if (needs.length) {
        needs.forEach(env => Environments.environments[env].send(slave));
        // Environments.requestEnvironments(slave);
    }
}

function environmentsInfo()
{
    let ret = JSON.stringify(Environments.environments);
    ret.maxSize = option("max-cache-size") || 0;
    ret.maxSizeBytes = bytes.parse(option("max-cache-size"));
    ret.usedSizeBytes = 0;
    for (let hash in Environments.environments) {
        let env = Environments.environments[hash];
        if (env.size)
            ret.usedSizeBytes += env.size;
    }
    ret.usedSize = bytes.format(ret.usedSizeBytes);
    return ret;
}

server.express.get("/environments", (req, res, next) => {

    res.send(environmentsInfo());
});

server.express.get("/slaves", (req, res, next) => {
    let ret = [];
    for (let ip in slaves) {
        let s = slaves[ip];
        ret.push({
            ip: s.ip,
            name: s.name,
            slots: s.slots,
            port: s.port,
            activeClients: s.activeClients,
            jobsScheduled: s.jobsScheduled,
            lastJob: s.lastJob ? new Date(s.lastJob).toString() : "",
            jobsPerformed: s.jobsPerformed,
            compileSpeed: s.jobsPerformed / s.totalCompileSpeed || 0,
            uploadSpeed: s.jobsPerformed / s.totalUploadSpeed || 0,
            hostname: s.hostname,
            system: s.system,
            name: s.name,
            created: s.created,
            load: s.load,
            npmVersion: s.npmVersion,
            environments: Object.keys(s.environments)
        });
    }
    res.send(ret);
});

server.express.get("/info", (req, res, next) => {
    let npmVersion = -1;
    try {
        npmVersion = JSON.parse(fs.readFileSync(path.join(__dirname, "../package.json"))).version;
    } catch (err) {
        console.log("Couldn't parse package json", err);
    }

    res.send({ npmVersion: npmVersion, environments: environmentsInfo(), configVersion: common.Version });
});

server.express.get("/quit-slaves", (req, res, next) => {
    res.sendStatus(200);
    const msg = {
        type: "quit",
        code: req.query.code || 0,
        purgeEnvironments: "purge_environments" in req.query
    };
    console.log("Sending quit message to slaves", msg, Object.keys(slaves));
    for (let ip in slaves) {
        slaves[ip].send(msg);
    }
});

server.express.get("/quit", (req, res, next) => {
    console.log("quitting", req.query);
    if ("purge_environments" in req.query) {
        try {
            fs.removeSync(path.join(common.cacheDir(), "environments"));
        } catch (err) {
        }
    }
    res.sendStatus(200);
    setTimeout(() => process.exit(), 100);
});

server.on("slave", function(slave) {
    slave.activeClients = 0;
    insertSlave(slave);
    console.log("slave connected", slave.ip, slave.name || "", slave.hostname || "", Object.keys(slave.environments), "slaveCount is", slaveCount);
    syncEnvironments(slave);

    slave.on("environments", function(message) {
        slave.environments = {};
        message.environments.forEach(env => slave.environments[env] = true);
        syncEnvironments(slave);
    });

    slave.on("error", function(msg) {
        console.error(`slave error '${msg}' from ${slave.ip}`);
    });
    slave.on("close", function() {
        removeSlave(slave);
        console.log("slave disconnected", slave.ip, slave.name || "", slave.hostname || "", "slaveCount is", slaveCount);
        slave.removeAllListeners();
    });

    slave.on("load", message => {
        slave.load = message.measure;
        // console.log(message);
    });

    slave.on("jobFinished", function(job) {
        ++slave.jobsPerformed;
        slave.totalCompileSpeed += job.compileSpeed;
        slave.totalUploadSpeed += job.uploadSpeed;
        console.log(`slave: ${slave.ip}:${slave.port} performed a job`, job);
        if (monitors.length) {
            const info = {
                type: "jobFinished",
                id: job.id,
                cppSize: job.cppSize,
                compileDuration: job.compileDuration,
                compileSpeed: job.compileSpeed,
                uploadDuration: job.uploadDuration,
                uploadSpeed: job.uploadSpeed
            };

            monitors.forEach(monitor => monitor.send(info));
        }
    });

    slave.on("jobAborted", function(job) {
        console.log(`slave: ${slave.ip}:${slave.port} aborted a job`, job);
        if (monitors.length) {
            const info = {
                type: "jobAborted",
                id: job.id
            };

            monitors.forEach(monitor => monitor.send(info));
        }
    });

});

let semaphoreMaintenanceTimers = {};
let pendingEnvironments = {};
server.on("compile", function(compile) {
    let arrived = Date.now();
    console.log("request", compile.hostname, compile.ip, compile.environment);
    if (!Environments.hasEnvironment(compile.environment)) {
        if (compile.environment in pendingEnvironments) {
            console.log(`We're already waiting for ${compile.environment}`);
            compile.send("slave", {});
            return;
        }
        pendingEnvironments[compile.environment] = true;
        console.log(`Asking ${compile.name} ${compile.ip} to upload ${needed}`);
        compile.send({ type: "needsEnvironment", environment: compile.environment });

        let file;
        let gotLast = false;
        compile.on("uploadEnvironment", environment => {
            file = Environments.prepare(environment);
            console.log("Got environment message", environment, typeof file);
            if (!file) {
                // we already have this environment
                console.error("already got environment", environment.message);
                compile.send({ error: "already got environment" });
                compile.close();
                return;
            }
            let hash = environment.hash;
            compile.on("uploadEnvironmentData", environment => {
                if (!file) {
                    console.error("no pending file");
                    compile.send({ error: "no pending file" });
                    compile.close();
                    return;
                }
                if (environment.last) {
                    gotLast = true;
                    console.log("Got environmentdata message", environment.data.length, environment.last);
                }
                file.save(environment.data).then(() => {
                    if (environment.last) {
                        file.close();
                        compile.close();
                        Environments.complete(file);
                        file = undefined;
                        // send any new environments to slaves
                        delete pendingEnvironments[hash];
                        purgeEnvironmentsToMaxSize().then(() => {
                            syncEnvironments();
                        }).catch(error => {
                            console.error("Got some error here", error);
                        });
                    }
                }).catch(err => {
                    console.log("file error", err);
                    file = undefined;
                });
            });
        });
        compile.on("error", msg => {
            console.error(`upload error '${msg}' from ${compile.ip}`);
            if (file) {
                file.discard();
                file = undefined;
            }
            delete pendingEnvironments[compile.environment];
        });
        compile.once("close", () => {
            if (file && !gotLast) {
                console.log("compile with upload closed", needed, "discarding");
                file.discard();
                file = undefined;
            }
            delete pendingEnvironments[compile.environment];
        });

        return;
    }
    let environments = [ compile.environment ];
    if (compatibleHashes[compile.environment]) {
        environments = environments.concat(Object.keys(compatibleHashes[compile.environment]));
    }

    function score(s) {
        let available = Math.min(4, s.slots - s.activeClients);
        return available * (1 - s.load);
    }
    let file;
    let slave;
    let bestScore;
    forEachSlave(s => {
        found = false;
        for (let i=0; i<environments.length; ++i) {
            if (environments[i] in s.environments) {
                found = true;
                break;
            }
        }

        // console.log(`Any finds for ${environments} ${found}`);

        if (found) {
            let slaveScore;
            // console.log("Got compile.slave", compile.slave, s.ip);
            if (compile.slave && (compile.slave == s.ip || compile.slave == s.name)) {
                slaveScore = Infinity;
            } else {
                slaveScore = score(s);
            }
            // console.log("comparing", slaveScore, bestScore);
            if (!slave || slaveScore > bestScore || (slaveScore == bestScore && s.lastJob < slave.lastJob)) {
                bestScore = slaveScore;
                slave = s;
            }
        // } else {
        //     console.log("Dude't havethe compiler", s.ip);
        }
    });
    let data = {};
    // console.log("WE'RE HERE", Object.keys(semaphoreMaintenanceTimers), compile.ip);
    if (compile.ip in semaphoreMaintenanceTimers) {
        clearTimeout(semaphoreMaintenanceTimers[compile.ip]);
    } else {
        data["maintain_semaphores"] = true;
    }
    semaphoreMaintenanceTimers[compile.ip] = setTimeout(() => {
        delete semaphoreMaintenanceTimers[compile.ip];
    }, 60 * 60000);

    if (slave) {
        ++activeJobs;
        let sendTime = Date.now();
        console.log(`${compile.name} ${compile.ip} ${compile.sourceFile} got slave ${slave.ip} ${slave.port} ${slave.name} score: ${bestScore} active jobs is ${activeJobs} arrived ${arrived} chewed for ${sendTime - arrived}`);
        ++slave.activeClients;
        ++slave.jobsScheduled;
        slave.lastJob = Date.now();
        let id = ++jobId;
        if (id == 2147483647)
            id = 0;
        data.id = id;
        data.ip = slave.ip;
        data.hostname = slave.hostname;
        data.port = slave.port;
        compile.send("slave", data);
        if (monitors.length) {
            let info = {
                type: "jobStarted",
                client: {
                    hostname: compile.hostname,
                    ip: compile.ip,
                    name: compile.name
                },
                sourceFile: compile.sourceFile,
                slave: {
                    hostname: slave.hostname,
                    ip: slave.ip,
                    name: slave.name,
                    port: slave.port
                },
                id: id
            };

            monitors.forEach(monitor => monitor.send(info));
        }

    } else {
        console.log("No slave for you", compile.ip);
        compile.send("slave", data);
    }
    compile.on("error", msg => {
        if (slave) {
            --slave.activeClients;
            --activeJobs;
            slave = undefined;
        }
        console.error(`compile error '${msg}' from ${compile.ip}`);
    });
    compile.on("close", event => {
        // console.log("Client disappeared");
        compile.removeAllListeners();
        if (slave) {
            --slave.activeClients;
            --activeJobs;
            slave = undefined;
        }
    });
});

function writeConfiguration(change)
{
    return new Promise((resolve, reject) => {
        if ((change.add instanceof Array) == (change.remove instanceof Array)) {
            reject(new Error("Bad change"));
        }
        switch (change.field) {
        case "compatibleHash":
            if (change.add) {
                if (change.add.length != 2)
                    throw new Error("Bad change");
                if (!compatibleHashes[change.add[0]])
                    compatibleHashes[change.add[0]] = {};
                compatibleHashes[change.add[0]][change.add[1]] = true;
                if (!compatibleHashes[change.add[1]])
                    compatibleHashes[change.add[1]] = {};
                compatibleHashes[change.add[1]][change.add[0]] = true;
            } else { // change.remove
                if (change.remove.length != 2)
                    throw new Error("Bad change");
                if (compatibleHashes[change.add[0]]) {
                    delete compatibleHashes[change.add[0]][change.add[1]];
                    if (!Object.length(compatibleHashes[change.add[0]])) {
                        delete compatibleHashes[change.add[0]];
                    }
                }
                if (compatibleHashes[change.add[1]]) {
                    delete compatibleHashes[change.add[1]][change.add[0]];
                    if (!Object.length(compatibleHashes[change.add[1]])) {
                        delete compatibleHashes[change.add[1]];
                    }
                }
            }
            return db.set("compatible_hashes", compatibleHashes);
        }
    });
}

function readConfiguration()
{
    return { compatibleHashes: compatibleHashes };
}

function hash(password, salt)
{
    return new Promise((resolve, reject) => {
        crypto.pbkdf2(password, salt, 12000, 256, "sha512", (err, hash) => {
            if (err) {
                reject(err);
            } else {
                resolve(hash);
            }
        });
    });
};

function randomBytes(bytes)
{
    return new Promise((resolve, reject) => {
        crypto.randomBytes(bytes, (err, result) => {
            if (err) {
                reject(`Failed to random bytes ${err}`);
            } else {
                resolve(result);
            }
        });
    });
}

server.on("monitor", client => {
    monitors.push(client);
    function remove()
    {
        var idx = monitors.indexOf(client);
        if (idx != -1) {
            monitors.splice(idx, 1);
        }
        client.removeAllListeners();
    }
    forEachSlave(slave => {
        client.send(slaveToMonitorInfo(slave, "slaveAdded"));
    });
    let user;
    client.on("message", messageText => {
        // console.log("GOT MESSAGE", messageText);
        let message;
        try {
            message = JSON.parse(messageText);
        } catch (err) {
            client.send({ success: false, error: `Bad message won't parse as JSON: ${err}` });
            client.close();
            return;
        }
        switch (message.type) {
        case 'readConfiguration':
            try {
                let conf = readConfiguration(message);
                client.send({ type: "readConfiguration", success: true, configuration: conf });
            } catch (err) {
                client.send({ type: "readConfiguration", success: false, "error": err.toString() });
            }
            break;
        case 'writeConfiguration':
            if (!user) {
                client.send({ type: "writeConfiguration", success: false, "error": `Unauthenticated message: ${message.type}` });
                return;
            }
            writeConfiguration(message).then(() => {
                client.send({ type: "writeConfiguration", success: true });
            }).catch(err => {
                client.send({ type: "writeConfiguration", success: false, "error": err.toString() });
            });
            break;
        case 'listUsers': {
            if (!user) {
                client.send({ type: "listUsers", success: false, "error": `Unauthenticated message: ${message.type}` });
                return;
            }
            db.get("users").then(users => {
                if (!users)
                    users = {};
                client.send({ type: "listUsers", success: true, users: Object.keys(users) });
            }).catch(err => {
                console.error(`Something went wrong ${message.type} ${err.toString()} ${err.stack}`);
                client.send({ type: "listUsers", success: false, error: err.toString() });
            });
            break; }
        case 'removeUser': {
            if (!user) {
                client.send({ type: "removeUser", success: false, "error": `Unauthenticated message: ${message.type}` });
                return;
            }
            if (!message.user) {
                client.send({ type: "removeUser", success: false, error: "Bad removeUser message" });
                return;
            }

            if (pendingUsers[message.user]) {
                client.send({ type: "removeUser", success: false, error: "Someone's here already" });
                return;
            }
            pendingUsers[message.user] = true;
            let users;
            db.get("users").then(users => {
                if (!users || !users[message.user]) {
                    throw new Error(`user ${message.user} doesn't exist`);
                }
                delete users[message.user];
                return db.set("users", users);
            }).then(() => {
                client.send({ type: "removeUser", success: true, user: message.user });
            }).catch(err => {
                console.error(`Something went wrong ${message.type} ${err.toString()} ${err.stack}`);
                client.send({ type: "removeUser", success: false, error: err.toString() });
            }).finally(() => {
                delete pendingUsers[message.user];
            });

            // console.log("gotta remove user", message);
            break; }
        case 'login': {
            user = undefined;
            if (!message.user || (!message.password && !message.hmac)) {
                client.send({ type: "login", success: false, error: "Bad login message" });
                return;
            }
            let users;
            db.get("users").then(u => {
                users = u || {};
                if (!users[message.user]) {
                    throw new Error(`User: ${message.user} does not seem to exist`);
                }
                if (message.hmac) {
                    if (!users[message.user].cookie) {
                        throw new Error("No cookie");
                    } else if (users[message.user].cookieIp != client.ip) {
                        throw new Error("Wrong ip address");
                    } else if (users[message.user].cookieExpiration <= Date.now()) {
                        throw new Error("Cookie expired");
                    } else {
                        const hmac = crypto.createHmac("sha512", Buffer.from(users[message.user].cookie, "base64"));
                        hmac.write(client.nonce);
                        hmac.end();
                        const hmacString = hmac.read().toString("base64");
                        if (hmacString != message.hmac) {
                            throw new Error(`Wrong password ${message.user}`);
                        };
                        return undefined;
                    }
                } else {
                    return hash(message.password, Buffer.from(users[message.user].salt, "base64")).then(hash => {
                        if (users[message.user].hash != hash.toString('base64')) {
                            throw new Error(`Wrong password ${message.user}`);
                        }
                    });
                }
            }).then(() => {
                return randomBytes(256);
            }).then(cookie => {
                user = message.user;
                const expiration = new Date(Date.now() + 12096e5);
                users[message.user].cookie = cookie.toString("base64");
                users[message.user].cookieIp = client.ip;
                users[message.user].cookieExpiration = expiration.valueOf();
                return db.set("users", users);
            }).then(() => {
                client.send({ type: "login", success: true, user: message.user, cookie: users[message.user].cookie });
            }).catch(err => {
                console.error(`Something went wrong ${message.type} ${err.toString()} ${err.stack}`);
                client.send({ type: "login", success: false, error: err.toString() });
            });
            break; }
        case 'addUser': {
            if (!message.user || !message.password) {
                client.send({ type: "addUser", success: false, error: "Bad addUser message" });
                return;
            }
            if (pendingUsers[message.user]) {
                client.send({ type: "addUser", success: false, error: "Someone's here already" });
                return;
            }
            pendingUsers[message.user] = true;
            let users;
            db.get("users").then(u => {
                users = u || {};
                if (users[message.user]) {
                    throw new Error(`user ${message.user} already exists`);
                }
                return randomBytes(256);
            }).then(salt => {
                users[message.user] = { salt: salt.toString("base64") };
                return hash(message.password, salt);
            }).then(hash => {
                users[message.user].hash = hash.toString("base64");
                return randomBytes(256);
            }).then(cookie => {
                users[message.user].cookie = cookie.toString("base64");
                users[message.user].cookieExpiration = (Date.now() + 12096e5);
                users[message.user].cookieIp = client.ip;
                return db.set("users", users);
            }).then(() => {
                // console.log("here", values);
                // values = [1,2];
                client.send({ type: "addUser",
                              success: true,
                              user: message.user,
                              cookie: users[message.user].cookie });
            }).catch(err => {
                console.error(`Something went wrong ${message.type} ${err.toString()} ${err.stack}`);
                client.send({ type: "addUser", success: false, error: err.toString() });
            }).finally(() => {
                delete pendingUsers[message.user];
            });

            // console.log("gotta add user", message);
            break; }
        }
    });
    client.on("close", remove);
    client.on("error", remove);
});

server.on("error", err => {
    console.error(`error '${err.message}' from ${err.ip}`);
});

Environments.load(option("env-dir", path.join(common.cacheDir(), "environments")))
    .then(purgeEnvironmentsToMaxSize)
    .then(() => {
        return db.get("compatible_hashes");
    }).then(h => {
        compatibleHashes = h || {};
        console.log("got compatible hashes", compatibleHashes);
    })
    .then(() => server.listen())
    .catch(e => {
        console.error(e);
        process.exit();
    });
