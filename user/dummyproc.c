#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/pstat.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: testticket <number of tickets>\n");
        exit(1);
    }
    int tickets = atoi(argv[1]);
    if(settickets(tickets) == 0) {
        if(fork() == 0) {
            while(1) ;
        }

        exit(0);
    }
    //int pid = fork();
    else {
        printf("error!!Failed to set tickets\n") ;
        exit(-1) ;
    }
}