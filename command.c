#include <unistd.h>
#include <stdio.h>
#include <iostream>
#include <signal.h>
#include <string.h>
#include <string>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>


#define IPC_KEY 2048
#define IPC_PERM 0666


typedef struct volume_control_command
{
  double new_volume;		// New volume to fadeout/fadein
  time_t time_gradient;		// Time take to fadeout/fadein
  
} volume_control_command_t;


int shmid;


void exithl(int s)
{
  if(shmctl(shmid, IPC_RMID, NULL) == -1) 
    fprintf(stderr, "%s\n", strerror(errno));
}

int main(int argn, char *argv[])
{
  volume_control_command_t *volume_command_data;
  std::string command;
  char __command[256];
  double new_vol, time;
  std::string::size_type idx = 0;
  key_t ipc_key = IPC_KEY;
  
  signal(SIGTERM, exithl);
  
  if( (shmid = shmget(ipc_key, sizeof(volume_control_command_t), IPC_CREAT|IPC_PERM)) == -1)
  {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  
  if( (volume_command_data = (volume_control_command_t*)shmat(shmid, NULL, SHM_W)) == (void*)-1)
  {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  
  memset(volume_command_data, 0x0, sizeof(volume_control_command_t));
  volume_command_data->new_volume = 1.0;
  
  do
  {
    memset(__command, 0x0, 256);
    command.clear();
    
    printf("Type 'vol [new volume] [time]' to change volume, 'quit' to exit\n");
    fgets(__command, 256, stdin);
    command = __command;
    
    if(command.substr(0, 3) == "vol")
    {
      try
      {
	new_vol = std::stod(command.substr(3, 256), &idx);
	time = std::stod(command.substr(idx + 3, 256));
	
	volume_command_data->new_volume = new_vol;
	volume_command_data->time_gradient = time;
      }
      catch(std::exception& ex)
      {
	printf("Invalid argument to vol\n");
      }
    }
    
  } while(command.substr(0,4) != "quit");
  
  if(shmctl(shmid, IPC_RMID, NULL) == -1) 
    fprintf(stderr, "%s\n", strerror(errno));
      
  return 0;
}
