#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "ftree.h"


#ifndef PORT
  #define PORT 30000
#endif

int main(int argc, char **argv) {
    /* Note: In most cases, you'll want HOST to be localhost or 127.0.0.1, so 
     * you can test on your local machine.*/
    //char *dest;
    if (argc != 3) {
        printf("Usage:\n\trcopy_client SRC HOST\n");
        printf("\t SRC - The file or directory to copy to the server\n");
        printf("\t HOST - The hostname of the server");
        return 1;
    }
	
	char *path_prefix = ""; // for the given path there is no prefix in the server, so that's why its an empty string
	char source[MAXPATH];
	realpath(argv[1], source); 	// this is to make sure if the path given contains "." or ".." or "../"
	
	printf("Conncecting to the server ...\n");
	
    if (rcopy_client(source, argv[2], PORT, path_prefix) != 0) {
        printf("Errors encountered during copy\n");
        return 1;
    } else {
        printf("Copy completed successfully\n");
        return 0;
    }
}