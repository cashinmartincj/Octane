#include "transport/ExecutionQueue.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>

void check(bool condition) {
    if (!condition) std::abort();
}

int main() {
    std::mutex mutex;
    std::condition_variable changed;
    bool running = false;
    bool release = false;

    octane::transport::detail::BoundedExecutionQueue queue(1, 1);
    check(queue.submit([&] {
        std::unique_lock lock(mutex);
        running = true;
        changed.notify_one();
        changed.wait(lock, [&] { return release; });
    }));

    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return running; });
    }

    check(queue.submit([] {}));
    check(!queue.submit([] {}));

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_one();
}
