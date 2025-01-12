#include "threadpool.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int printMe(void *arg) {
	printf("%d enters function\n", (int)pthread_self());
	int sum=0;
	for (int i=0; i<10000; i++)
		sum+=i;
	printf("%d finish function, sum = %d\n", (int)pthread_self(), sum);	
	return 0;	
}

void* destroy(void* p) {
	threadpool* tp = (threadpool*)p;
	printf("start destroying tp\n");
	destroy_threadpool(tp);
	printf("tp was destroyed\n");
	return 0;
}


int main(int argc,char* args[]) {
	if(argc != 4) {
		printf("Usage: tester <number of thread> <max queue size> <num jobs>\n");
		exit(1);
	}
	int numThreads = atoi(args[1]);
	int qMaxSize = atoi(args[2]);
	int numJobs = atoi(args[3]);
	pthread_t destroyer;
	threadpool* tp =  create_threadpool(numThreads,qMaxSize);
	
	for(int i=0; i<numJobs; i++)
		dispatch(tp, printMe, NULL);
	
	pthread_create(&destroyer, NULL, destroy, tp);
	
	pthread_join(destroyer,NULL);
}
