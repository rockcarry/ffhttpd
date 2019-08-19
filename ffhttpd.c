#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>

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
#include <arpa/inet.h>
#include <netinet/in.h>
#define  closesocket close
#define  SOCKET      int
static char* strlwr(char *s)
{
    char *p = s;
    while (*p) { *p += *p > 'A' && *p < 'Z' ? 'a' - 'A' : 0; p ++; }
    return s;
}
#endif

#define FFHTTPD_SERVER_PORT      8080
#define FFHTTPD_MAX_CONNECTIONS  16
#define FFHTTPD_MAX_WORK_THREADS FFHTTPD_MAX_CONNECTIONS

static char *g_ffhttpd_head1 =
"HTTP/1.1 200 OK\r\n"
"Server: ffhttpd/1.0.0\r\n"
"Accept-Ranges: bytes\r\n"
"Content-Type: %s\r\n"
"Content-Length: %d\r\n"
"Connection: close\r\n\r\n";

static char *g_ffhttpd_head2 =
"HTTP/1.1 206 Partial Content\r\n"
"Server: ffhttpd/1.0.0\r\n"
"Content-Range: bytes %d-%d/%d\r\n"
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
    { ".mp4" , "video/mp4"                      },
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

static void get_file_range_size(char *file, int *start, int *end, int *size)
{
    FILE *fp = fopen(file, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        *size = ftell(fp);
        fclose(fp);
        if (*start < 0) *start = *size + *start;
        if (*end >= *size) *end = *size - 1;
    } else {
        *start = *end = *size = 0;
    }
}

static void send_file_data(SOCKET fd, char *file, int start, int end)
{
    FILE *fp = fopen(file, "rb");
    if (fp) {
        char buf[1024];
        int  len = end - start + 1, ret = 0, n;
        fseek(fp, start, SEEK_SET);
        do {
            n = len < sizeof(buf) ? len : sizeof(buf);
            n = (int)fread(buf, 1, n, fp);
            len -= n > 0 ? n : 0;
            while (n > 0) {
                ret = send(fd, buf, n, 0);
#ifdef WIN32
                if (ret == 0 || (ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINTR)) goto done;
#else
                if (ret == 0 || (ret < 0 && errno != EWOULDBLOCK && errno != EINTR)) goto done;
#endif
                n  -= ret > 0 ? ret : 0;
            }
        } while (len > 0 && !feof(fp));
done:   fclose(fp);
    }
}

static void parse_range_datasize(char *str, int *partial, int *start, int *end, int *size)
{
    char *range_start, *range_end, *temp;
    *start = 0;
    *end   = 0x7FFFFFFF;
    *size  = 0;
    if (!str) return;
    range_start = strstr(str, "range");
    if (range_start && (range_end = strstr(range_start, "\r\n"))) {
        if (strstr(range_start, ":") && strstr(range_start, "bytes") && (range_start = strstr(range_start, "="))) {
            range_start += 1;
            *start = atoi(range_start);
            if (*start < 0) {
                range_start  = strstr(range_start, "-");
                range_start += 1;
            }
            range_start = strstr(range_start, "-");
            if (range_start && range_start + 1 < range_end) {
                range_start += 1;
                *end = atoi(range_start);
            }
        }
    }
    temp = strstr(str, "content-length");
    if (temp) {
        temp += 14;
        temp  = strstr(temp, ":");
        if (temp) *size = atoi(temp+1);
    }
    *partial = !!range_start;
}

typedef struct {
    int    head;
    int    tail;
    int    size; // size == -1 means exit
    SOCKET conns[FFHTTPD_MAX_CONNECTIONS];
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       threads[FFHTTPD_MAX_WORK_THREADS];
} THEADPOOL;

static SOCKET threadpool_dequeue(THEADPOOL *tp)
{
    SOCKET fd = -1;
    pthread_mutex_lock(&tp->mutex);
    while (tp->size == 0) pthread_cond_wait(&tp->cond, &tp->mutex);
    if (tp->size != -1) {
        fd = tp->conns[tp->head++ % FFHTTPD_MAX_CONNECTIONS];
        tp->size--;
        pthread_cond_signal(&tp->cond);
    }
    pthread_mutex_unlock(&tp->mutex);
    return fd;
}

static void threadpool_enqueue(THEADPOOL *tp, SOCKET fd)
{
    pthread_mutex_lock(&tp->mutex);
    while (tp->size == FFHTTPD_MAX_CONNECTIONS) pthread_cond_wait(&tp->cond, &tp->mutex);
    if (tp->size != -1) {
        tp->conns[tp->tail++ % FFHTTPD_MAX_CONNECTIONS] = fd;
        tp->size++;
        pthread_cond_signal(&tp->cond);
    }
    pthread_mutex_unlock(&tp->mutex);
}

