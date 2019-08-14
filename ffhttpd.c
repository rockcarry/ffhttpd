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
"Content-Type: %s\r\n"
"Content-Length: %d\r\n"
"Accept-Ranges: bytes\r\n"
"Connection: close\r\n\r\n";

static char *g_content_type_list[][2] = {
    { ".asf" , "video/x-ms-asf"                 },
    { ".avi" , "video/avi"                      },
    { ".bmp" , "application/x-bmp"              },
    { ".css" , "text/css"                       },
    { ".exe" , "application/x-msdownload"       },
    { ".gif" , "image/gif"                      },
    { ".htm" , "text/html"                      },
    { ".html", "text/html"                      },
    { ".ico" , "image/x-icon"                   },
    { ".jpeg", "image/jpeg"                     },
    { ".jpg" , "image/jpeg"                     },
    { ".mp3" , "audio/mp3"                      },
    { ".mp4" , "video/mpeg4"                    },
    { ".pdf" , "application/pdf"                },
    { ".png" , "image/png"                      },
    { ".ppt" , "application/x-ppt"              },
    { ".swf" , "application/x-shockwave-flash"  },
    { ".tif" , "image/tiff"                     },
    { ".tiff", "image/tiff"                     },
    { ".txt" , "text/plain"                     },
    { ".wav" , "audio/wav"                      },
    { ".wma" , "audio/x-ms-wma"                 },
    { ".wmv" , "video/x-ms-wmv"                 },
    { ".xml" , "text/xml"                       },
    {},
};

static char* get_content_type(char *file)
{
    int   i;
    char *ext = file + strlen(file);
    while (ext > file && *ext != '.') ext--;
    if (ext != file) {
        for (i=0; g_content_type_list[i][0]; i++) {
            if (strcasecmp(g_content_type_list[i][0], ext) == 0) {
                return g_content_type_list[i][1];
            }
        }
    }
    return "application/octet-stream";
}

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
        else filesize = 0;
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

static void parse_string(char *str, char *type, int tsize, char *path, int psize)
{
    char *src_start, *src_end, *dst_end;

    src_start = str;
    src_end   = strstr(str, " ");
    dst_end   = type + tsize;
    if (src_start) {
        while (src_start < src_end && type < dst_end) *type++ = *src_start++;
    }
    *type = 0;

    src_start = strstr(str, "/");
    if (src_start) {
        src_start += 1;
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
    char   type[5], recvbuf[1024], sendbuf[1204], path[1024], *args, *data;

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

        length = recv(conn_fd, recvbuf, sizeof(recvbuf), 0);
        recvbuf[length] = 0;
//      printf("request : %s\n", recvbuf); fflush(stdout);
        parse_string(recvbuf, type, sizeof(type), path, sizeof(path));
        if (path[0]) {
            args = strstr(path, "?");
            if (args) *args++ = 0;
        } else {
            strcpy(path, "index.html");
        }
//      printf("path: %s, args: %s\n", path, args);

        if (strcmp(type, "GET") == 0 || strcmp(type, "HEAD") == 0) {
            length = file_load(path, &data);
            snprintf(sendbuf, sizeof(sendbuf), g_ffhttpd_head, get_content_type(path), length);
//          printf("response: %s\n", sendbuf); fflush(stdout);
            send(conn_fd, sendbuf, strlen(sendbuf), 0);
            if (length && strcmp(type, "GET") == 0) {
                send(conn_fd, data, length, 0);
            }
            file_free(&data);
        } else if (strcmp(type, "POST") == 0) {
            data = strstr(recvbuf, "\r\n\r\n");
            if (data) {
                data += 4;
//              printf("POST PATH: %s\n", path);
//              printf("POST DATA: %s\n", data);
                snprintf(sendbuf, sizeof(sendbuf), g_ffhttpd_head, "text/plain", 0);
//              printf("response: %s\n", sendbuf); fflush(stdout);
                send(conn_fd, sendbuf, strlen(sendbuf), 0);
            }
        }
        closesocket(conn_fd);
    }

    closesocket(server_fd);
#ifdef __WIN32__
    WSACleanup();
#endif
}

