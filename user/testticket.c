#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc,char *argv[]){


    if( argc<2 ){
        fprintf(2, "Usage: %s sys_call_num command\n", argv[0]);
        exit(1);
    }
    int ticket = atoi(argv[1]);
    if (settickets(ticket) < 0) {
        fprintf(2, "%s: exec failed\n", argv[0]);
        exit(1);
    }
    if( fork() == 0 ){
        while(1);
    }
    exit(0);
}