static void* handle_http_request(void *argv)
{
    THEADPOOL *tp = (THEADPOOL*)argv;
    int    range_start, range_end, datasize, partial;
    char  *request_header, *request_data = NULL, *request_type = NULL, *request_path = NULL, *url_args = NULL;
    char   recvbuf[1024], sendbuf[1024];
    SOCKET conn_fd;

    while ((conn_fd = threadpool_dequeue(tp)) != -1) {
        datasize = recv(conn_fd, recvbuf, sizeof(recvbuf), 0);
        recvbuf[datasize < sizeof(recvbuf) ? datasize : sizeof(recvbuf) - 1] = 0;
        printf("request :\n%s\n", recvbuf); fflush(stdout);

        request_header = strstr(recvbuf, "\r\n");
        if (request_header) {
            request_header[0] = 0;
            request_header   += 2;
            strlwr(request_header);
            request_data = strstr(request_header, "\r\n\r\n");
            if (request_data) {
                request_data[0] = 0;
                request_data   += 4;
            }
        }
        request_type = recvbuf;
        request_path = strstr(recvbuf, " ");
        if (request_path) {
           *request_path++ = 0;
            request_path   = strstr(request_path, "/");
        }
        if (request_path) {
            request_path  += 1;
            url_args = strstr(request_path, " ");
            if (url_args) *url_args   = 0;
            url_args = strstr(request_path, "?");
            if (url_args) *url_args++ = 0;
            if (!request_path[0]) request_path = "index.html";
        }

        parse_range_datasize(request_header, &partial, &range_start, &range_end, &datasize);
        printf("request_type: %s, request_path: %s, url_args: %s, request_data: %s\n", request_type, request_path, url_args, request_data); fflush(stdout);

        if (strcmp(request_type, "GET") == 0 || strcmp(request_type, "HEAD") == 0) {
            get_file_range_size(request_path, &range_start, &range_end, &datasize);
            if (!partial) {
                snprintf(sendbuf, sizeof(sendbuf), g_ffhttpd_head1, get_content_type(request_path), datasize);
            } else {
                snprintf(sendbuf, sizeof(sendbuf), g_ffhttpd_head2, range_start, range_end, datasize, get_content_type(request_path), datasize ? range_end - range_start + 1 : 0);
            }
            printf("response:\n%s\n", sendbuf); fflush(stdout);
            send(conn_fd, sendbuf, (int)strlen(sendbuf), 0);
            if (strcmp(request_type, "GET") == 0) send_file_data(conn_fd, request_path, range_start, range_end);
        } else if (strcmp(request_type, "POST") == 0) {
            snprintf(sendbuf, sizeof(sendbuf), g_ffhttpd_head1, "text/plain", 0);
            send(conn_fd, sendbuf, (int)strlen(sendbuf), 0);
//          printf("\nhttp post request\npath = %s, args = %s, data length = %d, data buffer = %s\n\n", request_path, url_args, datasize, request_data); fflush(stdout);
        }
        closesocket(conn_fd);
    }
    return NULL;
}

static void threadpool_init(THEADPOOL *tp)
{
    int i;
    memset(tp, 0, sizeof(THEADPOOL));
    pthread_mutex_init(&tp->mutex, NULL);
    pthread_cond_init (&tp->cond , NULL);
    for (i=0; i<FFHTTPD_MAX_WORK_THREADS; i++) pthread_create(&tp->threads[i], NULL, handle_http_request, tp);
}

static void threadpool_free(THEADPOOL *tp)
{
    int i;
    pthread_mutex_lock(&tp->mutex);
    tp->size = -1;
    pthread_cond_broadcast(&tp->cond);
    pthread_mutex_unlock(&tp->mutex);
    for (i=0; i<FFHTTPD_MAX_WORK_THREADS; i++) pthread_join(tp->threads[i], NULL);
    pthread_mutex_destroy(&tp->mutex);
    pthread_cond_destroy (&tp->cond );
}

static int g_exit_server = 0;
static void sig_handler(int sig)
{
    struct sockaddr_in server_addr;
    SOCKET client_fd;
    printf("sig_handler %d\n", sig); fflush(stdout);
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        g_exit_server = 1;
        client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd != -1) {
            server_addr.sin_family      = AF_INET;
            server_addr.sin_port        = htons(FFHTTPD_SERVER_PORT);
            server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
                closesocket(client_fd);
            }
        }
        break;
    }
}

int main(void)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    SOCKET    server_fd, conn_fd;
    int       addrlen = sizeof(client_addr);
    THEADPOOL thread_pool;

#ifdef WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        exit(1);
    }
#endif

    signal(SIGINT , sig_handler);
    signal(SIGTERM, sig_handler);

    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(FFHTTPD_SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("failed to open socket !\n"); fflush(stdout);
        exit(1);
    }

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        printf("failed to bind !\n"); fflush(stdout);
        exit(1);
    }

    if (listen(server_fd, FFHTTPD_MAX_CONNECTIONS) == -1) {
        printf("failed to listen !\n"); fflush(stdout);
        exit(1);
    }

    threadpool_init(&thread_pool);
    while (!g_exit_server) {
        conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, (void*)&addrlen);
        if (conn_fd != -1) threadpool_enqueue(&thread_pool, conn_fd);
        else printf("failed to accept !\n");
    }
    threadpool_free(&thread_pool);

    closesocket(server_fd);
#ifdef WIN32
    WSACleanup();
#endif
    return 0;
}

