#include <nan.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "utils.h"
#include "Redirector.h"
#include <errno.h>
#include <functional>

// #define LOG

struct State
{
    State() : started(false), paused(false), stopped(false), savedLine(0), savedPoint(0) { }

    v8::Isolate* iso;

    bool started;
    bool paused;
    uv_thread_t thread;
    int wakeupPipe[2];
    Redirector redirector;
    std::string prompt;
#ifdef LOG
    FILE* log;
#endif

    Mutex mutex;
    bool stopped;

    struct {
        Nan::Persistent<v8::Function> function;
        uv_async_t async;

        Queue<std::string> lines;
    } readline;

    struct {
        Nan::Persistent<v8::Function> function;
        Nan::Persistent<v8::Function> callback;
        uv_async_t async;

        struct {
            const char* buffer;
            const char* text;
            int start, end;
        } pending;
        std::vector<std::string> results;

        Mutex mutex;
        Condition condition;
    } completion;

    static void run(void* arg);

    bool init();
    void cleanup();

    enum WakeupReason {
        WakeupStop,
        WakeupPause,
        WakeupResume,
        WakeupPrompt
    };
    void wakeup(WakeupReason reason);
    uv_async_t pauseAsync, resumeAsync, promptAsync;
    Nan::Callback pauseCb, resumeCb, promptCb;

    // pause state
    char* savedLine;
    int savedPoint;

    void saveState();
    void restoreState();
};

static State state;

bool State::init()
{
    state.iso = v8::Isolate::GetCurrent();
#ifdef LOG
    state.log = fopen("/tmp/nrl.log", "w");
#endif

    int r = pipe(state.wakeupPipe);
    if (r == -1) {
        // badness
        state.wakeupPipe[0] = state.wakeupPipe[1] = -1;
        return false;
    }
    r = fcntl(state.wakeupPipe[0], F_GETFL);
    if (r == -1) {
        // horribleness
        return false;
    }
    fcntl(state.wakeupPipe[0], F_SETFL, r | O_NONBLOCK);

    return true;
}

void State::cleanup()
{
    wakeup(WakeupStop);
    uv_thread_join(&state.thread);

    if (state.wakeupPipe[0] != -1)
        close(state.wakeupPipe[0]);
    if (state.wakeupPipe[1] != -1)
        close(state.wakeupPipe[1]);

#ifdef LOG
    fclose(state.log);
#endif
}

void State::wakeup(WakeupReason reason)
{
    int e;
    char r = reason;
    EINTRWRAP(e, ::write(state.wakeupPipe[1], &r, 1));
}

void State::saveState()
{
    if (state.savedLine)
        return;
    state.savedPoint = rl_point;
    state.savedLine = rl_copy_text(0, rl_end);
    rl_save_prompt();
    rl_replace_line("", 0);
    rl_redisplay();
}

void State::restoreState()
{
    if (!state.savedLine)
        return;
    rl_restore_prompt();
    rl_replace_line(state.savedLine, 0);
    rl_point = state.savedPoint;
    rl_redisplay();
    free(state.savedLine);
    state.savedLine = 0;
}

static void handleOut(int fd, const std::function<void(const char*, int)>& write)
{
    bool saved = false;

    // read until the end of time
    char buf[16384];
    for (;;) {
        const ssize_t r = read(fd, buf, sizeof(buf));
        if (r == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            // badness!
            return;
        } else if (!r) {
            // done?
            break;
        } else {
            if (!saved && !state.paused) {
                state.saveState();
                saved = true;
            }
            write(buf, r);

#ifdef LOG
            fprintf(state.log, "wrote '%s' \n", std::string(buf, r).c_str());
            fflush(state.log);
#endif
        }
    }
    if (saved)
        state.restoreState();
}

