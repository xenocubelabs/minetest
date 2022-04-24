#include "mainloop.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cassert>
#include <iostream>
#include <emsocketctl.h>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>


#define MAX_WARNINGS 10

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void emloop_reenter(void);

    EMSCRIPTEN_KEEPALIVE
    void emloop_reenter_blessed(void);

    EMSCRIPTEN_KEEPALIVE
    void emloop_invoke_main(int argc, char* argv[]);

    EM_BOOL irrlicht_want_pointerlock(void);
}

namespace emloop_private {
    std::function<void()> next_callback;
    bool blessed = false;
    bool invokedMain = false;
    bool busy = false; // executing main
    bool dead = false; // Main exited, or got uncaught exception
    int warnCount = 0;
    pthread_t mainThreadId;

    pthread_t helperThread;
    std::mutex helperMutex;
    std::condition_variable helperCond;
    AsyncPayload helperTask;
};

using namespace emloop_private;


static bool havePointerLock = false;
EM_BOOL report_pointerlockchange(int eventType, const EmscriptenPointerlockChangeEvent *pointerlockChangeEvent, void *userData) {
    bool isActive = pointerlockChangeEvent->isActive ? true : false;
    if (havePointerLock != isActive) {
      std::cout << "PointerLockChange: isActive=" << isActive << std::endl;
      havePointerLock = isActive;
    }
    return 0;
}

EM_BOOL report_pointerlockerror(int eventType, const void *reserved, void *userData) {
    std::cout << "PointerLockError!" << std::endl;
    return 0;
}

void* helper_thread_main(void*) {
    std::unique_lock<std::mutex> ul(helperMutex);
    for (;;) {
        while (!helperTask) {
            helperCond.wait(ul);
        }
        AsyncPayload task = std::move(helperTask);
        helperTask = nullptr;
        MainLoop::NextFrame(task());
    }
}

void emloop_init() {
    mainThreadId = pthread_self();
    // Launch async helper thread, used instead of the main thread during blocking network tasks.
    int rc = pthread_create(&helperThread, NULL, helper_thread_main, NULL);
    if (rc != 0) {
        std::cerr << "MainLoop: Failed to launch helper thread" << std::endl;
        abort();
    }
    emsocket_init();
    emsocket_set_proxy("wss://minetest.dustlabs.io/proxy");
}

void MainLoop::RunAsyncThenResume(AsyncPayload payload) {
    {
        std::lock_guard<std::mutex> lock(helperMutex);
        assert(!helperTask);
        helperTask = payload;
    }
    helperCond.notify_all();
}

void MainLoop::NextFrame(std::function<void()> resolve) {
    assert(!next_callback);
    next_callback = resolve;
}

void emloop_reenter_blessed(void) {
    // If we're already pointerlocked, there's no need to expedite main re-entry,
    // and it may be better for performance to not do so. (prevents WebGL stalls)
    if (havePointerLock) {
        return;
    }
    blessed = true;
    emloop_reenter();
    blessed = false;
}

void emloop_reenter(void) {
    if (dead || !invokedMain) {
        return;
    }
    if (busy) {
        if (warnCount < MAX_WARNINGS) {
            std::cout << "[WARNING] MainLoop attempted reentry" << std::endl;
            warnCount++;
        }
        return;
    }
    if (!pthread_equal(pthread_self(), mainThreadId)) {
        if (warnCount < MAX_WARNINGS) {
            std::cout << "[WARNING] MainLoop reentry attempted off main thread" << std::endl;
            warnCount++;
        }
        return;
    }
    busy = true;
    try {
        if (next_callback) {
            std::function<void()> callback = std::move(next_callback);
            next_callback = nullptr;
            callback();

            // Adjust pointer lock
            bool wantPointerLock = irrlicht_want_pointerlock();
            if (blessed && !havePointerLock && wantPointerLock) {
                emscripten_request_pointerlock("#canvas", EM_FALSE);
            } else if (havePointerLock && !wantPointerLock) {
                emscripten_exit_pointerlock();
            }
        }
    } catch (const std::exception &exc) {
        std::cout << "Unhandled exception: " << exc.what() << std::endl;
        busy = false;
        dead = true;
        throw;
    } catch (...) {
        std::cout << "Unhandled exception (non-standard)" << std::endl;
        busy = false;
        dead = true;
        throw;
    }
    busy = false;
}

void main_resolve(int rv) {
    std::cout << "main() exited with return value " << rv << std::endl;
}

void main2(int argc, char *argv[], std::function<void(int)> resolve);

void emloop_invoke_main(int argc, char* argv[]) {
    // Caller must guarantee that argv remains valid forever.
    if (invokedMain) {
        std::cout << "ERROR: emloop_invoke_main called more than once" << std::endl;
        return;
    }
    invokedMain = true;
    MainLoop::NextFrame([argc, argv]() {
        main2(argc, argv, main_resolve);
    });
    blessed = true;
    emloop_reenter();
    blessed = false;
}

int main(int argc, char *argv[])
{
    std::cout << "ENTERED main()" << std::endl;
    emloop_init();

    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockchange_callback("#canvas", 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockerror_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockerror);
    emscripten_set_pointerlockerror_callback("#canvas", 0, 1, report_pointerlockerror);

    // The first next_frame() is special. It will be called directly from a user-generated click,
    // which allows sound to be initialized. (and maybe full screen mode)
    std::cout << "Main thread waiting for play signal (click on canvas to activate)" << std::endl;
    EM_ASM({
        emloop_ready();
    });
    emscripten_set_main_loop(emloop_reenter, 0, 1);
    std::cout << "THIS SHOULD BE UNREACHABLE" << std::endl;
    return 0;
}

