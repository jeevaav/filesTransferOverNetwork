#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <stdlib.h>        /* for getenv */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */
#include <netdb.h>
#include "ftree.h"

#ifndef PORT
	#define PORT 30000
#endif

// Note: If there is no permission to open any file/directory, server will skip it. In other words, 
// server will not change the permission to an intermediate state and revert it back.

// helper functions prototypes
static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
int handleclient(struct client *p, struct client *top);
int handleclient_helper(struct client *p) ;
int bindandlisten(void);
void print_request(struct client *client);

// client struct to keep track of the information regarding a client in the server
struct client {
	int fd;		// file descriptor that the client connected to the server
	struct in_addr ipaddr;
	struct client *next;
	int state;		//	the current state of the server to this particular client
	struct request *client_request;		// specific type of request from the client
	int data_status;	// is data transfer completed/incompleted
	int data_written;	// number of bytes written to the file in the server
	FILE* file;		// file that is responsible to be copied by the client
};

/*
 * Helper function to add client in the linked lists of clients that the server keeps track.
 */
static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
	struct client *p = malloc(sizeof(struct client));
   if (!p) {
       perror("malloc");
       exit(1);
   }

	p->client_request = malloc(sizeof(struct request));
	if (!p->client_request) {
		perror("malloc");
		exit(1);
	}
   p->fd = fd;
   p->state = AWAITING_TYPE;
   p->ipaddr = addr;
   p->next = top;	 
   top = p;
   return top;
}

/*
 * Helper function to remove client in the linked lists of clients that the server keeps track.
 */
static struct client *removeclient(struct client *top, int fd) {
   struct client **p;

   for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
   // Now, p points to (1) top, or (2) a pointer to another client
   // This avoids a special case for removing the head of the list
   if (*p) {
       struct client *t = (*p)->next;
       free((*p)->client_request);
       free(*p);
       *p = t;
   } else {
       fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                fd);
   }
   return top;
}

 /* 
  * Bind and listen, abort on error. Returns FD of listening socket.
  */
int bindandlisten(void) {
   struct sockaddr_in r;
   int listenfd;

   if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
       perror("socket");
       exit(1);
   }
   int yes = 1;
   if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
       perror("setsockopt");
   }
   memset(&r, '\0', sizeof(r));
   r.sin_family = AF_INET;
   r.sin_addr.s_addr = INADDR_ANY;
   r.sin_port = htons(PORT);

   if (bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
       perror("bind");
       exit(1);
   }

   if (listen(listenfd, 5)) {
       perror("listen");
       exit(1);
   }
   return listenfd;
}

/*
 * Returns a string which is the basename for the path.
 */
char *basename(const char *path) {
	
	char *s = strrchr(path, '/');
	if (!s) {
		return strdup(path);
	}
    
	return strdup(s + 1);
}

/*
 * Return the size of a file. 
 */
int file_size_finder(FILE *file) {
	fseek(file, 0, SEEK_END); // seek to the end of the file
	int size = ftell(file);  // find the size
	fseek(file, 0, SEEK_SET); // seek back to the beginning of the file
	return size; 
}

/*
 * Return the block_size if the two hashes are same or return the index at which the
 * hash value differ.
 */
int check_hash(const char *hash1, const char *hash2, long block_size) {

	for (int i = 0; i < block_size; i++) {
		if (hash1[i] != hash2[i]) {
			return i;
		}
	}
    return block_size;
}

/*
 * Return whether a file is similar or not based on size and content (hash value).
 */
int cmp_file(FILE *file, int size, char *hash_src) {   
   
	// get the size of the file at the destination
    int dest_size = file_size_finder(file);
	
	// compare the size
   if (size == dest_size) {        
      char hash_val[BLOCKSIZE];	// allocate memory for hash value of the file 
      hash(hash_val, file);		// compute the hash value of the file
      if (check_hash(hash_val, hash_src, BLOCKSIZE) < BLOCKSIZE) {
      	// hash value is not same
   	  	return -1;
   	} 
      return 1; // files are same
   } 
	// size not same
   return -1;
} 