void State::run(void* arg)
{
    state.stopped = false;

    auto handler = [](char* line) {
        if (!line) {
            // we're done
            MutexLocker locker(&state.mutex);
            state.stopped = true;
            return;
        }
        state.readline.lines.push(line);
        uv_async_send(&state.readline.async);
    };
    auto completer = [](const char* text, int start, int end) -> char** {
        // ### if we want file completion, just return nullptr before setting this variable
        rl_attempted_completion_over = 1;

        //state.redirector.writeStdout("precomplete\n");
        MutexLocker locker(&state.completion.mutex);
        state.completion.pending = { rl_line_buffer, text, start, end };
        uv_async_send(&state.completion.async);
        state.completion.condition.wait(&state.completion.mutex);
        //state.redirector.writeStdout("postcomplete\n");

        // loop through results and make a char**
        if (!state.completion.results.empty()) {
            char** array = static_cast<char**>(malloc((2 + state.completion.results.size()) * sizeof(*array)));
            array[0] = strdup(longest_common_prefix(text, state.completion.results).c_str());
            size_t ptr = 1;
            for (const auto& m : state.completion.results) {
                array[ptr++] = strdup(m.c_str());
            }
            array[ptr] = nullptr;
            return array;
        }
        return nullptr;
    };

    rl_outstream = state.redirector.stdoutFile();
    rl_callback_handler_install(state.prompt.c_str(), handler);
    rl_attempted_completion_function = completer;

    const int stdoutfd = state.redirector.stdout();
    const int stderrfd = state.redirector.stderr();

    fd_set rdset;
    int max = state.wakeupPipe[0];
    if (STDIN_FILENO > max)
        max = STDIN_FILENO;
    if (stdoutfd > max)
        max = stdoutfd;
    if (stderrfd > max)
        max = stderrfd;

    const auto stdoutfunc = std::bind(&Redirector::writeStdout, &state.redirector, std::placeholders::_1, std::placeholders::_2);
    const auto stderrfunc = std::bind(&Redirector::writeStderr, &state.redirector, std::placeholders::_1, std::placeholders::_2);

    //uv_loop_t* loop = static_cast<uv_loop_t*>(arg);
    bool pendingPause = false;
    for (;;) {
        // we need to wait on both wakeupPipe[0] and stdin
        FD_ZERO(&rdset);
        FD_SET(state.wakeupPipe[0], &rdset);
        if (!state.paused)
            FD_SET(STDIN_FILENO, &rdset);
        FD_SET(stdoutfd, &rdset);
        FD_SET(stderrfd, &rdset);
        const int r = select(max + 1, &rdset, 0, 0, 0);
        if (r <= 0) {
            // boo
            break;
        }
        if (FD_ISSET(state.wakeupPipe[0], &rdset)) {
            // do stuff
            // read everything on our pipe
            char c, r;
            bool stopped = false;
            for (;;) {
                EINTRWRAP(r, read(state.wakeupPipe[0], &c, 1));
                if (r == -1)
                    break;
                if (r == 1) {
                    switch (c) {
                    case WakeupStop:
                        stopped = true;
                        break;
                    case WakeupPause:
                        pendingPause = true;
                        break;
                    case WakeupResume:
                        if (state.paused) {
                            state.paused = false;
                            rl_callback_handler_install(state.prompt.c_str(), handler);
                            state.restoreState();
                        }
                        uv_async_send(&state.resumeAsync);
                        break;
                    case WakeupPrompt:
                        if (!state.paused)
                            rl_redisplay();
                        uv_async_send(&state.promptAsync);
                        break;
                    }
                }
            }
            if (stopped)
                break;
        }
        if (FD_ISSET(stdoutfd, &rdset)) {
            handleOut(stdoutfd, stdoutfunc);
        }
        if (FD_ISSET(stderrfd, &rdset)) {
            handleOut(stderrfd, stderrfunc);
        }
        if (FD_ISSET(STDIN_FILENO, &rdset) && !state.paused) {
            // read until we have nothing more to read
            if (r == -1) {
                // ugh
                break;
            }
            bool error = false;
            int rem;
            for (;;) {
                rl_callback_read_char();
                // loop while we have more characters
                if (ioctl(STDIN_FILENO, FIONREAD, &rem) == -1) {
                    // ugh
                    error = true;
                    break;
                }
                if (!rem)
                    break;
            }
            if (error)
                break;
        }

        if (pendingPause) {
            if (!state.paused) {
                state.paused = true;
                state.saveState();
                rl_callback_handler_remove();
            }
            uv_async_send(&state.pauseAsync);
            pendingPause = false;
        }

        {
            MutexLocker locker(&state.mutex);
            if (state.stopped)
                break;
        }
    }

    rl_callback_handler_remove();
}

