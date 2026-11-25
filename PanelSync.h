#pragma once

// The lock and the wake-ups between the drawing core and the present worker.
//
// Where the C++ library has threads, they are std::mutex and
// std::condition_variable. Where it has none (a Pico SDK build: Arm's
// arm-none-eabi library is built for one thread), they are whatever the
// platform hands SetPanelSyncHooks: on a Pico, a hardware spinlock and the
// cores' wait-for-event. A platform that hands none has no present worker,
// so the drawing core is the only one that takes them and they do nothing.

// <mutex> for std::lock_guard and std::unique_lock, which it has either way.
#include <mutex>
#if defined(__STDCPP_THREADS__)
#include <condition_variable>
#endif

#include "DekiDisplayPackage.h"

namespace DekiDisplay
{

/// A platform's lock and wake-ups, for a build whose C++ library has no
/// threads. `wait` may return early; the caller checks its condition again.
struct PanelSyncHooks
{
    void (*lock)() = nullptr;
    void (*unlock)() = nullptr;
    /// Sleeps until `wake` is called on the other core, or a moment passes.
    void (*wait)() = nullptr;
    void (*wake)() = nullptr;
};

/// Sets the hooks PanelMutex and PanelSignal use where there are no threads.
/// Call before the present worker starts. `hooks` must outlive the display.
DEKI_DISPLAY_API void SetPanelSyncHooks(const PanelSyncHooks* hooks);
DEKI_DISPLAY_API const PanelSyncHooks* GetPanelSyncHooks();

#if defined(__STDCPP_THREADS__)

using PanelMutex = std::mutex;
using PanelSignal = std::condition_variable;

#else

/// The platform's lock, or nothing when it has none.
struct PanelMutex
{
    void lock()
    {
        if (const PanelSyncHooks* hooks = GetPanelSyncHooks())
        {
            hooks->lock();
        }
    }
    void unlock()
    {
        if (const PanelSyncHooks* hooks = GetPanelSyncHooks())
        {
            hooks->unlock();
        }
    }
    bool try_lock()
    {
        lock();
        return true;
    }
};

/// The platform's wake-ups. Without them a wait returns at once: with no
/// other core, the condition can only already hold or never.
struct PanelSignal
{
    template <typename Lock, typename Predicate>
    void wait(Lock& lock, Predicate predicate)
    {
        const PanelSyncHooks* hooks = GetPanelSyncHooks();
        if (!hooks)
        {
            return;
        }
        while (!predicate())
        {
            lock.unlock();
            hooks->wait();
            lock.lock();
        }
    }
    void notify_one()
    {
        if (const PanelSyncHooks* hooks = GetPanelSyncHooks())
        {
            hooks->wake();
        }
    }
    void notify_all() { notify_one(); }
};

#endif

}  // namespace DekiDisplay
