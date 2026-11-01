// AI-assisted (Claude Opus 5.5).
//
// Bounded wait for a blocking call that cannot be cancelled (#189).
//
// esp_video's VFS has no select()/poll() and no O_NONBLOCK, and its
// VIDIOC_DQBUF waits on the frame queue with portMAX_DELAY. If the sensor
// never delivers a frame, ioctl(VIDIOC_DQBUF) blocks its caller forever. The
// call itself cannot be interrupted, so run it on a persistent worker thread
// and let the caller give up after a deadline. The worker stays parked in the
// call; the next Wait() adopts that same pending call instead of starting a
// second one, so a late frame is simply picked up by the next caller.
#pragma once

#include <condition_variable>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class BoundedCall {
public:
    using Fn = std::function<int()>;

    explicit BoundedCall(Fn fn) : s_(std::make_shared<State>())
    {
        s_->fn = std::move(fn);
        auto s = s_;
        // Detached: a call stuck forever must not make the destructor hang.
        std::thread([s]() {
            std::unique_lock<std::mutex> lk(s->m);
            for (;;) {
                s->cv.wait(lk, [&] { return s->requested || s->quit; });
                if (s->quit) {
                    return;
                }
                s->requested = false;
                lk.unlock();
                int rc = s->fn();
                lk.lock();
                s->rc   = rc;
                s->done = true;
                s->cv.notify_all();
            }
        }).detach();
    }

    ~BoundedCall()
    {
        std::lock_guard<std::mutex> lk(s_->m);
        s_->quit = true;
        s_->cv.notify_all();
    }

    BoundedCall(const BoundedCall&)            = delete;
    BoundedCall& operator=(const BoundedCall&) = delete;

    // Returns true and sets *rc if the call finished within timeout_ms.
    // Returns false on timeout; the call stays pending and a later Wait()
    // resumes waiting on it. Callers must be serialised.
    bool Wait(int timeout_ms, int* rc)
    {
        std::unique_lock<std::mutex> lk(s_->m);
        if (!s_->pending) {
            s_->pending   = true;
            s_->done      = false;
            s_->requested = true;
            s_->cv.notify_all();
        }
        if (!s_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return s_->done; })) {
            return false;
        }
        s_->pending = false;
        *rc         = s_->rc;
        return true;
    }

    // True while a timed-out call is still parked in the blocking function.
    bool Pending() const
    {
        std::lock_guard<std::mutex> lk(s_->m);
        return s_->pending;
    }

private:
    struct State {
        mutable std::mutex m;
        std::condition_variable cv;
        Fn fn;
        bool requested = false, pending = false, done = false, quit = false;
        int rc = 0;
    };
    std::shared_ptr<State> s_;
};
