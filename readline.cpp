#include <nan.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <memory>
#include <unordered_map>
#include "utils.h"
#include "Redirector.h"
#include <errno.h>
#include <assert.h>
#include <functional>

// #define LOG

#define rlassert(expr)                                                  \
    do {                                                                \
        if (!(expr)) {                                                  \
            char buf[1024];                                             \
            snprintf(buf, sizeof(buf), "assertion failed: %s:%d (%s)\n", __FILE__, __LINE__, __FUNCTION__); \
            state.redirector.writeStdout(buf);                          \
            abort();                                                    \
        }                                                               \
    } while (false)                                                     \

struct State
{
    State() : started(false), stopped(false), lastIterateState(State::StateNormal), savedLine(0), savedPoint(0) { }

    v8::Isolate* iso;

    bool started;
    uv_thread_t thread;
    int wakeupPipe[2];
    Redirector redirector;
#ifdef LOG
    FILE* log;
#endif

    Mutex mutex, resumeMutex;
    bool stopped;
    std::vector<std::function<void()> > onResume, onNext;

    struct {
        Nan::Callback function;
        Nan::Persistent<v8::Function> callback;
        uv_async_t async;

        std::string text, over;

        Mutex mutex;
        bool has, updated, waiting;

        bool isWaiting()
        {
            MutexLocker locker(&mutex);
            bool old = waiting;
            waiting = false;
            return old;
        }
    } prompt;

    struct {
        uv_async_t async;

        Queue<std::string> lines;
    } readline;

    struct {
        Nan::Persistent<v8::Function> callback;
        uv_async_t async;

        struct {
            std::string buffer;
            std::string text;
            int start, end;
        } pending;
        std::vector<std::string> results;

        Mutex mutex;
        //Condition condition;
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
        WakeupRunResumes,
        WakeupRunNexts,
        WakeupPrompt,
        WakeupInt,
        WakeupWinch
    };
    void wakeup(WakeupReason reason);
    uv_async_t pauseAsync, resumeAsync, promptAsync;
    std::unique_ptr<Nan::Callback> promptCb;
    std::vector<std::unique_ptr<Nan::Callback> > pauseCb, resumeCb;

    enum IterateState {
        StateNormal,
        StatePause,
        StatePrompt,
        StateForcePrompt,
        StateCompletion
    };
    IterateState lastIterateState;
    std::vector<IterateState> iterateState;
    std::function<void(IterateState)> iterate;

    uv_signal_t handleWinchSignal;

    struct {
        std::string file;
        uv_async_t async;
    } history;

    std::unordered_map<std::string, std::vector<std::unique_ptr<Nan::Callback> > > ons;
    bool runOns(const std::string& type, std::vector<v8::Local<v8::Value> >& val, Nan::TryCatch& tryCatch);

    // pause state
    char* savedLine;
    int savedPoint;

    void saveState();
    void restoreState();

    void readTermInfo();

    void forcePrompt(const std::string& prompt);
};

static State state;

static void logException(const char* msg, const Nan::TryCatch& tryCatch)
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

// handle utf-8
#define ADVANCE_CHAR(_str, _strsize, _i)            \
    do {                                            \
        if (_i + 1 <= _strsize) {                   \
            if (_str[++_i] & 0x10000000)            \
                continue;                           \
        } else if (_i == _strsize) {                \
            ++_i;                                   \
        }                                           \
    }                                               \
    while (0)

