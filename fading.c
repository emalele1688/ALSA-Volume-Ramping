#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/global.h>

#include "ramping.h"


#define ERROR(message){			\
		destroy();		\
		puts(message);		\
		return -1;		\
}

#define PLUGIN_NAME		fading
#define PLUGIN_DESCRIPTION	"ALSA fade(out/in) plugin"

#define IPC_PERM 		0666

#define MICRO 			1000000L



/*************************************** Shared data struct */
typedef struct fading_command
{
  float fading_volume;		// Volume to switch out
  float time_gradient;		// Ramp length
  float time_length;		// Time length 
  int enable;
  
} fading_command_t;


/*************************************** Main data struct */
typedef struct snd_pcm_fading
{
  fading_command_t *fading_command_data;
  void (*fading_state)(fading_command_t*);
  
  snd_pcm_extplug_t ext;
  
  float current_volume;		// Volume output
  float start_volume;
  float volume_to_restore;
  
  struct timeval start_to_fading; 
  
  key_t ipc_key;
  int shmid;

} snd_pcm_fading_t;



static int ipc_connect(void);

static int fading_init(snd_pcm_extplug_t *ext);

static void destroy();

static snd_pcm_sframes_t fading_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size);

static snd_pcm_chmap_query_t **fading_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED);

static snd_pcm_chmap_t *fading_get_chmap(snd_pcm_extplug_t *ext);

static int fading_close(snd_pcm_extplug_t *ext);

static void _fade_start(fading_command_t *);

static void _fade_in(fading_command_t *);

static void _fade_keep(fading_command_t *);

static void _fade_out(fading_command_t *);


static const snd_pcm_extplug_callback_t fading_callback = 
{
  .transfer = fading_transfer,
  .init = fading_init,
  .close = fading_close,
  
#if SND_PCM_EXTPLUG_VERSION >= 0x10002
  .query_chmaps = fading_query_chmaps,
  .get_chmap = fading_get_chmap,
#endif
};


static snd_pcm_fading_t *fading_data = NULL;

const char* message = NULL;


static int ipc_connect(void)
{
  if(( (fading_data->shmid = shmget(fading_data->ipc_key, sizeof(fading_command_t), IPC_PERM)) == -1) || 
    ((fading_data->fading_command_data = shmat(fading_data->shmid, NULL, SHM_W)) == (void*)-1))
  {
    message = strerror(errno);
    return -1;
  }
  
  return 0;
}

static int fading_init(snd_pcm_extplug_t *ext)
{ 
  int ret;
  
#ifdef DEBUG
  printf("fading: Start to init\n");
#endif
 
  if((ret = ipc_connect()) == -1)
    ERROR(message);
  
  fading_data->fading_state = _fade_start;
  
  fading_data->current_volume = 1.0f;
  fading_data->start_volume = 1.0f;
  fading_data->volume_to_restore = 1.0f;
  
#ifdef DEBUG
  printf("fading: Initialized\n");
#endif
  
  return 0;
}

static void destroy(void)
{
  if(fading_data != NULL)
  {
    if(fading_data->fading_command_data != NULL)
      shmdt(fading_data->fading_command_data);
    
    free(fading_data);
  }
}

static void _fade_start(fading_command_t *_command_data)
{ 
  ramping_settime();
  fading_data->start_volume = fading_data->current_volume;
  fading_data->volume_to_restore = fading_data->current_volume;
  
  fading_data->fading_state = _fade_in;
  
#ifdef DEBUG
printf("_fade_start:\n\tcurrent_volume: %f\n\tstart_volume: %f\n\tvolume_to_restore: %f\n", 
  fading_data->start_volume, fading_data->start_volume, fading_data->volume_to_restore
);
#endif
  
  _fade_in(_command_data);
}

static void _fade_in(fading_command_t *_command_data)
{
  if((float)fading_data->current_volume == (float)_command_data->fading_volume)
  {
    gettimeofday(&fading_data->start_to_fading, NULL);
    fading_data->fading_state = _fade_keep;
    
    return _fade_keep(_command_data);
  }
  else
    fading_data->current_volume = ramping_execute(_command_data->fading_volume, _command_data->time_gradient, fading_data->start_volume);

#ifdef DEBUG
  printf("_fade_in:\n\tcurrent_volume: %f\n\tfading_volume: %f\n", 
    fading_data->current_volume, _command_data->fading_volume
  );
#endif
}

