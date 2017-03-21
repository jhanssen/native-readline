#include <nan.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <memory>
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
#ifdef LOG
    FILE* log;
#endif

    Mutex mutex;
    bool stopped;

    struct {
        Nan::Callback function;
        Nan::Persistent<v8::Function> callback;
        uv_async_t async;

        std::string text, over;

        Mutex mutex;
        Condition condition;

        bool has, updated;
    } prompt;

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

    struct {
        Nan::Callback function;
        uv_async_t async;
        int rows, cols;
        Mutex mutex;
    } term;

    static void run(void* arg);

    bool init();
    void cleanup();

    enum WakeupReason {
        WakeupStop,
        WakeupPause,
        WakeupResume,
        WakeupPrompt,
        WakeupInt,
        WakeupWinch
    };
    void wakeup(WakeupReason reason);
    uv_async_t pauseAsync, resumeAsync, promptAsync;
    std::unique_ptr<Nan::Callback> pauseCb, resumeCb, promptCb;

    uv_signal_t handleWinchSignal;

    // pause state
    char* savedLine;
    int savedPoint;

    void saveState();
    void restoreState();

    void readTermInfo();
};

static State state;

void logException(const char* msg, const Nan::TryCatch& tryCatch)
{
    Nan::HandleScope scope;
    Nan::Utf8String str(tryCatch.Exception());
    fprintf(stderr, "EXCEPTION: %s '%s'\n", msg, *str);
    auto maybeStack = tryCatch.StackTrace();
    if (!maybeStack.IsEmpty()) {
        auto stack = maybeStack.ToLocalChecked();
        Nan::Utf8String stackStr(stack);
        fprintf(stderr, "%s\n", *stackStr);
    } else {
        auto msg = tryCatch.Message();
        if (!msg.IsEmpty()) {
            auto stack = msg->GetStackTrace();
            //fprintf(stderr, "frames:%d\n", stack->GetFrameCount());
            for (auto i = 0; i < stack->GetFrameCount(); ++i) {
                auto frame = stack->GetFrame(i);
                if (!frame.IsEmpty()) {
                    auto frameName = frame->GetScriptName();
                    auto funcName = frame->GetFunctionName();
                    Nan::Utf8String name(frameName);
                    Nan::Utf8String func(funcName);
                    fprintf(stderr, "%s (%s:%d:%d)\n", *func, *name, frame->GetLineNumber(), frame->GetColumn());
                }
            }
        }
    }
    fprintf(stderr, "---\n");
}

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

