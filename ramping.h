#ifndef _RAMPING_H
#define _RAMPING_H


#include <sys/time.h>


extern void ramping_settime(void);

extern double ramping_execute(double target_volume, float time_gradient, double start_volume);


#endif
