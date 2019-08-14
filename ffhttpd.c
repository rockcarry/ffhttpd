#include <stdlib.h>  
#include <stdio.h >  
#include <string.h>

#ifdef __WIN32__
#include <winsock2.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define  closesocket close
#endif

#define FFHTTPD_SERVER_PORT     8080
#define FFHTTPD_MAX_CONNECTION  100

static char *g_ffhttpd_head = 
"HTTP/1.1 200 OK\r\n"
"Server: ffhttpd/1.0.0\r\n"
"Content-Type: text/html\r\n"
"Content-Length: %d\r\n"
"Accept-Ranges: bytes\r\n"
"Connection: close\r\n\r\n";

static int file_load(char *path, char **data)
{
    char *buffer   = NULL;
    int   filesize = 0;
    FILE *fp       = NULL;
    if (!path || !data) return 0;
    fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        buffer = malloc(filesize);
        if (buffer) fread(buffer, filesize, 1, fp);
        fclose(fp);
    }
    *data = buffer;
    return filesize;
}

static void file_free(char **data)
{
    if (data) {
        free(*data);
        *data = NULL;
    }
}

static void parse_string(char *str, char *path, int size)
{
    char *src_start = NULL;
    char *src_end   = NULL;
    char *dst_end   = path + size;
    src_start = strstr(str, "GET /");
    if (src_start) {
        src_start += 5;
        src_end = strstr(src_start, " ");
        while (src_start < src_end && path < dst_end) *path++ = *src_start++;
        if (path == dst_end) path--;
    }
    *path = 0;
}

int main(void)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    int    server_fd, conn_fd, addrlen, length;
    char   buffer[1024], path[1024], *filedata;

#ifdef __WIN32__
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        exit(1);
    }
#endif

    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(FFHTTPD_SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        printf("failed to open socket !\n");
        exit(1);
    }

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        printf("failed to bind !\n");
        exit(1);
    }

    if (listen(server_fd, FFHTTPD_MAX_CONNECTION) == -1) {
        printf("failed to listen !\n");
        exit(1);
    }

    addrlen = sizeof(client_addr);
    while (1) {
        conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (conn_fd == -1) {
            printf("failed to accept !\n");
            continue;
        }

        length = recv(conn_fd, buffer, sizeof(buffer), 0);
        buffer[length] = 0;
        printf(buffer); fflush(stdout);
        parse_string(buffer, path, sizeof(path));
        if (!path[0]) strcpy(path, "index.html");
        length = file_load(path, &filedata);
        snprintf(buffer, sizeof(buffer), g_ffhttpd_head, length);
        if (1     ) send(conn_fd, buffer, strlen(buffer), 0);
        if (length) send(conn_fd, filedata, length, 0);
        file_free(&filedata);
        closesocket(conn_fd);
    }

    closesocket(server_fd);
#ifdef __WIN32__
    WSACleanup();
#endif
}

