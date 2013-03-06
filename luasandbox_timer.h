#ifndef LUASANDBOX_TIMER_H
#define LUASANDBOX_TIMER_H

#include "luasandbox_types.h"

#ifdef CLOCK_REALTIME

#ifdef CLOCK_PROCESS_CPUTIME_ID
#define LUASANDBOX_CLOCK_ID CLOCK_PROCESS_CPUTIME_ID
#else
#define LUASANDBOX_CLOCK_ID CLOCK_REALTIME
#endif

#ifdef SIGRTMIN
#define LUASANDBOX_SIGNAL (SIGRTMIN+5)
#else
#define LUASANDBOX_SIGNAL SIGUSR2
#endif


#else /*CLOCK_REALTIME*/

#define LUASANDBOX_NO_CLOCK

#endif /*CLOCK_REALTIME*/

void luasandbox_timer_install_handler(struct sigaction * oldact);
void luasandbox_timer_remove_handler(struct sigaction * oldact);
void luasandbox_timer_create(luasandbox_timer_set * lts, struct _php_luasandbox_obj * sandbox);
void luasandbox_timer_set_limits(luasandbox_timer_set * lts,
		struct timespec * normal_timeout, 
		struct timespec * emergency_timeout);
int luasandbox_timer_enable_profiler(luasandbox_timer_set * lts, struct timespec * period);
void luasandbox_timer_start(luasandbox_timer_set * lts);
void luasandbox_timer_stop(luasandbox_timer_set * lts);
void luasandbox_timer_destroy(luasandbox_timer_set * lts);
void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts);
void luasandbox_timer_pause(luasandbox_timer_set * lts);
void luasandbox_timer_unpause(luasandbox_timer_set * lts);
int luasandbox_timer_is_paused(luasandbox_timer_set * lts);
void luasandbox_timer_timeout_error(lua_State *L);
int luasandbox_timer_is_expired(luasandbox_timer_set * lts);

#endif /*LUASANDBOX_TIMER_H*/