/*
 * Copy the contents of the source file and write to the socket.
 */
int copy_file_to_soc(FILE *source, int fd) {
	// get the sze of the file
	int file_size = file_size_finder(source);
	
	// keep count of the number of bytes read
	int total_num_read = 0;
	
	// allocate memory for the content
	char content[MAXDATA];
	
	// read from the file
	int num_char_read = fread(content, sizeof(char), sizeof(char) * MAXDATA, source);
	
	// increment the total number of bytes read
	total_num_read += num_char_read;
	
	// keep reading until there is no bytes left to read in the file
	while (num_char_read != 0) {
		if (write(fd, content, num_char_read) != num_char_read) {
			fprintf(stderr, "Error: data is not fully written to file\n");
			return -1;	
		}		
		num_char_read = fread(content, sizeof(char), num_char_read, source);
		total_num_read += num_char_read;
	}
	
	// file not fully read, so an error occurred
	if (file_size != total_num_read) {
		return -1;
	} 
	
	// no error occurred
	return file_size;
}


/*
 * Copy the contents of the socket to destination file and update the total
 * number of bytes written at the destination.
 */
int copy_soc_to_file(int fd, FILE *destination, int *total_read) {
	
	// allocate memory to store the data
	char content[MAXDATA];
	
	// read the data from the socket
	int num_read = read(fd, content, MAXDATA);

	// write the read data to the file at the destination or return -1 on error
	if (fwrite(content, sizeof(char), num_read, destination) != num_read) {
		fprintf(stderr, "Error: data is not fully written to file\n");
		return -1;
	}
	
	// update the number of bytes read	
	return *total_read += num_read;
}

/*
 * Initiates a connection from the client to the server. Return the associated fd.  
 */
