///////////////////////////////////////////////////////////////////////////////
// FILE:          LiveClock.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Waiting until a deadline to within ~0.1 ms, for the live
//                loop's frame schedule and the camera's sequence pacing.
//                Windows' Sleep() rounds up to the system timer tick (15.6 ms
//                by default): the live loop slept (exposure - render time) and
//                ran at 64 fps for a 10 ms exposure, 32 fps for 20 ms. Here: a
//                high-resolution waitable timer (Windows 10 1803+), else the
//                1 ms timer period (timeBeginPeriod), and a short spin for the
//                last fraction of a millisecond.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace isc {

class PreciseWaiter
{
public:
   using Clock = std::chrono::steady_clock;

   PreciseWaiter()
   {
#ifdef _WIN32
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (!timer_)
         period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
   }
   ~PreciseWaiter()
   {
#ifdef _WIN32
      if (timer_)
         CloseHandle(timer_);
      if (period_)
         timeEndPeriod(1);
#endif
   }
   PreciseWaiter(const PreciseWaiter&) = delete;
   PreciseWaiter& operator=(const PreciseWaiter&) = delete;

   // Waits until t. abort (optional) is polled at least every 2 ms; true from
   // it ends the wait early. Returns false if aborted.
   bool WaitUntil(Clock::time_point t, const std::function<bool()>& abort = nullptr)
   {
      using namespace std::chrono;
      const auto spin = microseconds(300);
      for (;;)
      {
         if (abort && abort())
            return false;
         const auto now = Clock::now();
         if (now >= t)
            return true;
         const auto left = t - now;
         if (left <= spin)
         {
            std::this_thread::yield();
            continue;
         }
         auto chunk = std::min<Clock::duration>(left - spin, milliseconds(2));
#ifdef _WIN32
         if (timer_)
         {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(duration_cast<nanoseconds>(chunk).count() / 100);
            if (due.QuadPart < 0 && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            {
               WaitForSingleObject(timer_, INFINITE);
               continue;
            }
         }
         const long ms = static_cast<long>(duration_cast<milliseconds>(chunk).count());
         if (ms >= 1)
            Sleep(static_cast<DWORD>(ms));
         else
            std::this_thread::yield();
#else
         std::this_thread::sleep_for(chunk);
#endif
      }
   }
   bool WaitFor(double ms, const std::function<bool()>& abort = nullptr)
   {
      return WaitUntil(Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<double, std::milli>(ms)),
                       abort);
   }

private:
#ifdef _WIN32
   HANDLE timer_ = nullptr;
   bool period_ = false;
#endif
};

} // namespace isc
