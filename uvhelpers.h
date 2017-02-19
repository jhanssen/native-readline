#ifndef UV_HELPERS_H
#define UV_HELPERS_H

#include <nan.h>
#include <assert.h>
#include <queue>
#include <errno.h>

#define EINTRWRAP(var, op)                      \
    do {                                        \
        var = op;                               \
    } while (var == -1 && errno == EINTR);

class Condition;

class Mutex
{
public:
    Mutex()
        : mLocked(false)
    {
        uv_mutex_init(&mMutex);
    }
    ~Mutex()
    {
        if (mLocked)
            unlock();
        uv_mutex_destroy(&mMutex);
    }

    void lock()
    {
        uv_mutex_lock(&mMutex);
        mLocked = true;
    }
    void unlock()
    {
        assert(mLocked);
        mLocked = false;
        uv_mutex_unlock(&mMutex);
    }

    bool locked() const { return mLocked; }

private:
    uv_mutex_t mMutex;
    bool mLocked;

    friend class Condition;
};

class MutexLocker
{
public:
    MutexLocker(Mutex* m)
        : mMutex(m)
    {
        mMutex->lock();
    }

    ~MutexLocker()
    {
        if (mMutex->locked())
            mMutex->unlock();
    }

    void unlock()
    {
        mMutex->unlock();
    }

    void relock()
    {
        mMutex->lock();
    }

private:
    Mutex* mMutex;
};

class Condition
{
public:
    Condition(Mutex* mutex)
        : mMutex(mutex)
    {
        uv_cond_init(&mCond);
    }

    ~Condition()
    {
        uv_cond_destroy(&mCond);
    }

    void wait()
    {
        uv_cond_wait(&mCond, &mMutex->mMutex);
    }

    void waitUntil(uint64_t timeout)
    {
        uv_cond_timedwait(&mCond, &mMutex->mMutex, timeout);
    }

    void signal()
    {
        uv_cond_signal(&mCond);
    }

    void broadcast()
    {
        uv_cond_broadcast(&mCond);
    }

private:
    Mutex* mMutex;
    uv_cond_t mCond;
};

template<typename T>
class Queue
{
public:
    Queue()
    {
    }

    ~Queue()
    {
    }

    void push(T&& t)
    {
        MutexLocker locker(&mMutex);
        mContainer.push(std::forward<T>(t));
    }

    T pop(bool* ok = 0)
    {
        MutexLocker locker(&mMutex);
        if (!mContainer.empty()) {
            if (ok)
                *ok = true;
            const T t = std::move(mContainer.back());
            mContainer.pop();
            return t;
        } else {
            if (ok)
                *ok = false;
            return T();
        }
    }

private:
    Mutex mMutex;
    std::queue<T> mContainer;
};

// jesus christ, v8 people. how bad can you make an API?
inline v8::Handle<v8::Value> makeValue(const std::string& str)
{
    Nan::EscapableHandleScope scope;
    const auto maybe = v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), str.c_str(), v8::NewStringType::kNormal);
    v8::Local<v8::String> v8str;
    if (!maybe.ToLocal(&v8str))
        return scope.Escape(v8::Handle<v8::Value>());
    return scope.Escape(v8str);
}

#endif