int create_connection(char *host, unsigned short port) {

	/* fill in peer address */
	struct hostent *hp;
	hp = gethostbyname(host);                
	if ( hp == NULL ) {  
       fprintf(stderr, "%s: unknown host\n", host);
       return -1;
	}
   
	/* create socket */
	int soc;
	if ((soc = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
	}
   
	/* request connection to server */
	struct sockaddr_in peer;
	peer.sin_family = PF_INET;
	peer.sin_port = htons(PORT); 
 	peer.sin_addr = *((struct in_addr *)hp->h_addr);
 	
	if (connect(soc, (struct sockaddr *)&peer, sizeof(peer)) == -1) {  
		perror("client: connect"); 
		close(soc);
		return -1; 
	}
	return soc;
}

/*
 * Send struct request to the server for the REGFILE type.
 */
struct request *send_regfile_request(int type, struct stat *src_stat, int soc, char* source, FILE* src_file, char *path_prefix) {
	
	int error = 0;	// to keep track of error while writing to the socket
	
	struct request *client_request = malloc(sizeof(struct request));
	if (!client_request) {
		perror("client: malloc");
		return NULL;
	}
	
	// write file type
	client_request->type = htonl(type);	// file type
	error += write(soc, &(client_request->type), sizeof(int));	// write the file type to the socket
	
	// write path
	if (strcmp(path_prefix, "") == 0) {
		strncpy(client_request->path, basename(source), MAXPATH - 1);
		client_request->path[MAXPATH - 1] = '\0';
	} else {
		strncpy(client_request->path, path_prefix, strlen(path_prefix));
		client_request->path[strlen(path_prefix)] = '\0';
		strncat(client_request->path, FILE_BREAKER, strlen(FILE_BREAKER));
		strncat(client_request->path, basename(source), strlen(basename(source)));
		client_request->path[MAXPATH - 1] = '\0';
	}
		
	error += write(soc, client_request->path, MAXPATH);
	
	// write mode
	client_request->mode = htonl(src_stat->st_mode); // file mode
	error += write(soc, &(client_request->mode), sizeof(mode_t));

	// write hash value
	hash(client_request->hash, src_file);	// get the hash of the file
	error += write(soc, client_request->hash, BLOCKSIZE);
	
	// write the file size
	client_request->size = htonl(file_size_finder(src_file)); // file size
	error += write(soc, &(client_request->size), sizeof(int));
	
	// if error occurs while writing
	if (error != ((sizeof(int) * 2) + sizeof(mode_t) + BLOCKSIZE + MAXPATH)) {
		return NULL;
	}
	
	return client_request;
}

/*
 * Send struct request to the server for the TRANSFILE type.
 */
int send_transfile_request(struct request *client_request, int soc) {

	int error;
	
	// change the file type and write it to socket
	client_request->type = htonl(TRANSFILE);	
	error = write(soc, &(client_request->type), sizeof(int));
	
	// write the path
	error += write(soc, client_request->path, MAXPATH);

	
	// write mode
	error += write(soc, &(client_request->mode), sizeof(mode_t));

	// write hash value
	error += write(soc, client_request->hash, BLOCKSIZE);
	
	// write the file size
	error += write(soc, &(client_request->size), sizeof(int));
	
	if (error != ((sizeof(int) * 2) + sizeof(mode_t) + MAXPATH + BLOCKSIZE)) {
		return -1;
	}
	
	return 0;
}

/*
 * Send struct request to the server for REGDIR.
 */
int send_regdir_request(int type, struct stat *src_stat, int soc, char* source, DIR* src_file, char *path_prefix) {

	int error = 0;
	
	// write file type
	int file_type = htonl(type);	// file type
	error += write(soc, &file_type, sizeof(int));	// write the file type to the socket
	
	// write path
	char source_name[MAXPATH];	// source basename 
	if (strcmp(path_prefix, "") == 0) {
		strncpy(source_name, basename(source), MAXPATH - 1);
		source_name[MAXPATH - 1] = '\0';
	} else {
		strncpy(source_name, path_prefix, strlen(path_prefix));
		source_name[strlen(path_prefix)] = '\0';
		strncat(source_name, FILE_BREAKER, strlen(FILE_BREAKER));
		strncat(source_name, basename(source), strlen(basename(source)));
		source_name[MAXPATH - 1] = '\0';
	}
	error += write(soc, source_name, MAXPATH);
	
	// write mode
	mode_t file_mode = htonl(src_stat->st_mode); // file mode
	error += write(soc, &file_mode, sizeof(mode_t));

	// write hash value
	char file_hash[BLOCKSIZE] = {0};
	error += write(soc, file_hash, BLOCKSIZE);
	
	// write the file size
	int file_size = htonl(src_stat->st_size); // file size
	error += write(soc, &file_size, sizeof(int));
	
	if (error != ((sizeof(int) * 2) + sizeof(mode_t) + BLOCKSIZE + MAXPATH)) {
		return -1;
	}
	return error;
}

// this counter will keep count the number of errors occurred throghout the copying process
int static global_error = 0;	

/*
 * Send files to the server based on the port number. Return 0 if there is no error, 
 * otherwise it will return 1 to indicate error. 
 */
int rcopy_client(char *source, char *host, unsigned short port, char *path_prefix) {
	
	// create the main client socket
	int soc = create_connection(host, port);
	
	if (soc < 0) {
		// error has occured in establishing the connection
		return 1;
	}
	
	printf("Connected to the server successfully \n");
	
	// call the helper function to get the number of fork() calls made
	int fork_called = rcopy_client_helper(soc, source, host, port, path_prefix);
	
	int return_val = 0;		// counter to keep track of the exit value from the children processes
	
	// call wait for number of fork calls made
	for (int i = 0; i < fork_called; i++) {			
		int status;
		if(wait(&status) == -1) {
			perror("wait");
			return 1;
		} else {
			if (WIFEXITED(status)) {
				char c = WEXITSTATUS(status);
				return_val += c;
			} else {
				// error occured
				return 1;
			}
		}
	}
	
	// close the main client socket
	if(close(soc) == -1) {
		perror("close");
		return 1;
	}
	
	printf("Disconnected from the server successfully \n");
	printf("Number of processes created during the transfer: %d\n", fork_called);
	
	// return 1 if there is an error
	if (return_val + global_error != 0) {
		return 1;
	}
	
	// no error has occured
	return 0;
}

/* 
 * Helper function for the client side to recurse through the directories and call fork to send files.
 * Return the number of fork calls made. 
 */
int rcopy_client_helper(int soc, char *source, char *host, unsigned short port, char *path_prefix) {

	// check if the source exist
	struct stat src_stat;
	if (lstat(source, &src_stat) == -1) {
		perror("lstat");
		global_error++;	// static variable error variable incremented
		return 0;	
	}
	
	int fork_called = 0;
	int response;
	
	// if source is a regular file
	if (S_ISREG(src_stat.st_mode)) {
		FILE *src_file = fopen(source, "r");
		if (src_file == NULL) {
			if(errno == EACCES) {					
				// no permission to open the file
				perror("fopen");
				global_error++;		// static variable error variable incremented
				return 0;
			} 
		}
		
		// send REGFILE request to the server by calling the helper function
		struct request *client_request = send_regfile_request(REGFILE, &src_stat, soc, source, src_file, path_prefix);
		if (client_request == NULL) {
			global_error++;		// static variable error variable incremented
			fprintf(stderr, "REGFILE request is not sent properly to the server for file: %s\n\n", source);
			return 0;
		}
		
		printf("REGFILE request sent for: %s\n\n", client_request->path);
		
		// close the source file
		if (fclose(src_file) != 0) {
			perror("client: fclose");
			global_error++;		// static variable error variable incremented
			return 0;
		}
		
		// read the response from the server regarding the request sent
		int num_read = read(soc, &response, sizeof(int));
		if (num_read != sizeof(int)) {
			perror("client: read");
			global_error++;		// static variable error variable incremented
			return 0;
		}
		
		response = ntohl(response);		// convert to network byte order
		if (response == ERROR) {
			fprintf(stderr, "File: %s has caused an error in the server\n\n", source);
			global_error++;		// static variable error variable incremented
			return 0;
			
		} else if (response == OK) {
			// the file exists in the server
			// free the dynammically allocated memory
			free(client_request); 		 
		
		} else if (response == SENDFILE) {
			// fork and send file to the server
			int p = fork();			
			fork_called += 1;		// fork_called is incremented
			
			// if error occurs on fork call
			if (p < 0) {
				perror("client: fork");
				global_error++;		// static variable error variable incremented
				return 0;
				
			// child process
			} else if (p == 0) {
				// helper function returns a socket fd after initializing connection with server
				int child_sock = create_connection(host, port);
				if (child_sock < 0) {
					// error has occurred for the child process to establish a connection with server
					return ERROR;
				}
			
				// send the TRANSFILE request to the server
				int error = send_transfile_request(client_request, child_sock);
				if (error < 0) {
					fprintf(stderr, "TRANSFILE request is not sent properly to the server for file: %s\n\n", source);
					return ERROR;
				}
				
				printf("TRANSFILE request sent for: %s\n\n", client_request->path);
				
				// open the source file to copy the file content to the socket
				FILE *src_file = fopen(source, "r");
				if (src_file == NULL) {
					return ERROR;
				}
				
				// call the helper function to write the file content to the socket
				int num_write = copy_file_to_soc(src_file, child_sock);
				if (num_write < 0) {
					// error has occurred while reading the data from the file and writing to the socket
					return ERROR;
				}
				
				// close the source file
				if (fclose(src_file) != 0) {
					perror("client: fclose");
					return ERROR;
				}
				
				// read the response from the server
				int response;
				int num_read = read(child_sock, &response, sizeof(int));
				if (num_read != sizeof(int)) {
					perror("client: read");
					return ERROR;
				}
				
				response = ntohl(response);		// convert to host network byte order
					
				if (response == ERROR) {
					fprintf(stderr, "File: %s has caused an error in the server\n\n", source);
				}
				
				// close the child socket
				if(close(child_sock) == -1) {
					perror("client: close");
					return ERROR;
				}
				
				free(client_request);	// free the dynamically allocated memory
				
				// exit with the appropriate response value
				exit(response);
			}			
		}

	// if source is a directory
	} else if (S_ISDIR(src_stat.st_mode)) {
		
		// open the source directory
		DIR *dir = opendir(source);
		if (dir == NULL) {
			perror("client: opendir");
			global_error++;		// static variable error variable incremented		
			return 0;
		}
		
		// send REGDIR request to the server
		int error = send_regdir_request(REGDIR, &src_stat, soc, source, dir, path_prefix);
		if (error < 0) {
			fprintf(stderr, "REGDIR request is not sent properly to the server for file: %s\n\n", source);
			global_error++; 	// static variable error variable incremented			
			return 0;
		}
		
		printf("REGDIR request sent for: %s\n\n", source);
		
		// read the response from the server
		int num_read = read(soc, &response, sizeof(int));
		if (num_read != sizeof(int)) {
			perror("client: read");
			global_error++; 	// static variable error variable incremented	
			return 0;
		}
		
		response = ntohl(response);		// convert the response to the host network byte order
		
		if (response == ERROR) {
			global_error++;		// static variable error variable incremented
			fprintf(stderr, "File: %s has caused an error in the server\n\n", source);
			return 0;
			
		} else if (response == OK) {
			// proceed with traversing down the directory
			// allocate memory to build path prefix
			char path_pre[MAXPATH];	
			if (strcmp(path_prefix, "") == 0) {
				strcpy(path_pre, basename(source));
				path_pre[strlen(basename(source))] = '\0';
			} else {
				strncpy(path_pre, path_prefix, strlen(path_prefix));
				path_pre[strlen(path_prefix)] = '\0';
				strncat(path_pre, FILE_BREAKER, strlen(FILE_BREAKER));
				strncat(path_pre, basename(source), strlen(basename(source)));
				path_pre[MAXPATH - 1] = '\0';
			}
		
			struct dirent *dir_info;
			while ((dir_info = readdir(dir))) {
				
				// excluding the files start with "." in the directory
				if ((dir_info->d_name)[0] != '.')	 {
				
					// allocate memory for the path for the subdirectory files/directories
					char *sub_src_name = malloc(strlen(source) + strlen(FILE_BREAKER) + strlen(dir_info->d_name) + sizeof(char));
					if (!sub_src_name) {
						perror("client: malloc");
						global_error++;		// static variable error variable incremented
						return 0;
					}
					
					// create the new path for a directory entry
					strncpy(sub_src_name, source, strlen(source));
					sub_src_name[strlen(source)] = '\0';
					strncat(sub_src_name, FILE_BREAKER, strlen(FILE_BREAKER));
					strncat(sub_src_name, dir_info->d_name, strlen(dir_info->d_name));
					
					// call the function recursively for the subdirectory contents
					fork_called += rcopy_client_helper(soc, sub_src_name, host, port, path_pre);
					
					// free the dynamically allocated memory
					free(sub_src_name);
				}
			}
			if (closedir(dir) != 0) {
				perror("client: closedir");
				global_error++;		// static variable error variable incremented
				return 0;
			}
		}		
	}
	// return the number of fork calls made
	return fork_called;
}

/* 
 * The server that receives files/directories from multiple clients and stores in the locally.
 * Note: if there is no permission to open any file/directory, server will skip it. In other words, 
 * server will not change the permission to an intermediate state and revert it back.  
 */
void rcopy_server(unsigned short port) {
   
   int clientfd, maxfd, nready;
   struct client *p;
   struct client *head = NULL;
   socklen_t len;
   struct sockaddr_in q;

   fd_set allset;
   fd_set rset;

   int listenfd = bindandlisten();
   // initialize allset and add listenfd to the
   // set of file descriptors passed into select
   FD_ZERO(&allset);
   FD_SET(listenfd, &allset);
   // maxfd identifies how far into the set to search
   maxfd = listenfd;

	while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;

        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
       
        if (nready == -1) {
            perror("select");
            continue;
        }
		
		if (FD_ISSET(listenfd, &rset)){
            len = sizeof(q);
            if ((clientfd = accept(listenfd, (struct sockaddr *)&q, &len)) < 0) {
                perror("accept");
                exit(1);
            }
            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("A new connection from %s\n\n", inet_ntoa(q.sin_addr));
            head = addclient(head, clientfd, q.sin_addr);
        }
		
		for(int i = 0; i <= maxfd; i++) {
            if (FD_ISSET(i, &rset)) {
                for (p = head; p != NULL; p = p->next) {
                    if (p->fd == i) {
                        int result = handleclient(p, head);
                        if (result == -1) {
                            int tmp_fd = p->fd;
                            head = removeclient(head, p->fd);
                            FD_CLR(tmp_fd, &allset);
                            close(tmp_fd);
                        }
                        break;
                    }
                }
            }
        }
	}	
}

