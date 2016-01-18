#include <unistd.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/global.h>


#define IPC_PERM 0666


typedef struct fading_shared_data
{
  time_t time_gradient;
  time_t time_length;
  float min_value;
  int enable;

} fading_shared_data_t;

typedef struct snd_pcm_fading
{
  fading_shared_data_t *fading_command_data;
  snd_pcm_extplug_t ext;
  
  double dacay_time;
  double sample_time;
  double natural_dacay_factor;
  double iteration_sum;

  key_t ipc_key;
  int shmid;

} snd_pcm_fading_t;


static int ipc_init(snd_pcm_fading_t *fading_data);

static int fade_init(snd_pcm_extplug_t *ext);

static snd_pcm_sframes_t fade_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size);

static snd_pcm_chmap_query_t **fade_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED);

static snd_pcm_chmap_t *fade_get_chmap(snd_pcm_extplug_t *ext);

static int fade_close(snd_pcm_extplug_t *ext);


static const snd_pcm_extplug_callback_t fading_callback = 
{
  .transfer = fade_transfer,
  .init = fade_init,
  .close = fade_close,
  
#if SND_PCM_EXTPLUG_VERSION >= 0x10002
  .query_chmaps = fade_query_chmaps,
  .get_chmap = fade_get_chmap,
#endif
};


int ipc_init(snd_pcm_fading_t *fading_data)
{
  if(( (fading_data->shmid = shmget(fading_data->ipc_key, sizeof(fading_shared_data_t), IPC_PERM)) == -1) || 
    ((fading_data->fading_command_data = shmat(fading_data->shmid, NULL, SHM_RDONLY)) == (void*)-1))
  {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  
  return 0;
}

static int fade_init(snd_pcm_extplug_t *ext)
{ 
  snd_pcm_fading_t *fading_data;
  int ret;
  
#ifdef DEBUG
  printf("Fading plugin: Start to init\n");
#endif
  
  fading_data = ext->private_data;
  
  if((ret = ipc_init(fading_data)) == -1)
    return -1;
  
  fading_data->dacay_time = 0.0001f;
  fading_data->sample_time = 1.0f / ext->rate;
  fading_data->natural_dacay_factor = exp(-fading_data->sample_time / fading_data->dacay_time);
  fading_data->iteration_sum = 1.0f;
  
#ifdef DEBUG
  printf("Fading plugin: Initialized\n");
#endif
  
  return 0;
}

static inline void apply_fading(snd_pcm_fading_t *fading_data, int *src, int *dst, int n, int m)
{
  int i, j;
  
  for(i = 0; i < n; i++)
  {
    for(j = 0; j < m; j++)
      dst[i + n * j] = (src[i + n * j] * fading_data->iteration_sum);
  }
}

static snd_pcm_sframes_t fade_transfer(snd_pcm_extplug_t *ext,
	       const snd_pcm_channel_area_t *dst_areas,
	       snd_pcm_uframes_t dst_offset,
	       const snd_pcm_channel_area_t *src_areas,
	       snd_pcm_uframes_t src_offset,
	       snd_pcm_uframes_t size)
{
  int *src, *dst;
  snd_pcm_fading_t *fading_data;
  
  fading_data = ext->private_data;
  
  src = (int*)(src_areas->addr + ((src_areas->first + (src_areas->step * src_offset)))/8);
  dst = (int*)(dst_areas->addr + ((dst_areas->first + (dst_areas->step * dst_offset)))/8);
  
  if(fading_data->fading_command_data->enable)
  {
    fading_data->iteration_sum *= fading_data->natural_dacay_factor;
    
#ifdef FINE_GRAIN_DEBUG
    printf("%f\n", fading_data->iteration_sum);
#endif
  
    apply_fading(fading_data, src, dst, size, ext->channels);
  }
  else
    snd_pcm_areas_copy(dst_areas, dst_offset, src_areas, src_offset, ext->channels, size, SND_PCM_FORMAT_S32);
    //pcm_raw_copy(src, dst, size, ext->channels);
  
  return size;
}

static int fade_close(snd_pcm_extplug_t *ext)
{ 
#ifdef DEBUG
  printf("Fading plugin: closed\n");
#endif
  
  free(ext->private_data);
  
  return 0;
}

static snd_pcm_chmap_query_t **fade_query_chmaps(snd_pcm_extplug_t *ext ATTRIBUTE_UNUSED)
{
#ifdef DEBUG
  printf("fade_query_chmaps");
#endif
  
  return NULL;
}

static snd_pcm_chmap_t *fade_get_chmap(snd_pcm_extplug_t *ext)
{
#ifdef DEBUG  
  printf("fade_get_chmap\n");
#endif
  
  return NULL;
}

SND_PCM_PLUGIN_DEFINE_FUNC(fading)
{
#ifdef DEBUG
  printf("Fading plugin called\n");
#endif
  
  const char *id;
  snd_pcm_fading_t *fading_data;
  snd_config_t *slave = NULL;
  snd_config_t *n;
  
  snd_config_iterator_t i, next;
  int err;
  
  fading_data = malloc(sizeof(snd_pcm_fading_t));
  memset(fading_data, 0, sizeof(snd_pcm_fading_t));
  
  fading_data->ipc_key = -1;
  
  if(fading_data == NULL)
    return -ENOMEM;
  
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
    return -EINVAL;
  }
  
  if(!slave)
  {
    SNDERR("No slave defined for _fading_data");
    return -EINVAL;
  }
  
  if(fading_data->ipc_key == -1)
  {
    SNDERR("No ipc_key for fading plugin");
    return -EINVAL;
  }
  
  fading_data->ext.version = SND_PCM_EXTPLUG_VERSION;
  fading_data->ext.name = "Fadeout plugin";
  fading_data->ext.callback = &fading_callback;
  fading_data->ext.private_data = fading_data;

  err = snd_pcm_extplug_create(&fading_data->ext, name, root, slave, stream, mode);
  if (err < 0) 
  {
    SNDERR("snd_pcm_extplug_create error");
    return err;
  }
  
  snd_pcm_extplug_set_param_minmax(&fading_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2, 2);
  snd_pcm_extplug_set_slave_param(&fading_data->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_param(&fading_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S32);
  snd_pcm_extplug_set_slave_param(&fading_data->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S32);
  
  
  *pcmp = fading_data->ext.pcm;

  return 0;
}

SND_PCM_PLUGIN_SYMBOL(fading);