// ugh, maybe we should delegate this to js?
// also, how many times have I written this code now?
int char_is_quoted(char* string, int eindex)
{
    enum State {
        Normal,
        Double,
        Single,
        Escape
    };
    bool wasEscaped = false;
    std::vector<State> state;
    state.push_back(Normal);
    auto current = [&state]() {
        return state.back();
    };
    auto maybePop = [&state](State s) {
        if (state.back() == s) {
            state.pop_back();
            return true;
        }
        return false;
    };
    for (int i = 0; i <= eindex;) {
        wasEscaped = current() == Escape;
        switch (string[i]) {
        case '\\':
            switch (current()) {
            case Escape:
                state.pop_back();
                break;
            default:
                state.push_back(Escape);
                break;
            }
            break;
        case '"':
            switch (current()) {
            case Normal:
                state.push_back(Double);
                break;
            case Double:
                state.pop_back();
                break;
            default:
                break;
            }
            maybePop(Escape);
            break;
        case '\'':
            switch (current()) {
            case Normal:
                state.push_back(Single);
                break;
            case Single:
                state.pop_back();
                break;
            default:
                break;
            }
            maybePop(Escape);
            break;
        default:
            maybePop(Escape);
            break;
        }

        ADVANCE_CHAR(string, eindex, i);
    }
    //printf("got state %d for %d (%c - '%s')\n", eindex, string[eindex], string);
    return (!wasEscaped && current() == Normal) ? 0 : 1;
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

void State::forcePrompt(const std::string& prompt)
{
    std::string p = prompt;
    {
        MutexLocker locker(&state.prompt.mutex);
        if (!state.prompt.over.empty()) {
            p = state.prompt.over;
        }
    }

    // there really must be a better way of doing this. right? right?
    int savedPoint = rl_point;
    int savedMark = rl_mark;
    char* savedLine = rl_copy_text(0, rl_end);
    rl_replace_line("", 0);
    rl_set_prompt("");
    rl_redisplay();

    rl_replace_line(savedLine, 0);
    rl_set_prompt(p.c_str());
    rl_point = savedPoint;
    rl_mark = savedMark;
    rl_redisplay();
    free(savedLine);
}

bool State::runOns(const std::string& type, std::vector<v8::Local<v8::Value> >& vals, Nan::TryCatch& tryCatch)
{
    auto& vec = ons[type];
    if (vec.empty())
        return true;
    for (const auto& cb : vec) {
        cb->Call(vals.size(), &vals[0]);
        if (tryCatch.HasCaught())
            return false;
    }
    return true;
}

static void handleOut(int fd, bool paused, const std::function<void(const char*, int)>& write)
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
            if (!saved && !paused) {
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

    if (saved) {
        state.restoreState();
    }
}

void State::run(void* arg)
{
    state.stopped = false;

    auto handler = [](char* line) {
        if (!line) {
            // we're done
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
        {
            MutexLocker locker(&state.completion.mutex);
            state.completion.pending = { std::string(rl_line_buffer), std::string(text), start, end };
            uv_async_send(&state.completion.async);
        }
        //state.redirector.writeStdout("postcomplete\n");
        state.iterate(State::StateCompletion);
        if (state.stopped)
            return nullptr;
        {
            MutexLocker locker(&state.completion.mutex);
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
        }
    };
    auto reprompt = [](bool force = false) -> bool {
        bool has = false;
        std::string prompt;
        {
            MutexLocker locker(&state.prompt.mutex);
            if (!state.prompt.over.empty()) {
                has = true;
                prompt = state.prompt.over;
            } else if (!state.prompt.has || state.prompt.updated || force) {
                has = true;
                state.prompt.updated = false;
                prompt = state.prompt.text;
            }
        }
        if (has) {
            state.forcePrompt(prompt);
            return true;
        }
        {
            MutexLocker locker(&state.prompt.mutex);
            state.prompt.waiting = true;
        }
        uv_async_send(&state.prompt.async);
        return false;
    };

    state.readTermInfo();

    rl_persistent_signal_handlers = 0;
    rl_catch_signals = 0;
    rl_catch_sigwinch = 0;
    rl_change_environment = 0;
    rl_outstream = state.redirector.stderrFile();

    rl_char_is_quoted_p = char_is_quoted;
    rl_completer_quote_characters = "'\"";

    rl_initialize();
    rl_resize_terminal();

    rl_callback_handler_install(state.prompt.text.c_str(), handler);
    rl_attempted_completion_function = completer;

    const auto stdoutfunc = std::bind(&Redirector::writeStdout, &state.redirector, std::placeholders::_1, std::placeholders::_2);
    const auto stderrfunc = std::bind(&Redirector::writeStderr, &state.redirector, std::placeholders::_1, std::placeholders::_2);

    struct RefScope
    {
        RefScope(IterateState is, int& r, std::function<void()>& ps, std::function<void()>& rs)
            : ref(r), pause(ps), resume(rs)
        {
            // char buf[1024];
            // snprintf(buf, 1024, "pre RefScope %zu\n", state.iterateState.size());
            // state.redirector.writeStdout(buf);
            icnt = state.iterateState.size();
            lstate = !icnt ? State::StateNormal : state.iterateState.back();
            state.iterateState.push_back(is);
            if (ref++ == 1)
                pause();
        }
        ~RefScope()
        {
            // char buf[1024];
            // snprintf(buf, 1024, "pre ~RefScope %zu\n", state.iterateState.size() - 1);
            // state.redirector.writeStdout(buf);
            rlassert(!state.iterateState.empty());
            state.lastIterateState = state.iterateState.back();
            state.iterateState.pop_back();
            rlassert(icnt == state.iterateState.size());
            // printf("lstate %d, curstate %d\n", lstate, !icnt ? State::StateNormal : state.iterateState.back());
            rlassert(lstate == (!icnt ? State::StateNormal : state.iterateState.back()));

            if (--ref == 1)
                resume();
        }

        bool paused() const { return ref > 1; };

        enum RunMode { Now, Enqueue };
        void run(RunMode mode, std::function<void()>&& func)
        {
            if (ref == 1) {
                func();
            } else if (mode == Enqueue) {
                MutexLocker locker(&state.resumeMutex);
                state.onResume.push_back(std::move(func));
            }
        }

        int& ref;
        std::function<void()>& pause;
        std::function<void()>& resume;
        size_t icnt;
        IterateState lstate;
    };

    auto pauseRl = std::function<void()>([&reprompt, &handler]() {
            state.saveState();
            rl_callback_handler_remove();
            state.redirector.pause();

            MutexLocker locker(&state.resumeMutex);
            state.onResume.push_back([&reprompt, &handler]() {
                    rl_resize_terminal();
                    state.readTermInfo();
                    rl_callback_handler_install(state.prompt.text.c_str(), handler);
                    state.restoreState();
                    rlassert(state.iterateState.size() == 1);
                    if (state.lastIterateState == State::StateForcePrompt) {
                        reprompt(true /* force */);
                    } else {
                        if (!reprompt()) {
                            state.iterate(State::StateForcePrompt);
                        }
                    }
                });
        });

    auto resumeRl = std::function<void()>([]() {
            state.redirector.resume();

            // go through all onresumes and run them
            std::vector<std::function<void()> > resumes;
            {
                MutexLocker locker(&state.resumeMutex);
                resumes = std::move(state.onResume);
            }

            for (auto r : resumes) {
                r();
            }
        });

    int ref = 0, pendingPause = 0;
    state.iterate = [&ref, &pendingPause, &stdoutfunc, &stderrfunc, &reprompt, &pauseRl, &resumeRl](IterateState iterateState) {
        RefScope scope(iterateState, ref, pauseRl, resumeRl);

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

        //uv_loop_t* loop = static_cast<uv_loop_t*>(arg);

        for (;;) {
            if (pendingPause) {
                if (!--pendingPause)
                    uv_async_send(&state.pauseAsync);
                state.iterate(State::StatePause);
            }

            // we need to wait on both wakeupPipe[0] and stdin
            FD_ZERO(&rdset);
            FD_SET(state.wakeupPipe[0], &rdset);
            if (!scope.paused())
                FD_SET(STDIN_FILENO, &rdset);
            FD_SET(stdoutfd, &rdset);
            FD_SET(stderrfd, &rdset);

            if (state.stopped)
                return;
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
                            state.stopped = true;
                            break;
                        case WakeupPause:
                            ++pendingPause;
                            break;
                        case WakeupResume:
                            if (pendingPause) {
                                if (!--pendingPause) {
                                    uv_async_send(&state.pauseAsync);
                                }
                                uv_async_send(&state.resumeAsync);
                            } else {
                                uv_async_send(&state.resumeAsync);
                                rlassert(ref > 1);
                                return;
                            }
                            break;
                        case WakeupRunResumes:
                            scope.run(RefScope::Now, []() {
                                    std::vector<std::function<void()> > resumes;
                                    {
                                        MutexLocker locker(&state.resumeMutex);
                                        resumes = std::move(state.onResume);
                                    }

                                    for (auto r : resumes) {
                                        r();
                                    }
                                });
                            break;
                        case WakeupRunNexts: {
                            std::vector<std::function<void()> > nexts;
                            {
                                MutexLocker locker(&state.resumeMutex);
                                nexts = std::move(state.onNext);
                            }

                            for (auto n : nexts) {
                                n();
                            }
                            break; }
                        case WakeupInt:
                            scope.run(RefScope::Now, [&reprompt]() {
                                    rl_callback_sigcleanup();

                                    if (rl_undo_list)
                                        rl_free_undo_list ();
                                    rl_clear_message();
                                    rl_crlf();
                                    rl_point = rl_mark = 0;
                                    rl_kill_text (rl_point, rl_end);
                                    rl_mark = 0;
                                    rl_reset_line_state();

                                    if (!reprompt()) {
                                        state.iterate(State::StatePrompt);
                                    }
                                });
                            break;
                        case WakeupPrompt:
                            scope.run(RefScope::Now, [&reprompt]() {
                                    if (!reprompt()) {
                                        state.iterate(State::StatePrompt);
                                    }
                                });
                            uv_async_send(&state.promptAsync);
                            break;
                        case WakeupWinch:
                            scope.run(RefScope::Enqueue, []() {
                                    rl_resize_terminal();
                                    state.readTermInfo();
                                });
                            break;
                        }
                    }
                }

                if (stopped)
                    break;
            }
            if (FD_ISSET(stdoutfd, &rdset)) {
                handleOut(stdoutfd, scope.paused(), stdoutfunc);
            }
            if (FD_ISSET(stderrfd, &rdset)) {
                handleOut(stderrfd, scope.paused(), stderrfunc);
            }
            if (FD_ISSET(STDIN_FILENO, &rdset) && !scope.paused()) {
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

            if (state.stopped)
                return;
        }
    };
    state.iterate(State::StateNormal);

    rl_callback_handler_remove();
}

