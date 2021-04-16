#include <stdio.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#define BACKLOG 8 // Maximum queue length of pending requests
#define PORT 9601 // The server port
#define MAX_MSG_LEN 4096 // The maximum length of a message
#define ADDR "127.0.0.1" // The address to listen on

int server_socket;

void signal_handler(int sig) {
    if (close(server_socket) < 0) {
        perror("close socket");
    }
    printf("Exit by signal %d\n", sig);
    exit(sig);
}

void init_signal_handler(void) {
    signal(SIGINT, signal_handler);  // No  2 interrupt
    signal(SIGQUIT, signal_handler); // No  3 quit
    signal(SIGILL, signal_handler);  // No  4 illegal instruction
    signal(SIGABRT, signal_handler); // No  6 abnormal termination
    signal(SIGFPE, signal_handler);  // No  8 floating point exception
    signal(SIGSEGV, signal_handler); // No 11 segfault
    signal(SIGTERM, signal_handler); // No 15 termination
}


void create_socket() {
    // Socket config data
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET; // Internet protocol
    server_addr.sin_port = htons(PORT); // Convert server port from host byte order into network byte order
    inet_aton(ADDR, &server_addr.sin_addr); // Convert server address into binary form

    // Create socket
    server_socket = socket(server_addr.sin_family, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("create socket");
        exit(EXIT_FAILURE);
    }

    // Bind socket to address
    int err = bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (err) {
        perror("bind socket");
        fprintf(stderr, "ERROR: Can not bind socket %d to address %s:%d\n", server_socket, ADDR, PORT);
        signal_handler(EXIT_FAILURE);
    }
    printf("Bind socket to %s:%d\n", ADDR, ntohs(server_addr.sin_port));

    // Listen to socket
    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen to socket");
        signal_handler(EXIT_FAILURE);
    }
    printf("Listen to socket with backlog %d\n", BACKLOG);
}

void respond_header(FILE* client_stream, size_t content_length, int status) {
    char* status_text;
    switch (status) {
        case 200:
            status_text = "OK";
            break;
        case 404:
            status_text = "Not Found";
            break;
        case 500:
            status_text = "Internal Server Error";
            break;
        default:
            status_text = "Unknown";
            break;
    }
    fprintf(client_stream, "HTTP/1.1 %d %s\n", status, status_text);
    fputs("Server: chttp\n", client_stream);
    fprintf(client_stream, "Content-length: %lu\n", content_length);
    fputs("Content-type: text/html; charset=utf-8\n", client_stream);
    fputs("\r\n", client_stream);
}

void respond_errno(FILE* client_stream, int errno_respond, char* text) {
    char err_msg_template[] = "<h1>Error: %s</h1><p>%s</p>";
    char err_msg[sizeof(err_msg_template) + strlen(text)];

    // Compose message
    sprintf(err_msg, err_msg_template, text, strerror(errno_respond));
    // Send header
    respond_header(client_stream, strlen(err_msg), 500);
    // Send content -> error message
    fputs(err_msg, client_stream);
}

void respond_file(FILE* client_stream, char* path) {
    int file = open(path, O_RDONLY);
    if (file < 0) {
        fprintf(stderr, "Error opening file\n");
        respond_errno(client_stream, errno, "opening file");
        return;
    }

    // Read file stats
    struct stat stats;
    fstat(file, &stats);

    // Map file into memory
    char* file_mmap = mmap(NULL, stats.st_size, PROT_READ, MAP_PRIVATE, file, 0);
    if (file_mmap == MAP_FAILED) {
        respond_errno(client_stream, errno, "reading file");
        return;
    }
    // Send header
    respond_header(client_stream, stats.st_size, 200);
    // Send file
    fwrite(file_mmap, sizeof(char), stats.st_size, client_stream);
    // Unmap file from memory
    munmap(file_mmap, stats.st_size);
}

void respond_directory(FILE* client_stream, char* path) {
    DIR* dir;
    struct dirent *dir_entry;
    dir = opendir(path);
    if (dir == NULL) {
        perror("respond_directory: opendir");
        respond_errno(client_stream, errno, "Could not open directory");
        return;
    }

    // Write headline
    size_t msg_len = 512;
    char* msg = malloc(msg_len);
    if (msg == NULL) {
        perror("respond_directory: malloc msg");
        respond_errno(client_stream, errno, "Could not allocate memory for a response");
        return;
    }
    sprintf(msg, "<h1>Content of %s</h1>", path);

    // Read directory content
    char line[1024];
    while ((dir_entry = readdir(dir)) != NULL) {
        // Build line
        char link[256];
        sprintf(link, "%s/%s", path, dir_entry->d_name);
        sprintf(line, "<a href=\"%s\">%s</a><br>", link, dir_entry->d_name);

        // Check if buffer is large enough to store the old message + the current line
        size_t needed_msg_len = strlen(line) + strlen(msg) + 1;
        if (needed_msg_len > msg_len) {
            // Allocate memory for the old msg + the new line + \0 byte
            char* old_msg = msg;
            msg_len = 2 * needed_msg_len;
            msg = malloc(msg_len);
            if (msg == NULL) {
                perror("respond_directory: malloc msg");
                respond_errno(client_stream, errno, "Could not allocate memory for a response");
                return;
            }
            // Copy old_msg and free its space
            strcpy(msg, old_msg);
            free(old_msg);
        }
        strcat(msg, line);
    }

    respond_header(client_stream, strlen(msg), 200);
    fputs(msg, client_stream);

    if (closedir(dir) < 0) {
        perror("respond_directory: closedir");
        return;
    }
}

void handle_request(int client_socket, struct sockaddr_in client_addr) {
    // Open client socket as stream
    FILE *client_stream = fdopen(client_socket, "r+");
    if (client_stream == NULL) {
        perror("handle_request: fdopen client socket");
        if (close(client_socket) < 0) {
            perror("handle_request: close client socket");
        }
        return;
    }

    char method[8];
    char path[64] = {'.'};
    fscanf(client_stream, "%s %s", method, path+1);

    // Read and ignore header
    char discard[MAX_MSG_LEN];
    do {
        fgets(discard, MAX_MSG_LEN, client_stream);
    } while (memcmp(discard, "\r\n", 2) != 0);

    printf("%s '%s' from %s:%d\n", method, path, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    struct stat path_stat;
    stat(path, &path_stat);

    if (S_ISDIR(path_stat.st_mode)) {
        // Path is directory
        respond_directory(client_stream, path);
    } else if (S_ISREG(path_stat.st_mode)) {
        // Path is file
        respond_file(client_stream, path);
    }

    fclose(client_stream);
    close(client_socket);
}

void accept_incoming_request() {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Accept one request
    int client_socket = accept(server_socket, (struct sockaddr *) &client_addr, &client_addr_len);
    if (client_socket < 0) {
        perror("accept connection on socket");
        return;
    }
    handle_request(client_socket, client_addr);
}

int main() {
    init_signal_handler();
    create_socket();
    while (1) {
        accept_incoming_request();
    }
    return EXIT_SUCCESS;
}
