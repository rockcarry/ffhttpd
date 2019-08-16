#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef WIN32
#include <winsock2.h>
#ifdef MSVC
#pragma comment(lib, "ws2_32.lib")
#pragma warning(disable:4996)
#define strcasecmp _stricmp
#define snprintf   _snprintf
#endif
#else
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define  closesocket close
#define  SOCKET      int
static char* strlwr(char *s)
{
    char *p = s;
    while (*p) {
        *p += *p > 'A' && *p < 'Z' ? 'a' - 'A' : 0;
         p ++;
    }
    return s;
}
#endif

#define FFHTTPD_SERVER_PORT     8080
#define FFHTTPD_MAX_CONNECTION  10

static char *g_ffhttpd_head =
"HTTP/1.1 200 OK\r\n"
"Server: ffhttpd/1.0.0\r\n"
"Accept-Ranges: bytes\r\n"
"Content-Type: %s\r\n"
"Content-Length: %d\r\n"
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
    { NULL },
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

static int get_file_length(char *file, int start, int end)
{
    int filesize;
    FILE *fp = fopen(file, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    filesize = ftell(fp) - start;
    fclose(fp);
    if (filesize < 0) filesize = 0;
    return (filesize < end - start + 1 ? filesize : end - start + 1);
}

static void send_file_data(SOCKET fd, char *file, int start, int length)
{
    FILE *fp = fopen(file, "rb");
    if (fp) {
        char buf[1024];
        int  ret = 0, n;
        fseek(fp, start, SEEK_SET);
        do {
            n = length < sizeof(buf) ? length : sizeof(buf);
            n = (int)fread(buf, 1, n, fp);
            length -= n > 0 ? n : 0;
            while (n > 0) {
                ret = send(fd, buf, n, 0);
#ifdef WIN32
                if (ret == 0 || (ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINTR)) goto done;
#else
                if (ret == 0 || (ret < 0 && errno != EWOULDBLOCK && errno != EINTR)) goto done;
#endif
                n  -= ret > 0 ? ret : 0;
            }
        } while (length > 0 && !feof(fp));
done:   fclose(fp);
    }
}

static void parse_type_and_path(char *str, char *type, int tsize, char *path, int psize)
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
    dst_end   = path + psize;
    if (src_start) {
        src_start += 1;
        src_end = strstr(src_start, " ");
        while (src_start < src_end && path < dst_end) *path++ = *src_start++;
        if (path == dst_end) path--;
    }
    *path = 0;
}

static void parse_range(char *str, int *start, int *end)
{
    char *header_start, *header_end, *header_buf, *range_start, *range_end;
    *start       = 0;
    *end         = 0x7FFFFFFE;
    header_start = strstr(str, "\r\n");
    header_end   = strstr(str, "\r\n\r\n");
    if (!header_start || !header_end) return;
    header_start+= 2;
    header_end  += 2;
    header_buf   = malloc(header_end - header_start + 1);
    if (header_buf) {
        memcpy(header_buf, header_start, header_end - header_start);
        header_buf[header_end - header_start] = 0;
        strlwr(header_buf);
        range_start = strstr(header_buf, "range");
        if (range_start && (range_end = strstr(range_start, "\r\n"))) {
            if (strstr(range_start, ":") && strstr(range_start, "bytes") && (range_start = strstr(range_start, "="))) {
                range_start += 1;
               *start = atoi(range_start);
                if (*start < 0) {
                    range_start = strstr(range_start, "-");
                    range_start+= 1;
                }
                range_start = strstr(range_start, "-");
                if (range_start && range_start + 1 < range_end) {
                    range_start += 1;
                    *end = atoi(range_start);
                }
            }
        }
        free(header_buf);
    }
}

typedef struct {
    SOCKET          fd;
    char            type[5];
    char            recvbuf[1024];
    char            sendbuf[1024];
    char            path[1024];
    char           *args;
    char           *data;
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    #define CONN_STATUS_RUNNING (1 << 0)
    #define CONN_STATUS_EXIT    (1 << 1)
    int             status;
} CONN;
static CONN g_conn_pool[FFHTTPD_MAX_CONNECTION] = {0};