NAN_METHOD(start) {
    info.GetIsolate()->SetCaptureStackTraceForUncaughtExceptions(true, 30);

    // two arguments, output callback and complete callback
    if (state.started) {
        Nan::ThrowError("Already started. Stop first");
        return;
    }
    if (!state.init()) {
        Nan::ThrowError("Unable to init readline");
        return;
    }

    {
        auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            if (info.Length() >= 1 && info[0]->IsString()) {
                {
                    MutexLocker locker(&state.prompt.mutex);
                    state.prompt.text = *Nan::Utf8String(info[0]);
                }

                MutexLocker locker(&state.resumeMutex);
                state.onResume.push_back([]() {
                        std::string prompt;
                        {
                            MutexLocker locker(&state.prompt.mutex);
                            prompt = state.prompt.text;
                        }
                        state.forcePrompt(prompt);
                    });
            }
            const bool waiting = state.prompt.isWaiting();
            // if (waiting)
            //     state.redirector.writeStdout("resume due to late prompt\n");
            state.wakeup(waiting ? State::WakeupResume : State::WakeupRunResumes);
        };
        auto cb = v8::Function::New(Nan::GetCurrentContext(), callback);
        state.prompt.callback.Reset(Nan::Persistent<v8::Function>(cb.ToLocalChecked()));
    }
    state.prompt.has = false;
    state.prompt.updated = false;
    if (info.Length() > 0 && info[0]->IsFunction()) {
        state.prompt.has = true;
        state.prompt.function.Reset(v8::Local<v8::Function>::Cast(info[0]));

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
            // state.redirector.writeStdout("resume due to comp 1\n");

            state.wakeup(State::WakeupResume);
        };
        auto cb = v8::Function::New(Nan::GetCurrentContext(), callback);
        state.completion.callback.Reset(Nan::Persistent<v8::Function>(cb.ToLocalChecked()));
    }

    auto cb = [](uv_async_t* async) {
        v8::Isolate::Scope isolateScope(state.iso);
        Nan::HandleScope scope;
        if (async == &state.readline.async) {
            bool ok;
            for (;;) {
                const std::string line = state.readline.lines.pop(&ok);
                if (!ok)
                    break;
                std::vector<v8::Local<v8::Value> > values;
                values.push_back(makeValue(line));
                Nan::TryCatch tryCatch;
                if (!state.runOns("line", values, tryCatch)) {
                    if (tryCatch.HasCaught()) {
                        logException("Readline", tryCatch);
                    }
                }
            }

            // possibly reprompt
            state.wakeup(State::WakeupPrompt);
        } else if (async == &state.completion.async) {
            auto iso = v8::Isolate::GetCurrent();
            v8::Handle<v8::Function> cb = v8::Local<v8::Function>::New(iso, state.completion.callback);

            std::vector<v8::Handle<v8::Value> > values;
            v8::Handle<v8::Object> data = v8::Object::New(iso);

            {
                MutexLocker locker(&state.completion.mutex);
                data->Set(makeValue("buffer"), makeValue(state.completion.pending.buffer));
                data->Set(makeValue("text"), makeValue(state.completion.pending.text));
                data->Set(makeValue("start"), v8::Integer::New(state.iso,state.completion.pending.start));
                data->Set(makeValue("end"), v8::Integer::New(state.iso,state.completion.pending.end));
            }

            values.push_back(data);
            values.push_back(cb);

            // ask js, pass a callback
            Nan::TryCatch tryCatch;
            if (!state.runOns("complete", values, tryCatch)) {
                logException("Completion", tryCatch);

                MutexLocker locker(&state.completion.mutex);
                state.completion.results.clear();
                //state.redirector.writeStdout("signaling\n");
                // state.redirector.writeStdout("resume due to comp 2\n");

                state.wakeup(State::WakeupResume);
            }
        } else if (async == &state.prompt.async) {
            bool hasPrompt = false;
            std::string text;
            Nan::TryCatch tryCatch;
            v8::Local<v8::Value> cb = v8::Local<v8::Function>::New(state.iso, state.prompt.callback);
            auto p = state.prompt.function.Call(1, &cb);
            if (tryCatch.HasCaught()) {
                logException("Prompt", tryCatch);
                // state.redirector.writeStdout("resume due to exception (prompt)\n");
                state.wakeup(State::WakeupResume);
            } else {
                if (!p.IsEmpty() && p->IsString()) {
                    Nan::Utf8String str(p);
                    hasPrompt = true;
                    text = *str;
                }
            }
            if (hasPrompt) {
                bool waiting;
                {
                    MutexLocker locker(&state.prompt.mutex);
                    state.prompt.text = text;
                    waiting = state.prompt.waiting;
                }
                if (!waiting) {
                    MutexLocker locker(&state.resumeMutex);
                    state.onResume.push_back([]() {
                            std::string prompt;
                            {
                                MutexLocker locker(&state.prompt.mutex);
                                prompt = state.prompt.text;
                            }
                            state.forcePrompt(prompt);
                        });
                    state.wakeup(State::WakeupRunResumes);
                    return;
                }
                MutexLocker locker(&state.prompt.mutex);
                state.prompt.waiting = false;
                // state.redirector.writeStdout("resume due to prompt\n");
                state.wakeup(State::WakeupResume);
            }
        } else if (async == &state.pauseAsync) {
            if (!state.pauseCb.empty()) {
                auto cbs = std::move(state.pauseCb);
                for (const auto& cb : cbs) {
                    cb->Call(0, 0);
                }
            }
        } else if (async == &state.resumeAsync) {
            if (!state.resumeCb.empty()) {
                auto cbs = std::move(state.resumeCb);
                for (const auto& cb : cbs) {
                    cb->Call(0, 0);
                }
            }
        } else if (async == &state.promptAsync) {
            if (state.promptCb) {
                auto cb = std::move(state.promptCb);
                cb->Call(0, 0);
            }
        } else if (async == &state.term.async) {
            auto iso = v8::Isolate::GetCurrent();
            v8::Local<v8::Object> data = v8::Object::New(iso);

            {
                MutexLocker locker(&state.term.mutex);
                data->Set(makeValue("rows"), v8::Integer::New(state.iso, state.term.rows));
                data->Set(makeValue("cols"), v8::Integer::New(state.iso, state.term.cols));
            }

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(data);

            // tell js
            Nan::TryCatch tryCatch;
            if (!state.runOns("term", values, tryCatch))  {
                logException("Term", tryCatch);
            }
        } else if (async == &state.history.async) {
            std::vector<v8::Local<v8::Value> > values;
            Nan::TryCatch tryCatch;
            if (!state.runOns("historyAdded", values, tryCatch)) {
                if (tryCatch.HasCaught()) {
                    logException("Readline", tryCatch);
                }
            }

        } else {
        }
    };

    uv_async_init(uv_default_loop(), &state.readline.async, cb);
    uv_async_init(uv_default_loop(), &state.completion.async, cb);
    uv_async_init(uv_default_loop(), &state.prompt.async, cb);
    uv_async_init(uv_default_loop(), &state.history.async, cb);

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
        state.pauseCb.push_back(std::make_unique<Nan::Callback>(v8::Local<v8::Function>::Cast(info[0])));
        // state.redirector.writeStdout("pause due to real pause\n");
        state.wakeup(State::WakeupPause);
    } else {
        Nan::ThrowError("pause takes a function callback");
    }
}