static void _fade_keep(fading_command_t *_command_data)
{ 
  struct timeval current_time;
  float time_lapse;
  
  gettimeofday(&current_time, NULL);
  time_lapse = (float)((MICRO * (current_time.tv_sec - fading_data->start_to_fading.tv_sec)) + 
    (current_time.tv_usec - fading_data->start_to_fading.tv_usec)); 
  time_lapse /= (float)MICRO;
  
#ifdef DEBUG
  printf("_fade_keep\n\ttime_lapse: %f\n", time_lapse);
#endif
  
  if(time_lapse >= _command_data->time_length)
  {
    ramping_settime();
    fading_data->start_volume = fading_data->current_volume;
    
    fading_data->fading_state = _fade_out;
    _fade_out(_command_data);
  }
}

static void _fade_out(fading_command_t *_command_data)
{  
  if(fading_data->current_volume == fading_data->volume_to_restore)
  {
    fading_data->fading_state = _fade_start;
    fading_data->fading_command_data->enable = 0;
    return;
  }
  else
    fading_data->current_volume = ramping_execute(fading_data->volume_to_restore, _command_data->time_gradient, fading_data->start_volume);

#ifdef DEBUG
  printf("_fade_out:\n\tcurrent_volume: %f\n\tvolume_to_restore: %f\n", fading_data->current_volume, fading_data->volume_to_restore);
#endif
}

static inline void execute_fading(fading_command_t *_command_data)
{ 
  fading_data->fading_state(_command_data);
}

static inline void apply_volume_to_output(int16_t *src, int16_t *dst, float volume, int sample_num, int channels_num)
{
  int i, j;
  
  for(i = 0; i < sample_num; i++)
  {
    for(j = 0; j < channels_num; j++)
      dst[i + sample_num * j] = (src[i + sample_num * j] * volume);
  }
}

static snd_pcm_sframes_t fading_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size)
{
  int16_t *src, *dst;
  
  src = (src_areas->addr + ((src_areas->first + (src_areas->step * src_offset)))/8);
  dst = (dst_areas->addr + ((dst_areas->first + (dst_areas->step * dst_offset)))/8);
  
  if(fading_data->fading_command_data->enable)
    execute_fading(fading_data->fading_command_data);
  
  apply_volume_to_output(src, dst, fading_data->current_volume, size, ext->channels);
  
  return size;
}

static int fading_close(snd_pcm_extplug_t *ext)
{ 
#ifdef DEBUG
  printf("Volume control plugin: closed\n");
#endif
  
  destroy();
  
  return 0;
}

static snd_pcm_chmap_query_t **fading_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED)
{
#ifdef DEBUG
  printf("control_vol_query_chmaps");
#endif
  
  return NULL;
}

static snd_pcm_chmap_t *fading_get_chmap(snd_pcm_extplug_t *ext)
{
#ifdef DEBUG  
  printf("control_vol_get_chmap\n");
#endif
  
  return NULL;
}

SND_PCM_PLUGIN_DEFINE_FUNC(PLUGIN_NAME)
{ 
  const char *id;
  snd_config_t *n, *slave = NULL;
  snd_config_iterator_t i, next;
  int err;
  
  if( (fading_data = malloc(sizeof(snd_pcm_fading_t))) == NULL)
    return ENOMEM;
  
  memset(fading_data, 0, sizeof(snd_pcm_fading_t));
  fading_data->ipc_key = -1;
  
  snd_config_for_each(i, next, conf)
  {
    n = snd_config_iterator_entry(i);
    
    if(snd_config_get_id(n, &id) < 0)
      continue;
    
    if(strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
      continue;
    
    if(strcmp(id, "slave") == 0)
    {
      slave = n;
      continue;
    }
    
    if(strcmp(id, "ipc_key") == 0)
    {
      snd_config_get_integer(n, (long int*)&fading_data->ipc_key);
#ifdef DEBUG
      printf("ipc_key: %d\n", fading_data->ipc_key);
#endif
      continue;
    }
    
    SNDERR("Unknown field %s", id);
    destroy();
    return -EINVAL;
  }
  
  if(!slave)
    ERROR(message);
  
  if(fading_data->ipc_key == -1)
    ERROR(message);
  
  fading_data->ext.version = SND_PCM_EXTPLUG_VERSION;
  fading_data->ext.name = PLUGIN_DESCRIPTION;
  fading_data->ext.callback = &fading_callback;
  fading_data->ext.private_data = fading_data;

  err = snd_pcm_extplug_create(&fading_data->ext, name, root, slave, stream, mode);
  if (err < 0) 
    ERROR(message);
  
  snd_pcm_extplug_set_param_minmax(&fading_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2, 2);
  snd_pcm_extplug_set_slave_param(&fading_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_param(&fading_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  snd_pcm_extplug_set_slave_param(&fading_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  
  
  *pcmp = fading_data->ext.pcm;

  return 0;
}

SND_PCM_PLUGIN_SYMBOL(PLUGIN_NAME); 
