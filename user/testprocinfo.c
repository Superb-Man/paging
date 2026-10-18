#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/pstat.h"
int
main(int argc,char *argv[]){
    struct pstat pst;
    if (getpinfo(&pst) < 0) {
        fprintf(2, "%s: exec failed\n", argv[0]);
        exit(1);
    }
    printf("PID | In Use | inQ | Original Tickets | Current Tickets | Time Slices\n");
    for (int i = 0; i < NPROC; i++) {
        if(pst.inuse[i] == -1) continue ;
        printf(" %d\t%d\t%d\t\t%d\t\t%d\t\t%d\n",
               pst.pid[i], pst.inuse[i], pst.inQ[i], pst.tickets_original[i], pst.tickets_current[i], pst.time_slices[i]);
    }
    exit(0);
}