void State::readTermInfo()
{
    struct winsize win;
    const int tty = STDIN_FILENO;
    if (ioctl(tty, TIOCGWINSZ, &win) == 0) {
        MutexLocker locker(&state.term.mutex);
        state.term.rows = win.ws_row;
        state.term.cols = win.ws_col;
        uv_async_send(&state.term.async);
    }
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

        // we want full control over the output
        rl_completion_suppress_append = 1;
        rl_completion_suppress_quote = 1;

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
    auto reprompt = []() -> const std::string& {
        MutexLocker locker(&state.prompt.mutex);
        if (!state.prompt.over.empty())
            return state.prompt.over;
        if (!state.prompt.has || state.prompt.updated) {
            state.prompt.updated = false;
            return state.prompt.text;
        }
        uv_async_send(&state.prompt.async);
        state.prompt.condition.wait(&state.prompt.mutex);
        return state.prompt.text;
    };

    state.readTermInfo();

    rl_persistent_signal_handlers = 0;
    rl_catch_signals = 0;
    rl_catch_sigwinch = 0;
    rl_change_environment = 0;
    rl_outstream = state.redirector.stderrFile();
    rl_initialize();

    rl_callback_handler_install(state.prompt.text.c_str(), handler);
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
                            state.redirector.resume();
                            const std::string& text = reprompt();
                            rl_resize_terminal();
                            state.readTermInfo();
                            rl_callback_handler_install(text.c_str(), handler);
                            state.restoreState();
                        }
                        uv_async_send(&state.resumeAsync);
                        break;
                    case WakeupInt: {
                        rl_callback_sigcleanup();

                        if (rl_undo_list)
                            rl_free_undo_list ();
                        rl_point = 0;
                        rl_kill_text (rl_point, rl_end);
                        rl_mark = 0;
                        rl_clear_message();
                        rl_crlf();
                        rl_reset_line_state();

                        const std::string& text = reprompt();
                        rl_set_prompt("");
                        rl_redisplay();
                        rl_set_prompt(text.c_str());
                        rl_redisplay();
                        break; }
                    case WakeupPrompt:
                        if (!state.paused) {
                            const std::string& text = reprompt();
                            // I really have no idea why I have to clear the prompt and redisplay, but I do.
                            rl_set_prompt("");
                            rl_redisplay();
                            rl_set_prompt(text.c_str());
                            rl_redisplay();
                        }
                        uv_async_send(&state.promptAsync);
                        break;
                    case WakeupWinch:
                        rl_resize_terminal();
                        state.readTermInfo();
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
                state.redirector.pause();
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
    info.GetIsolate()->SetCaptureStackTraceForUncaughtExceptions(true, 30);

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
    state.readline.function.Reset(Nan::Persistent<v8::Function>(v8::Local<v8::Function>::Cast(info[0])));
    state.completion.function.Reset(Nan::Persistent<v8::Function>(v8::Local<v8::Function>::Cast(info[1])));

    {
        auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            if (info.Length() >= 1 && info[0]->IsString()) {
                MutexLocker locker(&state.prompt.mutex);
                state.prompt.text = *Nan::Utf8String(info[0]);
                state.prompt.updated = true;
                state.wakeup(State::WakeupPrompt);
            }
        };
        auto cb = v8::Function::New(Nan::GetCurrentContext(), callback);
        state.prompt.callback.Reset(Nan::Persistent<v8::Function>(cb.ToLocalChecked()));
    }
    state.prompt.has = false;
    state.prompt.updated = false;
    if (info.Length() > 2 && info[2]->IsFunction()) {
        state.prompt.has = true;
        state.prompt.function.Reset(v8::Local<v8::Function>::Cast(info[2]));

        auto iso = info.GetIsolate();
        v8::Local<v8::Value> cb = v8::Local<v8::Function>::New(iso, state.prompt.callback);
        auto ret = state.prompt.function.Call(1, &cb);
        if (!ret.IsEmpty() && ret->IsString()) {
            Nan::Utf8String str(ret);
            state.prompt.text = *str;
        } else {
            state.prompt.text = "jsh> ";
        }
    } else {
        state.prompt.text = "jsh> ";
    }

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
                Nan::TryCatch tryCatch;
                f->Call(f, 1, &value);
                if (tryCatch.HasCaught()) {
                    logException("Readline", tryCatch);
                }
            }

            // possibly reprompt
            state.wakeup(State::WakeupPrompt);
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
            Nan::TryCatch tryCatch;
            f->Call(f, values.size(), &values[0]);
            if (tryCatch.HasCaught()) {
                logException("Completion", tryCatch);

                MutexLocker locker(&state.completion.mutex);
                state.completion.results.clear();
                //state.redirector.writeStdout("signaling\n");
                state.completion.condition.signal();
            }
        } else if (async == &state.prompt.async) {
            bool hasPrompt = false;
            std::string text;
            Nan::TryCatch tryCatch;
            v8::Local<v8::Value> cb = v8::Local<v8::Function>::New(state.iso, state.prompt.callback);
            auto p = state.prompt.function.Call(1, &cb);
            if (tryCatch.HasCaught()) {
                logException("Prompt", tryCatch);
            } else {
                if (!p.IsEmpty() && p->IsString()) {
                    Nan::Utf8String str(p);
                    hasPrompt = true;
                    text = *str;
                }
            }
            MutexLocker locker(&state.prompt.mutex);
            if (hasPrompt) {
                state.prompt.text = text;
            }
            state.prompt.condition.signal();
        } else if (async == &state.pauseAsync) {
            if (state.pauseCb) {
                auto cb = std::move(state.pauseCb);
                cb->Call(0, 0);
            }
        } else if (async == &state.resumeAsync) {
            if (state.resumeCb) {
                auto cb = std::move(state.resumeCb);
                cb->Call(0, 0);
            }
        } else if (async == &state.promptAsync) {
            if (state.promptCb) {
                auto cb = std::move(state.promptCb);
                cb->Call(0, 0);
            }
        } else if (async == &state.term.async) {
            if (state.term.function.IsEmpty())
                return;

            auto iso = v8::Isolate::GetCurrent();
            v8::Local<v8::Object> data = v8::Object::New(iso);

            {
                MutexLocker locker(&state.term.mutex);
                data->Set(makeValue("rows"), v8::Integer::New(state.iso, state.term.rows));
                data->Set(makeValue("cols"), v8::Integer::New(state.iso, state.term.cols));
            }

            v8::Local<v8::Value> val = data;

            // tell js
            Nan::TryCatch tryCatch;
            state.term.function.Call(1, &val);
            if (tryCatch.HasCaught()) {
                logException("Term", tryCatch);
            }
        } else {
        }
    };

    uv_async_init(uv_default_loop(), &state.readline.async, cb);
    uv_async_init(uv_default_loop(), &state.completion.async, cb);
    uv_async_init(uv_default_loop(), &state.prompt.async, cb);

    uv_async_init(uv_default_loop(), &state.pauseAsync, cb);
    uv_async_init(uv_default_loop(), &state.resumeAsync, cb);
    uv_async_init(uv_default_loop(), &state.promptAsync, cb);

    uv_async_init(uv_default_loop(), &state.term.async, cb);

    uv_signal_init(uv_default_loop(), &state.handleWinchSignal);
    uv_signal_start(&state.handleWinchSignal, [](uv_signal_t*, int) {
            state.wakeup(State::WakeupWinch);
        }, SIGWINCH);

    uv_thread_create(&state.thread, State::run, 0);
}

