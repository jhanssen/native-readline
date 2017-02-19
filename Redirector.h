#ifndef REDIRECTOR_H
#define REDIRECTOR_H

#include <stdio.h>

class Redirector
{
public:
    Redirector();
    ~Redirector();

    int stdout() const { return mStdout.pipe[0]; }
    int stderr() const { return mStderr.pipe[0]; }

    FILE* stdoutFile() const { return mStdout.file; }
    FILE* stderrFile() const { return mStderr.file; }

    void writeStdout(const char* data, int len = -1);
    void writeStderr(const char* data, int len = -1);

private:
    struct Dup
    {
        int real;
        int pipe[2];
        FILE* file;
    };

    Dup mStdout, mStderr;
};

#endif