/*
 * Helper function to handle the clients of the server. This function will collect the data
 * received from each client and take appropriate actions. 
 */
int handleclient(struct client *p, struct client *top) {

	// server waiting for data and the data is not completely sent
	if (p->state == AWAITING_DATA && p->data_status == INCOMPLETE) {
		
		int response;
		
		// copy data to the file by calling the helper function
		int num_read = copy_soc_to_file(p->fd, p->file, &(p->data_written)); 
		
		// check if the size equals to the data written to the file in the server
		if (p->data_written == p->client_request->size) {
			// close the file because all the data has been copied
			if (fclose(p->file) != 0) {
				perror("server: fclose");
				response = htonl(ERROR);
				printf("Transfer file: Failure: %s\n\n", p->client_request->path);
			}
			
			// change the permission because file transfer is completed
			if (chmod(p->client_request->path, p->client_request->mode & 0777) == -1) {
				perror("server: chmod");
				response = htonl(ERROR);
				printf("Transfer file: Failure: %s\n\n", p->client_request->path);
			} else {
				response = htonl(OK);
				// print to the server about the accomplishment
				printf("Transfer file: Success: %s\n\n", p->client_request->path);
			}
		
			// data copying completed
			p->data_status = COMPLETE;
			
			// write the response to the client
			int num_write = write(p->fd, &response, sizeof(int));
			if (num_write != sizeof(int)) {
				perror("server: write");
				return -1;
			}
				
		} else if (num_read < 0) {
			response = htonl(ERROR);
			printf("Transfer file: Failure: %s\n", p->client_request->path);
			// write the response to the client about the error
			int num_write = write(p->fd, &response, sizeof(int));
			if (num_write != sizeof(int)) {
				perror("server: write");
				return -1;
			}
			return -1;
		} 
		
	// if data status is complete we try to read and remove the client
	} else if (p->state == AWAITING_DATA && p->data_status == COMPLETE) {
		char content[MAXDATA];
		int num_read = read(p->fd, content, MAXDATA);
		if (num_read == 0) {
			// socket is closed 
			printf("Disconnecting from %s\n\n", inet_ntoa(p->ipaddr));
			return -1;
		}
		
	// server waiting for type of the request
    } else if (p->state == AWAITING_TYPE) {
		int num_read = read(p->fd, &(p->client_request->type), sizeof(int));
		if (num_read == 0) {
			// socket is closed 
			printf("Disconnecting from %s\n\n", inet_ntoa(p->ipaddr));
			return -1;
		}
		p->client_request->type = ntohl(p->client_request->type); // convert to host network byte order
		p->state = AWAITING_PATH;	// now the server is waiting for the path
	
	// server waiting for the path
	} else if (p->state == AWAITING_PATH) {
		int num_read = read(p->fd, p->client_request->path, MAXPATH);
		if (num_read == 0) {
			// socket is closed 
			printf("Disconnecting from %s\n\n", inet_ntoa(p->ipaddr));
			return -1;
		}
		p->state = AWAITING_PERM;	// now the server is waiting for the mode
	
	// server waiting for the mode
	} else if (p->state == AWAITING_PERM) {
		int num_read = read(p->fd, &(p->client_request->mode), sizeof(mode_t));
		if (num_read == 0) {
			// socket is closed 
			printf("Disconnecting from %s\n\n", inet_ntoa(p->ipaddr));
			return -1;
		}
		p->client_request->mode = ntohl(p->client_request->mode); // convert to host network byte order
		p->state = AWAITING_HASH;	// now the server is waiting for the hash
	
	// server waiting for the hash value
	} else if (p->state == AWAITING_HASH) {
		int num_read = read(p->fd, p->client_request->hash, BLOCKSIZE);
		if (num_read == 0) {
			// socket is closed 
			printf("Disconnecting from %s\n\n", inet_ntoa(p->ipaddr));
			return -1;
		}
		p->state = AWAITING_SIZE;	// now the server is waiting for the size
	
	// server waiting for the size
	} else if (p->state == AWAITING_SIZE) {
		int num_read = read(p->fd, &(p->client_request->size), sizeof(int));
		if (num_read == 0) {
			// socket is closed 
			printf("Disconnecting from %s\n\n", inet_ntoa(p->ipaddr));
			return -1;
		}
		p->client_request->size = ntohl(p->client_request->size);	// convert to host network byte order
	
		// now we have gotten all the data for the request, print the request nature in the server
		print_request(p);
		
		// call the handleclient_helper function to respond to the client
		int val = handleclient_helper(p);
		if (val < 0) {
			printf("Error occured while the server was responding to the request\n");
		} else {
			printf("Server responded to the request with no error\n\n");
		}
		return val;
	}
	 
	return 0;
}

