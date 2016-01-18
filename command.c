#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>


#define IPC_KEY 2048
#define IPC_PERM 0666


typedef struct fading_shared_data
{
  time_t time_gradient;
  time_t time_length;
  float min_value;
  int enable;

} fading_shared_data_t;


int shmid;

void exithl(int s)
{
  if(shmctl(shmid, IPC_RMID, NULL) == -1) 
    fprintf(stderr, "%s\n", strerror(errno));
}

int main(int argn, char *argv[])
{
  fading_shared_data_t *fading_command_data;
  char command[256];
  key_t ipc_key = IPC_KEY;
  
  signal(SIGTERM, exithl);
  
  if( (shmid = shmget(ipc_key, sizeof(fading_shared_data_t), IPC_CREAT|IPC_PERM)) == -1)
  {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  
  if( (fading_command_data = shmat(shmid, NULL, SHM_W)) == (void*)-1)
  {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  
  memset(fading_command_data, 0x0, sizeof(fading_shared_data_t));
  
  do
  {
    memset(command, 0x0, 256);
    printf("Type 'a' to fading, 'quit' to exit\n");
    
    fgets(command, 256, stdin);
    if(strncmp(command, "a", 1) == 0)
      fading_command_data->enable = 1;
  } while(strncmp(command, "quit", 4) != 0);
  
  if(shmctl(shmid, IPC_RMID, NULL) == -1) 
    fprintf(stderr, "%s\n", strerror(errno));
    
  
  return 0;
}