static void* handle_http_request(void *argv)
{
    CONN *conn = (CONN*)argv;
    int length, range_start, range_end, exitflag = 0;

    while (1) {
        pthread_mutex_lock(&conn->mutex);
        while (!(conn->status & (CONN_STATUS_EXIT|CONN_STATUS_RUNNING))) pthread_cond_wait(&conn->cond, &conn->mutex);
        if ((conn->status & CONN_STATUS_EXIT) && conn->fd >= 0) {
            if (conn->fd != -1) {
                closesocket(conn->fd);
                conn->fd = -1;
            }
            exitflag = 1;
        }
        pthread_mutex_unlock(&conn->mutex);
        if (exitflag) break;

        length = recv(conn->fd, conn->recvbuf, sizeof(conn->recvbuf), 0);
        conn->recvbuf[length] = 0;
        conn->data = strstr(conn->recvbuf, "\r\n\r\n");
        conn->data = conn->data ? conn->data + 4 : NULL;
        printf("request :\n%s\n", conn->recvbuf); fflush(stdout);
        parse_type_and_path(conn->recvbuf, conn->type, sizeof(conn->type), conn->path, sizeof(conn->path));
        parse_range(conn->recvbuf, &range_start, &range_end);
        if (conn->path[0]) {
            conn->args = strstr(conn->path, "?");
            if (conn->args) *conn->args++ = 0;
        } else {
            strcpy(conn->path, "index.html");
        }
        printf("conn->type: %s, conn->path: %s, conn->args: %s, conn->data: %s, range_start: %d, range_end: %d\n\n",
                conn->type, conn->path, conn->args, conn->data, range_start, range_end); fflush(stdout);

        if (strcmp(conn->type, "GET") == 0 || strcmp(conn->type, "HEAD") == 0) {
            length = get_file_length(conn->path, range_start, range_end);
            snprintf(conn->sendbuf, sizeof(conn->sendbuf), g_ffhttpd_head, get_content_type(conn->path), length);
            printf("response:\n%s\n", conn->sendbuf); fflush(stdout);
            send(conn->fd, conn->sendbuf, (int)strlen(conn->sendbuf), 0);
            if (length > 0 && strcmp(conn->type, "GET") == 0) send_file_data(conn->fd, conn->path, range_start, length);
        } else if (strcmp(conn->type, "POST") == 0 && conn->data) {
            snprintf(conn->sendbuf, sizeof(conn->sendbuf), g_ffhttpd_head, "text/plain", 0);
            send(conn->fd, conn->sendbuf, (int)strlen(conn->sendbuf), 0);
        }

        pthread_mutex_lock(&conn->mutex);
        if (conn->fd != -1) {
            closesocket(conn->fd);
            conn->fd = -1;
        }
        conn->status &= ~CONN_STATUS_RUNNING;
        pthread_mutex_unlock(&conn->mutex);
    }
    return NULL;
}

static void conn_pool_init(CONN *pool, int n)
{
    int i;
    for (i=0; i<n; i++) {
        pthread_mutex_init(&pool[i].mutex, NULL);
        pthread_cond_init (&pool[i].cond , NULL);
        pthread_create(&pool[i].thread, NULL, handle_http_request, &pool[i]);
    }
}

static void conn_pool_free(CONN *pool, int n)
{
    int i;
    for (i=0; i<n; i++) {
        pthread_mutex_lock(&pool[i].mutex);
        pool[i].status |= CONN_STATUS_EXIT;
        pthread_cond_signal(&pool[i].cond);
        pthread_mutex_unlock(&pool[i].mutex);
        pthread_join(pool[i].thread, NULL);
    }
}

static void conn_pool_run(CONN *pool, int n, SOCKET connfd)
{
    int flag = 0, i;
    for (i=0; i<n&&!flag; i++) {
        pthread_mutex_lock(&pool[i].mutex);
        if (!(pool[i].status & CONN_STATUS_RUNNING)) {
            pool[i].status |= CONN_STATUS_RUNNING;
            pool[i].fd = connfd;
            pthread_cond_signal(&pool[i].cond);
            flag = 1;
        }
        pthread_mutex_unlock(&pool[i].mutex);
    }
}

int main(void)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    SOCKET server_fd, conn_fd;
    int    addrlen;

#ifdef WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        exit(1);
    }
#endif

    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(FFHTTPD_SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
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
    conn_pool_init(g_conn_pool, FFHTTPD_MAX_CONNECTION);
    while (1) {
        conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, (void*)&addrlen);
        if (conn_fd != -1) conn_pool_run(g_conn_pool, FFHTTPD_MAX_CONNECTION, conn_fd);
        else printf("failed to accept !\n");
    }
    conn_pool_free(g_conn_pool, FFHTTPD_MAX_CONNECTION);

    closesocket(server_fd);
#ifdef WIN32
    WSACleanup();
#endif
    return 0;
}

