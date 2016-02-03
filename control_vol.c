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


#define ERROR(message){			\
		destroy();		\
		puts(message);		\
		return -1;		\
}

#define PLUGIN_NAME		control_vol
#define PLUGIN_DESCRIPTION	"ALSA Controller volume plugin with fading support"

#define IPC_PERM 		0666
#define MICRO 			1000000L



/*************************************** Shared data struct */
typedef struct volume_control_command
{
  double new_volume;		// New volume to fadeout/fadein
  time_t time_gradient;		// Time take to fadeout/fadein
  
} volume_control_command_t;


/*************************************** Main data struct */
typedef struct snd_pcm_volume_control
{
  volume_control_command_t *volume_command_data;
  snd_pcm_extplug_t ext;
  
  double current_volume;	// Current volume of the pcm
  double target_volume;		// Next level volume to switch
  double start_volume;		// Take the current volume when the fading is enable

  key_t ipc_key;
  int shmid;

} snd_pcm_volume_control_t;



static int ipc_connect(void);

static int control_vol_init(snd_pcm_extplug_t *ext);

static void destroy();

static double ramping(double target_volume, time_t time_gradient, double start_volume);

static snd_pcm_sframes_t control_vol_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size);

static snd_pcm_chmap_query_t **control_vol_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED);

static snd_pcm_chmap_t *control_vol_get_chmap(snd_pcm_extplug_t *ext);

static int control_vol_close(snd_pcm_extplug_t *ext);


static const snd_pcm_extplug_callback_t controll_vol_callback = 
{
  .transfer = control_vol_transfer,
  .init = control_vol_init,
  .close = control_vol_close,
  
#if SND_PCM_EXTPLUG_VERSION >= 0x10002
  .query_chmaps = control_vol_query_chmaps,
  .get_chmap = control_vol_get_chmap,
#endif
};


static snd_pcm_volume_control_t *volume_control_data = NULL;

static struct timeval time_start;

const char* message = NULL;



static int ipc_connect(void)
{
  if(( (volume_control_data->shmid = shmget(volume_control_data->ipc_key, sizeof(volume_control_command_t), IPC_PERM)) == -1) || 
    ((volume_control_data->volume_command_data = shmat(volume_control_data->shmid, NULL, SHM_RDONLY)) == (void*)-1))
  {
    message = strerror(errno);
    return -1;
  }
  
  return 0;
}

static int control_vol_init(snd_pcm_extplug_t *ext)
{ 
  int ret;
  
#ifdef DEBUG
  printf("control_vol plugin: Start to init\n");
#endif
 
  if((ret = ipc_connect()) == -1)
    ERROR(message);
  
  if(volume_control_data->volume_command_data->new_volume > 1.0f || 
    volume_control_data->volume_command_data->new_volume < 0.0f)
    ERROR(message);
  
  volume_control_data->current_volume = volume_control_data->volume_command_data->new_volume;
  volume_control_data->target_volume = volume_control_data->current_volume;
  volume_control_data->start_volume = volume_control_data->current_volume;
  
#ifdef DEBUG
  printf("control_vol plugin: Initialized\n");
#endif
  
  return 0;
}

static void destroy(void)
{
  if(volume_control_data != NULL)
  {
    if(volume_control_data->volume_command_data != NULL)
      shmdt(volume_control_data->volume_command_data);
    
    free(volume_control_data);
  }
}

static double ramping(double target_volume, time_t time_gradient, double start_volume)
{
  struct timeval current_time;
  double cvolume, ratio;
  uint32_t time_lapse;
  
  gettimeofday(&current_time, NULL);
  time_lapse = (MICRO * (current_time.tv_sec - time_start.tv_sec)) + (current_time.tv_usec - time_start.tv_usec); 
  
  ratio = (double)time_lapse / (double)time_gradient;
  ratio *= 0.000001;
  
  if(ratio > 1.0f)
    ratio = 1.0f;
  
  cvolume = ((target_volume - start_volume) * ratio) + start_volume;
  
#ifdef DEBUG
  printf("time lapse: %u\n", time_lapse);
  printf("ratio: %f\n", ratio);
  printf("volume computed: %f\n", cvolume);
#endif
    
  return cvolume;
}

