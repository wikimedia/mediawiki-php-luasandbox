#ifndef LUASANDBOX_TIMER_H
#define LUASANDBOX_TIMER_H

#ifdef CLOCK_REALTIME

#ifdef CLOCK_PROCESS_CPUTIME_ID
#define LUASANDBOX_CLOCK_ID CLOCK_PROCESS_CPUTIME_ID
#else
#define LUASANDBOX_CLOCK_ID CLOCK_REALTIME
#endif

#ifdef SIGRTMIN
#define LUASANDBOX_SIGNAL (SIGRTMIN+1)
#else
#define LUASANDBOX_SIGNAL SIGUSR2
#endif

struct _php_luasandbox_obj;

typedef struct {
	int emergency;
	struct _php_luasandbox_obj * sandbox;
} luasandbox_timer_callback_data;

typedef struct {
	timer_t timer;
	luasandbox_timer_callback_data cbdata;
} luasandbox_timer;

typedef struct {
	luasandbox_timer normal_timer;
	luasandbox_timer emergency_timer;
	struct timespec normal_limit, normal_remaining;
	struct timespec emergency_limit, emergency_remaining;
	struct timespec usage_start, usage;
	struct _php_luasandbox_obj * sandbox;
	int is_running;
	int normal_running;
	int emergency_running;
} luasandbox_timer_set;


#else /*CLOCK_REALTIME*/

#define LUASANDBOX_NO_CLOCK

typedef struct {} luasandbox_timer;
typedef struct {} luasandbox_timer_set;

#endif /*CLOCK_REALTIME*/

void luasandbox_timer_install_handler(struct sigaction * oldact);
void luasandbox_timer_remove_handler(struct sigaction * oldact);
void luasandbox_timer_create(luasandbox_timer_set * lts, struct _php_luasandbox_obj * sandbox);
void luasandbox_timer_set_limits(luasandbox_timer_set * lts,
		struct timespec * normal_timeout, 
		struct timespec * emergency_timeout);
void luasandbox_timer_start(luasandbox_timer_set * lts);
void luasandbox_timer_stop(luasandbox_timer_set * lts);
void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts);
void luasandbox_timer_timeout_error(lua_State *L);
int luasandbox_timer_is_expired(luasandbox_timer_set * lts);

#endif /*LUASANDBOX_TIMER_H*/
