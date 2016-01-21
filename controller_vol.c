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


#define IPC_PERM 		0666


typedef struct volume_control_command
{
  double new_volume;		// New volume to fadeout/fadein
  time_t time_gradient;		// Time take to fadeout/fadein
  
} volume_control_command_t;


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


static int ipc_connect(snd_pcm_volume_control_t *volume_controll_data);

static int controll_vol_init(snd_pcm_extplug_t *ext);

static snd_pcm_sframes_t controll_vol_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size);

static snd_pcm_chmap_query_t **controll_vol_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED);

static snd_pcm_chmap_t *controll_vol_get_chmap(snd_pcm_extplug_t *ext);

static int controller_vol_close(snd_pcm_extplug_t *ext);


static const snd_pcm_extplug_callback_t controll_vol_callback = 
{
  .transfer = controll_vol_transfer,
  .init = controll_vol_init,
  .close = controller_vol_close,
  
#if SND_PCM_EXTPLUG_VERSION >= 0x10002
  .query_chmaps = controll_vol_query_chmaps,
  .get_chmap = controll_vol_get_chmap,
#endif
};


int ipc_connect(snd_pcm_volume_control_t *volume_controll_data)
{
  if(( (volume_controll_data->shmid = shmget(volume_controll_data->ipc_key, sizeof(volume_control_command_t), IPC_PERM)) == -1) || 
    ((volume_controll_data->volume_command_data = shmat(volume_controll_data->shmid, NULL, SHM_RDONLY)) == (void*)-1))
  {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  
  return 0;
}

static int controll_vol_init(snd_pcm_extplug_t *ext)
{ 
  snd_pcm_volume_control_t *volume_controll_data;
  int ret;
  
#ifdef DEBUG
  printf("Volume controll plugin: Start to init\n");
#endif
  
  volume_controll_data = ext->private_data;
  
  if((ret = ipc_connect(volume_controll_data)) == -1)
    return -1;
  
  if(volume_controll_data->volume_command_data->new_volume > 1.0f || 
    volume_controll_data->volume_command_data->new_volume < 0.0f)
  {
    SNDERR("controller_vol plugin: The volume value range must be defined from 0.0 to 1.0");
    return -1;
  }
  
  volume_controll_data->current_volume = volume_controll_data->volume_command_data->new_volume;
  volume_controll_data->target_volume = volume_controll_data->current_volume;
  volume_controll_data->start_volume = volume_controll_data->current_volume;
  
#ifdef DEBUG
  printf("Volume controll plugin: Initialized\n");
#endif
  
  return 0;
}

#define MICRO 	1000000L
static struct timeval time_start;

static double ramping(double target_volume, time_t time_gradient, double start_volume)
{
  struct timeval current_time;
  double cvolume, ratio;
  uint32_t time_lapse;
  
  gettimeofday(&current_time, NULL);
  time_lapse = (MICRO * (current_time.tv_sec - time_start.tv_sec)) + (current_time.tv_usec - time_start.tv_usec); 
  
  ratio = (double)time_lapse / (double)time_gradient;
  ratio *= 0.000001;
  
  // WARNING
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

static snd_pcm_sframes_t controll_vol_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size)
{
  int16_t *src, *dst;
  snd_pcm_volume_control_t *volume_controll_data;
  
  volume_controll_data = ext->private_data;
  
  src = (src_areas->addr + ((src_areas->first + (src_areas->step * src_offset)))/8);
  dst = (dst_areas->addr + ((dst_areas->first + (dst_areas->step * dst_offset)))/8);
  
  if(volume_controll_data->current_volume != volume_controll_data->volume_command_data->new_volume)
  {
    if(volume_controll_data->target_volume != volume_controll_data->volume_command_data->new_volume)
    {
      // init ramp counter:
      gettimeofday(&time_start, NULL);
      
      volume_controll_data->target_volume = volume_controll_data->volume_command_data->new_volume;
      volume_controll_data->start_volume = volume_controll_data->current_volume;
    }
    
    volume_controll_data->current_volume = ramping(volume_controll_data->target_volume, 
						   volume_controll_data->volume_command_data->time_gradient,
						   volume_controll_data->start_volume
						  );
  }
  else /* if(volume_controll_data->target_volume != volume_controll_data->current_volume) */
  {
    volume_controll_data->target_volume = volume_controll_data->current_volume;
    volume_controll_data->start_volume = volume_controll_data->current_volume;
  }
  
  apply_volume_to_output(src, dst, volume_controll_data->current_volume, size, ext->channels);
  
  return size;
}

static int controller_vol_close(snd_pcm_extplug_t *ext)
{ 
#ifdef DEBUG
  printf("Volume controll plugin: closed\n");
#endif
  
  free(ext->private_data);
  
  return 0;
}

static snd_pcm_chmap_query_t **controll_vol_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED)
{
#ifdef DEBUG
  printf("fade_query_chmaps");
#endif
  
  return NULL;
}

static snd_pcm_chmap_t *controll_vol_get_chmap(snd_pcm_extplug_t *ext)
{
#ifdef DEBUG  
  printf("fade_get_chmap\n");
#endif
  
  return NULL;
}

SND_PCM_PLUGIN_DEFINE_FUNC(controller_vol)
{
#ifdef DEBUG
  printf("Controller vol\n");
#endif
  
  const char *id;
  snd_pcm_volume_control_t *volume_controll_data;
  snd_config_t *slave = NULL;
  snd_config_t *n;
  
  snd_config_iterator_t i, next;
  int err;
  
  if( (volume_controll_data = malloc(sizeof(snd_pcm_volume_control_t))) == NULL)
    return ENOMEM;
  
  memset(volume_controll_data, 0, sizeof(snd_pcm_volume_control_t));
  volume_controll_data->ipc_key = -1;
  
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
      snd_config_get_integer(n, (long int*)&volume_controll_data->ipc_key);
#ifdef DEBUG
      printf("ipc_key: %d\n", volume_controll_data->ipc_key);
#endif
      continue;
    }
    
    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }
  
  if(!slave)
  {
    SNDERR("No slave defined for controll_vol");
    return -EINVAL;
  }
  
  if(volume_controll_data->ipc_key == -1)
  {
    SNDERR("No ipc_key for fading plugin");
    return -EINVAL;
  }
  
  volume_controll_data->ext.version = SND_PCM_EXTPLUG_VERSION;
  volume_controll_data->ext.name = "ALSA Controller volume plugin with fading support";
  volume_controll_data->ext.callback = &controll_vol_callback;
  volume_controll_data->ext.private_data = volume_controll_data;

  err = snd_pcm_extplug_create(&volume_controll_data->ext, name, root, slave, stream, mode);
  if (err < 0) 
  {
    SNDERR("controller_vol: snd_pcm_extplug_create error");
    return err;
  }
  
  snd_pcm_extplug_set_param_minmax(&volume_controll_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2, 2);
  snd_pcm_extplug_set_slave_param(&volume_controll_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_param(&volume_controll_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  snd_pcm_extplug_set_slave_param(&volume_controll_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  
  
  *pcmp = volume_controll_data->ext.pcm;

  return 0;
}

SND_PCM_PLUGIN_SYMBOL(controller_vol);
 
