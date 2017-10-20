#include <stdio.h>
#include <stdlib.h>

#define BLOCK_SIZE 8

char *hash(char *hash_val, FILE *f) {
    char ch;
    int hash_index = 0;
	 int index;
    for (index = 0; index < BLOCK_SIZE; index++) {
        hash_val[index] = '\0';
    }

    while(fread(&ch, 1, 1, f) != 0) {
        hash_val[hash_index] ^= ch;
        hash_index = (hash_index + 1) % BLOCK_SIZE;
    }

    return hash_val;
}