/*
 * Send response to the client based on the response in the server side for REGFILE,
 * TRANSFILE and REGDIR types.
 */
int handleclient_helper(struct client *p) {
	
	// if the client_request is REGFILE type
	if  (p->client_request->type == REGFILE) {
	
		int response;	
			
		// check for mismatch in the server
		DIR *test_dir = opendir(p->client_request->path);
		if (test_dir != NULL) {
			fprintf(stderr, "Mismatch detected on file: %s\n\n", p->client_request->path);
			response = ERROR;
			if (closedir(test_dir) != 0) {
				perror("server: closedir");
			}
			
		// no mismatch detected
		} else {
			// open the file to check if the file exists
			FILE *src_file_in_dest = fopen(p->client_request->path, "r");
			if (src_file_in_dest == NULL) {
				if (errno != EACCES) {
					// file does not exist
					response = SENDFILE;
				} else {
					// error occured when trying to open the file
					response = ERROR;
				}
					
			} else {
				// file exists, so compare the two files' size and the hash value by calling the helper function
				int compare = cmp_file(src_file_in_dest, p->client_request->size, p->client_request->hash);
				
				if (compare < 0) {
					// files are not same, hence we send the file
					response = SENDFILE;
				} else {
					// files are same, so we change the permission to match the client's file mode
					if (chmod(p->client_request->path, p->client_request->mode & 0777) == -1) {
						perror("server: chmod");
						response = ERROR;
					} else {
						response = OK;
					}
				}
				
				if (fclose(src_file_in_dest) != 0) {
					perror("server: fclose");
					response = ERROR;
				}	
			}	
		}
			
		// write the response to the client
		response = htonl(response); // convert to network byte order
		int num_write = write(p->fd, &response, sizeof(int)); // write to the socket about the response 
		if (num_write != sizeof(int)) {
			perror("server: write");
			return -1;
		}
		
		// set the state to AWAITING_TYPE because the same client can send more requests
		p->state = AWAITING_TYPE;
		
		// 	free the dynammically allocated memory and reallocate
		free(p->client_request);	
		p->client_request = malloc(sizeof(struct request));
		if (!p->client_request) {
			perror("server: malloc");
			return -1;
		}
			
	// if the client_request is TRANSFILE type
	} else if (p->client_request->type == TRANSFILE) {
			
		int response;
		
		// change the state of the server to AWAITING_DATA
		p->state = AWAITING_DATA;
		
		//	open the file that we want to copy and store in the client structure
		p->file = fopen(p->client_request->path, "w");
		if (p->file == NULL) {
			perror("server: fopen");
			response = htonl(ERROR);
			
			// write to the socket about the response
			int num_write = write(p->fd, &response, sizeof(int));  
			if (num_write != sizeof(int)) {
				perror("server: write");
				return -1;
			}
		}
		
		// check if the size of the file that client want to copy is 0 or not
		// if 0, server do not need to wait for any data (0 bytes)
		if (p->client_request->size == 0) {
			p->data_status = COMPLETE;		// data status is completed
			if (fclose(p->file) != 0) {
				perror("server: fclose");
				response = htonl(ERROR);
			} else {
				response = htonl(OK);
			}
			
			// write to the socket about the response
			int num_write = write(p->fd, &response, sizeof(int));
			if (num_write != sizeof(int)) {
				perror("server: write");
				return -1;
			}
			
		// if size is not zero, server will wait for the data and the status is INCOMPLETE
		} else {
			p->data_status = INCOMPLETE;
			p->data_written = 0;	// no bytes has been written yet
		}
	
	// if the client_request is REGDIR type
	} else {
		int response;
			
		// check if there is mismatch in the server
		FILE *test_file = fopen(p->client_request->path, "r+");
		if (test_file != NULL) {
			fprintf(stderr, "Mismatch detected on file: %s\n\n", p->client_request->path);
			response = ERROR;
			if (fclose(test_file) != 0) {
				perror("server: fclose");
			}
			
		// no mismatch detected
		} else {
			DIR *src_file_in_dest = opendir(p->client_request->path);
			if (src_file_in_dest == NULL) {
				if (errno != EACCES) {
					// file does not exist so make directory
					if (mkdir(p->client_request->path, S_IRWXU) < 0) {
						perror("mkdir");
						response = ERROR;
					}
					// change the permissions to match the client directory's mode
					if (chmod(p->client_request->path, p->client_request->mode & 0777) == -1) {
						perror("server: chmod");
						response = ERROR;
					} else {
						response = OK;
					}
				} else {
					// no permission to open the dir
					response = ERROR;
				}
			} else {
				// directory exists and no error has occured
				response = OK;
				if (closedir(src_file_in_dest) != 0) {
					perror("server: closedir");
					response = ERROR;
				}
			}
		}
	
		// write to the socket about the response
		response = htonl(response); // convert to network byte order
		int num_write = write(p->fd, &response, sizeof(int)); 
		if (num_write != sizeof(int)) {
			perror("server: write");
			return -1;
		}
		
		// let it wait for different request
		p->state = AWAITING_TYPE;
		
		// free and reallocate the memory to make available for different request
		free(p->client_request);
		p->client_request = malloc(sizeof(struct request));
		if (!p->client_request) {
			perror("server: malloc");
			return -1;
		}
	}
	
	return 0;
}

/*
 * Helper function to print the type of the request received by the server.
 */
void print_request(struct client *client) {
	
	if (client->client_request->type == REGFILE) {
		printf("A new REGFILE request is received from %s\n", inet_ntoa(client->ipaddr));
		printf("Request is made for the file: %s\n", client->client_request->path);		
	} else if (client->client_request->type == REGDIR) {
		printf("A new REGDIR request is received from %s\n", inet_ntoa(client->ipaddr));
		printf("Request is made for the directory: %s\n", client->client_request->path);	
	} else if (client->client_request->type == TRANSFILE) {
		printf("A new TRANSFILE request is received from %s\n", inet_ntoa(client->ipaddr));
		printf("Request is made for the file: %s\n", client->client_request->path);		
	}
}