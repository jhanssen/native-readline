/*global require*/

const rl = require('./build/Debug/native-readline.node');

function read(str)
{
    console.log("got", str);
}

function complete()
{
}

rl.start(read, complete);