NAN_METHOD(pause) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        uv_signal_stop(&state.handleWinchSignal);

        state.pauseCb = std::make_unique<Nan::Callback>(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupPause);
    } else {
        Nan::ThrowError("pause takes a function callback");
    }
}

NAN_METHOD(resume) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        if (info.Length() >= 2 && info[1]->IsString()) {
            MutexLocker locker(&state.prompt.mutex);
            state.prompt.text = *Nan::Utf8String(info[1]);
            state.prompt.updated = true;
        }

        uv_signal_start(&state.handleWinchSignal, [](uv_signal_t*, int) {
                state.wakeup(State::WakeupWinch);
            }, SIGWINCH);

        state.resumeCb = std::make_unique<Nan::Callback>(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupResume);
    } else {
        Nan::ThrowError("resume takes a function callback");
    }
}

NAN_METHOD(prompt) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        if (info.Length() >= 2 && info[1]->IsString()) {
            MutexLocker locker(&state.prompt.mutex);
            state.prompt.text = *Nan::Utf8String(info[1]);
            state.prompt.updated = true;
        }

        state.promptCb = std::make_unique<Nan::Callback>(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupPrompt);
    } else {
        Nan::ThrowError("prompt takes a function callback");
    }
}

NAN_METHOD(setPrompt) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        state.prompt.function.Reset(v8::Local<v8::Function>::Cast(info[0]));
        auto iso = info.GetIsolate();
        v8::Local<v8::Value> cb = v8::Local<v8::Function>::New(iso, state.prompt.callback);
        auto ret = state.prompt.function.Call(1, &cb);

        MutexLocker locker(&state.prompt.mutex);
        state.prompt.has = true;
        if (!ret.IsEmpty() && ret->IsString()) {
            Nan::Utf8String str(ret);
            state.prompt.text = *str;
            state.prompt.updated = true;
        }

        state.wakeup(State::WakeupPrompt);
    }
}

NAN_METHOD(overridePrompt) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        MutexLocker locker(&state.prompt.mutex);
        state.prompt.over = *Nan::Utf8String(info[0]);
        state.wakeup(State::WakeupPrompt);
    }
}

NAN_METHOD(restorePrompt) {
    MutexLocker locker(&state.prompt.mutex);
    state.prompt.over.clear();
    state.wakeup(State::WakeupPrompt);
}

NAN_METHOD(setTerm) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        state.term.function.Reset(v8::Local<v8::Function>::Cast(info[0]));
        state.wakeup(State::WakeupWinch);
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

NAN_METHOD(sigint) {
    MutexLocker locker(&state.prompt.mutex);
    state.prompt.over.clear();
    state.wakeup(State::WakeupInt);
}

NAN_METHOD(setOptions) {
    if (info.Length() >= 1 && info[0]->IsObject()) {
        auto obj = v8::Handle<v8::Object>::Cast(info[0]);
        auto histMax = Nan::Get(obj, Nan::New("historyMax").ToLocalChecked()).ToLocalChecked();
        if (!histMax.IsEmpty() && histMax->IsNumber()) {
            history_max_entries = static_cast<int>(v8::Handle<v8::Number>::Cast(histMax)->Value());
        }
    } else {
        Nan::ThrowError("setOptions takes an object");
    }
}

NAN_METHOD(addHistory) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        Nan::Utf8String str(info[0]);
        add_history(*str);
    } else {
        Nan::ThrowError("addHistory takes a string");
    }
}

NAN_METHOD(readHistory) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        Nan::Utf8String str(info[0]);
        const int ret = read_history(*str);
        if (!ret) {
            using_history();
        }
        info.GetReturnValue().Set(Nan::New<v8::Uint32>(ret));
    } else {
        Nan::ThrowError("readHistory takes a string");
    }
}

NAN_METHOD(writeHistory) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        Nan::Utf8String str(info[0]);
        const int ret = write_history(*str);
        info.GetReturnValue().Set(Nan::New<v8::Uint32>(ret));
    } else {
        Nan::ThrowError("writeHistory takes a string");
    }
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, start);
    NAN_EXPORT(target, stop);
    NAN_EXPORT(target, pause);
    NAN_EXPORT(target, resume);
    NAN_EXPORT(target, setPrompt);
    NAN_EXPORT(target, overridePrompt);
    NAN_EXPORT(target, restorePrompt);
    NAN_EXPORT(target, setTerm);
    NAN_EXPORT(target, prompt);
    NAN_EXPORT(target, log);
    NAN_EXPORT(target, error);
    NAN_EXPORT(target, sigint);
    NAN_EXPORT(target, setOptions);
    NAN_EXPORT(target, addHistory);
    NAN_EXPORT(target, readHistory);
    NAN_EXPORT(target, writeHistory);
}

NODE_MODULE(nativeReadline, Initialize)
