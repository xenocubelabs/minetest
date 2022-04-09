#include "mainloop.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cassert>
#include <iostream>
#include <emsocketctl.h>

extern "C" {
	EMSCRIPTEN_KEEPALIVE
	void mainloop_reenter(void);

	EMSCRIPTEN_KEEPALIVE
	void mainloop_reenter_blessed(void);

	EMSCRIPTEN_KEEPALIVE
	void mainloop_play(void);

	EM_BOOL irrlicht_want_pointerlock(void);
}

#define MAX_WARNINGS 100

std::function<void()> MainLoop::next_callback;
bool MainLoop::blessed = false;
bool MainLoop::playing = false;
bool MainLoop::busy = false;
int MainLoop::warned = 0;
pthread_t MainLoop::mainThreadId;


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

void mainloop_reenter(void) {
	MainLoop::reenter();
}

void mainloop_reenter_blessed(void) {
	MainLoop::reenter_blessed();
}

void mainloop_play(void) {
	MainLoop::play();
}

void MainLoop::init() {
	mainThreadId = pthread_self();
	emsocket_init();
	emsocket_set_proxy("wss://minetest.dustlabs.io/proxy");
}

void MainLoop::run_forever() {
        emscripten_set_main_loop(mainloop_reenter, 0, 1);
	assert(0);
}

void MainLoop::next_frame(std::function<void()> resolve) {
	assert(!next_callback);
	next_callback = resolve;
}

void MainLoop::reenter_blessed() {
	// If we're already pointerlocked, there's no need to expedite main re-entry,
	// and it may be better for performance to not do so. (prevent WebGL stalls)
	if (havePointerLock) {
		return;
	}
	blessed = true;
	reenter();
	blessed = false;
}

void MainLoop::reenter() {
	if (!playing) {
		// Don't do anything until we get the PLAY click.
		return;
	}
	if (busy) {
		if (warned < MAX_WARNINGS) {
			std::cout << "[WARNING] MainLoop attempted reentry" << std::endl;
			warned++;
		}
		return;
	}
	if (!pthread_equal(pthread_self(), mainThreadId)) {
		if (warned < MAX_WARNINGS) {
			std::cout << "[WARNING] MainLoop reentry attempted off main thread" << std::endl;
			warned++;
		}
		return;
	}
	busy = true;
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
	busy = false;
}

void MainLoop::play() {
	if (!playing && next_callback) {
		playing = true;
		reenter();
	}
}

void main_resolve(int rv) {
        std::cout << "main() exited with return value " << rv << std::endl;
}

void main2(int argc, char *argv[], std::function<void(int)> resolve);

int main(int argc, char *argv[])
{
        std::cout << "ENTERED main()" << std::endl;
	MainLoop::init();

	emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockchange);
	emscripten_set_pointerlockchange_callback("#canvas", 0, 1, report_pointerlockchange);
	emscripten_set_pointerlockerror_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockerror);
	emscripten_set_pointerlockerror_callback("#canvas", 0, 1, report_pointerlockerror);

        // The first next_frame() is special. It will be called directly from a user-generated click,
        // which allows sound to be initialized. (and maybe full screen mode)
        MainLoop::next_frame([argc, argv]() { main2(argc, argv, main_resolve); });
        std::cout << "Main thread waiting for play signal (click on canvas to activate)" << std::endl;
	EM_ASM({
		mainloop_ready_to_play();
	});
        MainLoop::run_forever();
        std::cout << "THIS SHOULD BE UNREACHABLE" << std::endl;
        return 0;
}

