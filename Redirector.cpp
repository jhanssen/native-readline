#include "Redirector.h"
#include "utils.h"
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

Redirector::Redirector()
    : mPaused(false)
{
    // should make this a bit more resilient against errors

    // first dup our real stdout and stderr
    mStdout.real = dup(STDOUT_FILENO);
    mStderr.real = dup(STDERR_FILENO);

    // make two pipes
    ::pipe(mStdout.pipe);
    ::pipe(mStderr.pipe);

    // make the read end non-blocking
    int r = fcntl(mStdout.pipe[0], F_GETFL);
    fcntl(mStdout.pipe[0], F_SETFL, r | O_NONBLOCK);
    r = fcntl(mStderr.pipe[0], F_GETFL);
    fcntl(mStderr.pipe[0], F_SETFL, r | O_NONBLOCK);

    // dup our stdout and stderr fds
    dup2(mStdout.pipe[1], STDOUT_FILENO);
    dup2(mStderr.pipe[1], STDERR_FILENO);

    // make our file ptrs
    mStdout.file = fdopen(mStdout.real, "w");
    mStderr.file = fdopen(mStderr.real, "w");
}

Redirector::~Redirector()
{
    // close the pipes
    ::close(mStdout.pipe[0]);
    ::close(mStdout.pipe[1]);
    ::close(mStderr.pipe[0]);
    ::close(mStderr.pipe[1]);

    // restore our file descriptors
    dup2(mStdout.real, STDOUT_FILENO);
    dup2(mStderr.real, STDERR_FILENO);

    // and close our file ptrs, not sure if this is needed
    fclose(mStdout.file);
    fclose(mStderr.file);
}

void Redirector::writeStdout(const char* data, int len)
{
    if (len == -1)
        len = strlen(data);
    int w;
    EINTRWRAP(w, write(mStdout.real, data, len));
}

void Redirector::writeStderr(const char* data, int len)
{
    if (len == -1)
        len = strlen(data);
    int w;
    EINTRWRAP(w, write(mStdout.real, data, len));
}

void Redirector::pause()
{
    if (mPaused)
        return;
    mPaused = true;
    // restore fds
    dup2(mStdout.real, STDOUT_FILENO);
    dup2(mStderr.real, STDERR_FILENO);
}

void Redirector::resume()
{
    if (!mPaused)
        return;
    mPaused = false;
    // restore fds
    dup2(mStdout.pipe[1], STDOUT_FILENO);
    dup2(mStderr.pipe[1], STDERR_FILENO);
}