static inline void apply_volume_to_output(int16_t *src, int16_t *dst, double volume, int sample_num, int channels_num)
{
  int i, j;
  
  for(i = 0; i < sample_num; i++)
  {
    for(j = 0; j < channels_num; j++)
      dst[i + sample_num * j] = (src[i + sample_num * j] * volume);
  }
}

static snd_pcm_sframes_t control_vol_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size)
{
  int16_t *src, *dst;
  
  src = (src_areas->addr + ((src_areas->first + (src_areas->step * src_offset)))/8);
  dst = (dst_areas->addr + ((dst_areas->first + (dst_areas->step * dst_offset)))/8);
  
  if(volume_control_data->current_volume != volume_control_data->volume_command_data->new_volume)
  {
    if(volume_control_data->target_volume != volume_control_data->volume_command_data->new_volume)
    {
      // init ramp counter:
      gettimeofday(&time_start, NULL);
      
      volume_control_data->target_volume = volume_control_data->volume_command_data->new_volume;
      volume_control_data->start_volume = volume_control_data->current_volume;
    }
    
    volume_control_data->current_volume = ramping(volume_control_data->target_volume, 
						   volume_control_data->volume_command_data->time_gradient,
						   volume_control_data->start_volume
						  );
  }
  else /* if(volume_controll_data->target_volume != volume_controll_data->current_volume) */
  {
    volume_control_data->target_volume = volume_control_data->current_volume;
    volume_control_data->start_volume = volume_control_data->current_volume;
  }
  
  apply_volume_to_output(src, dst, volume_control_data->current_volume, size, ext->channels);
  
  return size;
}

static int control_vol_close(snd_pcm_extplug_t *ext)
{ 
#ifdef DEBUG
  printf("Volume control plugin: closed\n");
#endif
  
  destroy();
  
  return 0;
}

static snd_pcm_chmap_query_t **control_vol_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED)
{
#ifdef DEBUG
  printf("control_vol_query_chmaps");
#endif
  
  return NULL;
}

static snd_pcm_chmap_t *control_vol_get_chmap(snd_pcm_extplug_t *ext)
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
  
  if( (volume_control_data = malloc(sizeof(snd_pcm_volume_control_t))) == NULL)
    return ENOMEM;
  
  memset(volume_control_data, 0, sizeof(snd_pcm_volume_control_t));
  volume_control_data->ipc_key = -1;
  
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
      snd_config_get_integer(n, (long int*)&volume_control_data->ipc_key);
#ifdef DEBUG
      printf("ipc_key: %d\n", volume_control_data->ipc_key);
#endif
      continue;
    }
    
    SNDERR("Unknown field %s", id);
    destroy();
    return -EINVAL;
  }
  
  if(!slave)
    ERROR(message);
  
  if(volume_control_data->ipc_key == -1)
    ERROR(message);
  
  volume_control_data->ext.version = SND_PCM_EXTPLUG_VERSION;
  volume_control_data->ext.name = PLUGIN_DESCRIPTION;
  volume_control_data->ext.callback = &controll_vol_callback;
  volume_control_data->ext.private_data = volume_control_data;

  err = snd_pcm_extplug_create(&volume_control_data->ext, name, root, slave, stream, mode);
  if (err < 0) 
    ERROR(message);
  
  snd_pcm_extplug_set_param_minmax(&volume_control_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2, 2);
  snd_pcm_extplug_set_slave_param(&volume_control_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_param(&volume_control_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  snd_pcm_extplug_set_slave_param(&volume_control_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  
  
  *pcmp = volume_control_data->ext.pcm;

  return 0;
}

SND_PCM_PLUGIN_SYMBOL(PLUGIN_NAME);
 