NAN_METHOD(resume) {
    if (info.Length() >= 1 && info[0]->IsFunction()) {
        state.resumeCb.push_back(std::make_unique<Nan::Callback>(v8::Local<v8::Function>::Cast(info[0])));

        if (info.Length() >= 2 && info[1]->IsString()) {
            MutexLocker locker(&state.prompt.mutex);
            state.prompt.text = *Nan::Utf8String(info[1]);
            state.prompt.updated = true;
        }
        // state.redirector.writeStdout("resume due to real resume\n");

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
            int max = static_cast<int>(v8::Handle<v8::Number>::Cast(histMax)->Value());

            MutexLocker locker(&state.resumeMutex);
            state.onNext.push_back([max]() {
                    history_max_entries = max;
                });
            state.wakeup(State::WakeupRunNexts);
        }
    } else {
        Nan::ThrowError("setOptions takes an object");
    }
}

NAN_METHOD(addHistory) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        Nan::Utf8String str(info[0]);
        std::string nstr = *str;

        bool write = false;
        if (info.Length() >= 2 && info[1]->IsBoolean())
            write = v8::Local<v8::Boolean>::Cast(info[1])->Value();

        MutexLocker locker(&state.resumeMutex);
        state.onNext.push_back([nstr, write]() {
                auto cur = current_history();
                if (!cur) {
                    // last one?
                    cur = history_get(history_base + history_length - 1);
                }
                if (cur) {
                    if (!strcmp(nstr.c_str(), cur->line))
                        return;
                }
                add_history(nstr.c_str());
                if (write && !state.history.file.empty())
                    write_history(state.history.file.c_str());
                uv_async_send(&state.history.async);
            });
        state.wakeup(State::WakeupRunNexts);
    } else {
        Nan::ThrowError("addHistory takes a string");
    }
}

