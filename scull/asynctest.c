#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

int gotdata = 0;
void sighandler(int signo)
{
    if(signo==SIGIO)
        gotdata++;
    return;
}

char buffer[4096];

int main(int argc,char argv)
{
    int count;
    struct sigaction action;
    memset(&action,0,sizeof(action));
    action.sa_handler = sighandler;
    action.sa_flags = 0;

    sigaction(SIGIO,&action,NULL);
    
    fcntl(STDIN_FILENO,F_SETOWN,getpid());
    fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|FASYNC);

    while(1) {
        sleep(86400);
        if(!gotdata)
            continue;
        count = read(0,buffer,4096);

        write(1,buffer,count);
        gotdata = 0;
    }
}