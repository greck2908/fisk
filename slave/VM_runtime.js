const path = require('path');
const posix = require('posix');
const Compile = require('./compile');

const argv = require('minimist')(process.argv.slice(2));

let pwd;
if (argv.user) {
    try {
        pwd = posix.getpwnam(argv.user);
    } catch(err) {
        console.error("Couldn't find user", argv.user);
        throw err;
    }

    try {
        process.initgroups(argv.user, pwd.gid);
    } catch(err) {
        throw new Error('changing groups failed: ' + err.message);
    }
}

try {
    console.log("chrooting to", argv.root);
    posix.chroot(argv.root);
} catch (err) {
    console.error('changing root or user failed', err);
    process.exit(1);
}

if (pwd) {
    process.setgid(pwd.gid);
    process.setuid(pwd.uid);
}

let compiles = {};
let stopping = false;

process.on('message', (msg) => {
    switch (msg.type) {
    case 'stop':
        if (!compiles.length)
            process.exit();
        break;
    case 'cancel':
        let c = compiles[msg.id];
        if (c)
            c.kill();
        break;
    case 'compile':
        console.log("GOT COMPILE", msg);
        let compile = new Compile(msg.commandLine, msg.argv0, msg.dir);
        compile.on('stdout', data => process.send({ type: 'compileStdOut', id: msg.id, data: data }));
        compile.on('stderr', data => process.send({ type: 'compileStdErr', id: msg.id, data: data }));
        compile.on('exit', event => {
            delete compiles[msg.id];
            process.send({type: 'compileFinished', id: msg.id, files: event.files, exitCode: event.exitCode, sourceFile: event.sourceFile });
            if (stopping && !compiles.length)
                process.exit();
        });
        compiles[msg.id] = compile;
        break;
    }
});