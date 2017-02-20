/*global require*/

const rl = require('./build/Debug/native-readline.node');

function read(str)
{
    console.log("got", str);
}

function complete(data, cb)
{
    if (data.buffer.substr(data.start, data.end - data.start) == "foo") {
        cb(["foobar", "foobaz"]);
    } else {
        cb();
        console.log("complete", data);
    }
}

rl.start(read, complete);
