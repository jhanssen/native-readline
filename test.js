/*global require*/

const rl = require('./build/Debug/native-readline.node');

function read(str)
{
    console.log("got", str);
}

function complete(data, cb)
{
    console.log("complete", data);
    cb();
}

rl.start(read, complete);
