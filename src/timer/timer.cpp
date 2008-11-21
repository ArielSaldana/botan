/**
* Timestamp Functions Source File
* (C) 1999-2008 Jack Lloyd
*/

#include <botan/timer.h>
#include <botan/loadstor.h>
#include <botan/util.h>
#include <ctime>

namespace Botan {

/**
* Get the system clock
*/
u64bit system_time()
   {
   return static_cast<u64bit>(std::time(0));
   }

/**
* Read the clock and return the output
*/
u32bit Timer::fast_poll(byte out[], u32bit length)
   {
   const u64bit clock_value = this->clock();

   for(u32bit j = 0; j != sizeof(clock_value); ++j)
      out[j % length] ^= get_byte(j, clock_value);

   return (length < 8) ? length : 8;
   }

u32bit Timer::slow_poll(byte out[], u32bit length)
   {
   return fast_poll(out, length);
   }

/**
* Combine a two time values into a single one
*/
u64bit Timer::combine_timers(u32bit seconds, u32bit parts, u32bit parts_hz)
   {
   static const u64bit NANOSECONDS_UNITS = 1000000000;
   parts *= (NANOSECONDS_UNITS / parts_hz);
   return ((seconds * NANOSECONDS_UNITS) + parts);
   }

/**
* ANSI Clock
*/
u64bit ANSI_Clock_Timer::clock() const
   {
   return combine_timers(std::time(0), std::clock(), CLOCKS_PER_SEC);
   }


}
