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

typedef struct {
	int emergency;
	php_luasandbox_obj * sandbox;
} luasandbox_timer_callback_data;

typedef struct {
	timer_t timer;
	luasandbox_timer_callback_data cbdata;
} luasandbox_timer;

typedef struct {
	luasandbox_timer normal_timer;
	luasandbox_timer emergency_timer;
	int use_emergency;
	php_luasandbox_obj * sandbox;
} luasandbox_timer_set;


#else

#define LUASANDBOX_NO_CLOCK

typedef struct {} luasandbox_timer;

#endif

void luasandbox_timer_install_handler(struct sigaction * oldact);
void luasandbox_timer_remove_handler(struct sigaction * oldact);
void luasandbox_timer_start(luasandbox_timer_set * lts, 
		php_luasandbox_obj * sandbox,
		struct timespec * normal_timeout,
		struct timespec * emergency_timeout);
void luasandbox_timer_stop(luasandbox_timer_set * lts);


#endif