NAN_METHOD(start) {
    // two arguments, output callback and complete callback
    if (state.started) {
        Nan::ThrowError("Already started. Stop first");
        return;
    }
    if (info.Length() < 2) {
        Nan::ThrowError("Start needs two arguments");
        return;
    }
    if (!info[0]->IsFunction() || !info[1]->IsFunction()) {
        Nan::ThrowError("First and second arguments need to be functions");
        return;
    }
    if (!state.init()) {
        Nan::ThrowError("Unable to init readline");
        return;
    }
    state.prompt = "foobar> ";
    state.readline.function.Reset(Nan::Persistent<v8::Function>(v8::Local<v8::Function>::Cast(info[0])));
    state.completion.function.Reset(Nan::Persistent<v8::Function>(v8::Local<v8::Function>::Cast(info[1])));

    {
        auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            // array of strings, put into results and notify our condition
            std::vector<std::string> results;

            if (info.Length() > 0 && info[0]->IsArray()) {
                v8::Handle<v8::Array> array = v8::Handle<v8::Array>::Cast(info[0]);
                for (uint32_t i = 0; i < array->Length(); ++i) {
                    results.push_back(*v8::String::Utf8Value(array->Get(i)));
                }
            }

            MutexLocker locker(&state.completion.mutex);
            state.completion.results = results;
            //state.redirector.writeStdout("signaling\n");
            state.completion.condition.signal();
        };
        auto cb = v8::Function::New(Nan::GetCurrentContext(), callback);
        state.completion.callback.Reset(Nan::Persistent<v8::Function>(cb.ToLocalChecked()));
    }

    auto cb = [](uv_async_t* async) {
        v8::Isolate::Scope isolateScope(state.iso);
        Nan::HandleScope scope;
        if (async == &state.readline.async) {
            auto iso = v8::Isolate::GetCurrent();
            v8::Handle<v8::Function> f = v8::Local<v8::Function>::New(iso, state.readline.function);

            bool ok;
            for (;;) {
                const std::string line = state.readline.lines.pop(&ok);
                if (!ok)
                    break;
                auto value = makeValue(line);
                f->Call(f, 1, &value);
            }
        } else if (async == &state.completion.async) {
            auto iso = v8::Isolate::GetCurrent();
            v8::Handle<v8::Function> f = v8::Local<v8::Function>::New(iso, state.completion.function);
            v8::Handle<v8::Function> cb = v8::Local<v8::Function>::New(iso, state.completion.callback);

            std::vector<v8::Handle<v8::Value> > values;
            v8::Handle<v8::Object> data = v8::Object::New(iso);

            {
                MutexLocker locker(&state.completion.mutex);
                data->Set(makeValue("buffer"), makeValue(std::string(state.completion.pending.buffer)));
                data->Set(makeValue("text"), makeValue(std::string(state.completion.pending.text)));
                data->Set(makeValue("start"), v8::Integer::New(state.iso,state.completion.pending.start));
                data->Set(makeValue("end"), v8::Integer::New(state.iso,state.completion.pending.end));
            }

            values.push_back(data);
            values.push_back(cb);

            // ask js, pass a callback
            f->Call(f, values.size(), &values[0]);
        } else if (async == &state.pauseAsync) {
            state.pauseCb.Call(0, 0);
            state.pauseCb.Reset();
        } else if (async == &state.resumeAsync) {
            state.resumeCb.Call(0, 0);
            state.resumeCb.Reset();
        } else if (async == &state.promptAsync) {
            state.promptCb.Call(0, 0);
            state.promptCb.Reset();
        } else {
        }
    };

    uv_async_init(uv_default_loop(), &state.readline.async, cb);
    uv_async_init(uv_default_loop(), &state.completion.async, cb);

    uv_async_init(uv_default_loop(), &state.pauseAsync, cb);
    uv_async_init(uv_default_loop(), &state.resumeAsync, cb);
    uv_async_init(uv_default_loop(), &state.promptAsync, cb);

    uv_thread_create(&state.thread, State::run, 0);
}

NAN_METHOD(pause) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        state.pauseCb.Reset(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupPause);
    } else {
        Nan::ThrowError("pause takes a function callback");
    }
}

NAN_METHOD(resume) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        state.resumeCb.Reset(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupResume);
    } else {
        Nan::ThrowError("resume takes a function callback");
    }
}

NAN_METHOD(prompt) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        state.promptCb.Reset(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupPrompt);
    } else {
        Nan::ThrowError("prompt takes a function callback");
    }
}

NAN_METHOD(stop) {
    state.cleanup();
}

NAN_METHOD(log) {
    const auto len = info.Length();
    for (int i = 0; i < len; ++i) {
        Nan::Utf8String str(info[i]);
        fprintf(stdout, "%s%s", *str, i + 1 < len ? " " : "");
    }
}

NAN_METHOD(error) {
    const auto len = info.Length();
    for (int i = 0; i < len; ++i) {
        Nan::Utf8String str(info[i]);
        fprintf(stderr, "%s%s", *str, i + 1 < len ? " " : "");
    }
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, start);
    NAN_EXPORT(target, stop);
    NAN_EXPORT(target, pause);
    NAN_EXPORT(target, resume);
    NAN_EXPORT(target, prompt);
    NAN_EXPORT(target, log);
    NAN_EXPORT(target, error);
}

NODE_MODULE(nativeReadline, Initialize)
