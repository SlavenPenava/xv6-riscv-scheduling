#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#define PROC_NUM 4
void
cpu_bound(int loop_count){
    volatile int x = 0;
    for(int i = 0; i < loop_count;i++)
        for(int j = 0; j < 5000000; j++)
            x += 314 * j;
    exit(0);
}
void
io_burst(int loop_count){
    for(int i = 0; i < loop_count; i++){
        volatile int x = 0;
        for(int j = 0; j < 25000000; j++){
            x += 11;
        }
        pause(1);
    }
    exit(0);
}

int
main(int argc, char *argv[]){
    printf("\nMLFQ TEST START\n");
    pause(20);

    if(fork() == 0){
        cpu_bound(2500);
    }

    if(fork() == 0){
        cpu_bound(3500);
    }
   
    if(fork() == 0){
        io_burst(1200);
    }

    if(fork() == 0){
        io_burst(800);
    }

    for(int i =0; i < PROC_NUM; i++)   wait(0);
    
    pause(10);
    printf("\nMLFQ TEST END\n");
    exit(0);
}