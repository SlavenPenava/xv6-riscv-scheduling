#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
    if(argc < 2){
    printf("Usage: hog <cycles in millions>\n");
    exit(1);
    }
    
    uint64 limit = atoi(argv[1]) *1000000;
    
    volatile uint64 counter = 0; //volatile so compiler doesnt optimize the loop
    
    printf("Hog (PID %d) starting with limit %dM cycles..\n", getpid(), atoi(argv[1]));
    
    for(uint64 i = 0; i < limit; i++) 
        for(uint64 j = 0; j < 10; j++ )
            counter++;
    
    printf("Hog (PID %d) finished!\n", getpid());
}