#pragma once
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <pthread.h>
#include <stdexcept>

/* AppKit/Metal dispatch UI operations to the main queue. Keep that queue
 * serviced while the guest runs, with the native stack size it needs. */
inline int mag_run_with_main_queue(int (*guest)(void*), void* arg) {
    struct Work {
        int (*guest)(void*);
        void* arg;
        std::atomic<bool> done{false};
        int result = 0;
    } work{guest, arg};
    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    pthread_attr_setstacksize(&attrs, 16u * 1024 * 1024);
    pthread_t thread;
    int error = pthread_create(&thread, &attrs, [](void* opaque) -> void* {
        auto* w = static_cast<Work*>(opaque);
        w->result = w->guest(w->arg);
        w->done.store(true, std::memory_order_release);
        return nullptr;
    }, &work);
    pthread_attr_destroy(&attrs);
    if (error) throw std::runtime_error("could not create guest thread");
    while (!work.done.load(std::memory_order_acquire))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    pthread_join(thread, nullptr);
    return work.result;
}
#endif
