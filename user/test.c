#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/riscv.h"
#define SZ 200
void swapTestBasic(){
    int n = 100;
    char *pa = sbrk(n * PGSIZE);
    for(int i = 0; i < n ; i++){
        pa[i * PGSIZE] = i;
    }
    for(int i = 0; i < n ; i++){
        if(pa[i * PGSIZE] != i){
            printf("issue %d\n", i);
        }
    }
    pagestats(1);
}
void swapTestFork(){
    int n = 100;
    int pgsize = 4096;
    char* pa = sbrk(n * pgsize);
    pagestats(0) ;
    pagestats(1);
    for(int i = 0; i < n ; i++){
        pa[i * PGSIZE] = i;
    }
    int r = fork();
    for(int i = 0; i < n; i++){
        if(pa[i * PGSIZE] != i){
            printf("issue with block: %d\n", i);
        }
    }
    wait(0);

    if(r) exit(0);

    if(fork() == 0){
        for(int i = 0; i < n; i++){
            pa[i * PGSIZE] = i + 1;
        }
        for(int i = 0; i < n; i++){
            if(pa[i * PGSIZE] != i + 1){
                printf("issue with block: %d\n", i);
            }
        }
        pagestats(0) ;
        sbrk(- n * PGSIZE);
        exit(0);
    }
    else{
        wait(0);
        for(int i = 0; i < n; i++){
            if(pa[i * PGSIZE] != i){
                printf("issue with block: %d\n", i);
            }
        }
        pagestats(1);
    }

}

int main(int argc, char** argv){
        pagestats(1);
        printf("--------------- basic test --------------\n");
        swapTestBasic();
        printf("--------------- Fork test --------------\n");
        swapTestFork();

        pagestats(0) ;

    return 0;
}