#ifndef _FTREE_H_
#define _FTREE_H_

#include "hash.h"
#include <sys/stat.h>

#define MAXPATH 128
#define MAXDATA 256

#define FILE_BREAKER "/"

// Input states
#define AWAITING_TYPE 0
#define AWAITING_PATH 1
#define AWAITING_SIZE 2
#define AWAITING_PERM 3
#define AWAITING_HASH 4
#define AWAITING_DATA 5


// Request types
#define REGFILE 1
#define REGDIR 2
#define TRANSFILE 3

#define OK 0
#define SENDFILE 1
#define ERROR 2

#define INCOMPLETE 0
#define COMPLETE 1

#ifndef PORT
    #define PORT 30100
#endif

struct request {
    int type;           // Request type is REGFILE, REGDIR, TRANSFILE
    char path[MAXPATH];
    mode_t mode;
    char hash[BLOCKSIZE];
    int size;
};

int rcopy_client_helper(int soc, char *source, char *host, unsigned short port, char* path_prefix);
void rcopy_server(unsigned short port);
int rcopy_client(char *source, char *host, unsigned short port, char *path_prefix);

#endif // _FTREE_H_
