#pragma once

#include <functional>
#include <pthread.h>

using AsyncPayload = std::function<std::function<void()>()>;

class MainLoop {
  public:
	MainLoop() = delete;

	static void init();
	static void run_forever();
	static void next_frame(std::function<void()> callback);
	static void RunAsyncThenResume(AsyncPayload payload);
	static void reenter();
	static void reenter_blessed();
	static void play();
  private:
	static std::function<void()> next_callback;
        static bool blessed;  // Blessed by being called from an input event
	static bool playing; // The first click has been given, so we're free to reenter.
	static bool busy;    // currently inside C/C++ code on the main thread
	static int warned;   // control warning spam
	static pthread_t mainThreadId;
};