NAN_METHOD(readHistory) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        Nan::Utf8String str(info[0]);
        std::string nstr = *str;

        MutexLocker locker(&state.resumeMutex);
        state.onNext.push_back([nstr]() {
                state.history.file = nstr;
                const int ret = read_history(nstr.c_str());
                if (!ret) {
                    using_history();
                }
            });
        state.wakeup(State::WakeupRunNexts);
    } else {
        Nan::ThrowError("readHistory takes a string");
    }
}

NAN_METHOD(writeHistory) {
    if (info.Length() >= 1 && info[0]->IsString()) {
        Nan::Utf8String str(info[0]);
        std::string nstr = *str;

        MutexLocker locker(&state.resumeMutex);
        state.onNext.push_back([nstr]() {
                write_history(nstr.c_str());
            });
        state.wakeup(State::WakeupRunNexts);
    } else {
        Nan::ThrowError("writeHistory takes a string");
    }
}

NAN_METHOD(clearHistory) {
    MutexLocker locker(&state.resumeMutex);
    state.onNext.push_back([]() {
            clear_history();
        });
    state.wakeup(State::WakeupRunNexts);
}

NAN_METHOD(on) {
    if (info.Length() > 1 && info[0]->IsString() && info[1]->IsFunction()) {
        const std::string name = *Nan::Utf8String(info[0]);
        state.ons[name].push_back(std::make_unique<Nan::Callback>(v8::Local<v8::Function>::Cast(info[1])));

        // magic for term
        if (name == "term")
            state.wakeup(State::WakeupWinch);
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
    NAN_EXPORT(target, prompt);
    NAN_EXPORT(target, log);
    NAN_EXPORT(target, error);
    NAN_EXPORT(target, sigint);
    NAN_EXPORT(target, on);
    NAN_EXPORT(target, setOptions);
    NAN_EXPORT(target, addHistory);
    NAN_EXPORT(target, readHistory);
    NAN_EXPORT(target, writeHistory);
    NAN_EXPORT(target, clearHistory);
}

NODE_MODULE(nativeReadline, Initialize)
