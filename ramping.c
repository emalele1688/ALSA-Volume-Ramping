#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "ramping.h"


#define MICRO 			1000000L


static struct timeval time_start;


void ramping_settime(void)
{
  gettimeofday(&time_start, NULL);
}

double ramping_execute(double target_volume, float time_gradient, double start_volume)
{
  struct timeval current_time;
  float ratio;
  double cvolume;
  uint32_t time_lapse;
  
  gettimeofday(&current_time, NULL);
  time_lapse = (MICRO * (current_time.tv_sec - time_start.tv_sec)) + (current_time.tv_usec - time_start.tv_usec); 
  time_gradient *= MICRO;
  ratio = ((float)time_lapse) / time_gradient;
  
  if(ratio > 1.0f)
    ratio = 1.0f;
  
  cvolume = ((target_volume - start_volume) * ratio) + start_volume;

#ifdef DEBUG
  printf("execute_ramping:\n\ttime_lapse: %d\n\tratio: %f\n\tvolume computed: %lf\n", time_lapse, ratio, cvolume);
#endif

  return cvolume;
}
