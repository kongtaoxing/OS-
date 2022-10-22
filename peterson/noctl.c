#include<stdio.h>
#include<stdlib.h>
#include<pthread.h>
#include<stdint.h>
#include<sys/time.h>

volatile int nAccount1 = 0;
volatile int nAccount2 = 0;
volatile int nLoop = 0;

void* ThreadExecutiveXYF(void* zThreadName) {
	int pThreadName = (int)(uintptr_t)zThreadName;
	int peer = 1 - pThreadName;
	int nTemp1, nTemp2, nRandom;
	do {
		nRandom = rand();
		nTemp1 = nAccount1;
		nTemp2 = nAccount2;
		nAccount1 = nTemp1 + nRandom;
		nAccount2 = nTemp2 - nRandom;
		if (nAccount1 + nAccount2 != 0) {
			printf("Chaos! nAccount1 + nAccount2 = %d\n", nAccount1 + nAccount2);
			break;
		}
		nLoop++;
    	} while (nAccount1 + nAccount2 == 0 && nLoop < 5000000);
    	if (nAccount1 + nAccount2 == 0)
    		printf("5000000 rounds without mistake!\n");
	return (void *)0;
}

int main() {
	struct timeval start;
	gettimeofday(&start, NULL);
	pthread_t p0, p1;
    	srand(0);
    	pthread_create(&p0, NULL, ThreadExecutiveXYF, (void*)0); 
    	pthread_create(&p1, NULL, ThreadExecutiveXYF, (void*)1);
    	pthread_join(p0, NULL);
    	pthread_join(p1, NULL);
    	struct timeval end;
    	gettimeofday(&end, NULL);
    	printf("经历的时间为：%lds, %ldμs。\n", end.tv_sec - start.tv_sec, end.tv_usec - start.tv_usec);
    	printf("Powerd by 夏云峰 20281128.\n");
    	return 0;
}
