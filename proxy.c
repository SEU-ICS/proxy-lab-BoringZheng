#include <stdio.h>
#include <string.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct cache_entry
{
    char key[MAXLINE];
    char *obj;
    size_t size;
    struct cache_entry *prev;
    struct cache_entry *next;
} cache_entry_t;

static cache_entry_t *chead = NULL, *ctail = NULL;
static size_t cache_bytes = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void lru_move_to_head(cache_entry_t *e)
{
    if (!e || e == chead) return;
    e->prev->next = e->next;
    if (e == ctail) ctail = e->prev;
    if (e->next) e->next->prev = e->prev;

    e->prev = NULL;
    e->next = chead;
    if (chead) chead->prev = e;
    chead = e;
    if (!ctail) ctail = e;
    return;
}

static void del_until(size_t need)
{
    while (cache_bytes + need > MAX_CACHE_SIZE && ctail)
    {
        cache_entry_t *vic = ctail;
        ctail = vic->prev;
        if (ctail) ctail->next = NULL;
        else chead = NULL;

        cache_bytes -= vic->size;
        Free(vic->obj);
        Free(vic);
    }
    return;
}

static int cache_get(const char *key, char **out, size_t *outlen)
{
    int hit = 0;
    *out = NULL;
    *outlen = 0;
    pthread_mutex_lock(&cache_mutex);
    for (cache_entry_t *e = chead; e; e = e->next)
    {
        if (!strcmp(e->key, key))
        {
            lru_move_to_head(e);
            char *copy = Malloc(e->size);
            memcpy(copy, e->obj, e->size);
            *out = copy; *outlen = e->size;
            hit = 1;
            break;
        }
    }

    pthread_mutex_unlock(&cache_mutex);
    return hit;
}

static void cache_put(const char *key, const char *buf, size_t len)
{
    if (len ==0 || len > MAX_OBJECT_SIZE) return;
    
    pthread_mutex_lock(&cache_mutex);

    int find_same = 0;
    for (cache_entry_t *e = chead; e; e = e->next)
    {
        if (!strcmp(e->key, key))
        {
            lru_move_to_head(e);
            find_same = 1;
            break;
        }
    }

    if (find_same)
    {
        pthread_mutex_unlock(&cache_mutex);
        return;
    }

     del_until(len);
     cache_entry_t *newe = Malloc(sizeof(cache_entry_t));
     strncpy(newe->key, key, sizeof(newe->key) - 1);
     newe->key[sizeof(newe->key) - 1] = '\0';
     newe->obj = Malloc(len);
     memcpy(newe->obj, buf, len);
     newe->size = len;

     newe->prev = NULL;
     newe->next = chead;
     if (chead) chead->prev = newe;
     else ctail = newe;
     chead = newe;

     cache_bytes += len;
    
     pthread_mutex_unlock(&cache_mutex);
}

static void clienterror(int fd, const char *cause,
                        const char *errnum, const char *shortmsg, const char *longmsg)
{
    char body[MAXBUF], header[MAXBUF];
    int blen = snprintf(body, MAXBUF,
            "<html><title>Proxy Error</title>"
            "<body bgcolor=""ffffff"">\r\n"
            "%s: %s\r\n"
            "<p>%s: %s\r\n"
            "<hr><em>The Proxy Server</em>\r\n",
            errnum, shortmsg, longmsg, cause);
    int hlen = snprintf(header, MAXBUF,
            "HTTP/1.0 %s %s\r\n"
            "Content-type: text/html\r\n"
            "Content-length: %d\r\n\r\n",
            errnum, shortmsg, blen);
    rio_writen(fd, header, hlen);
    rio_writen(fd, body, blen);
}

static void build_request(rio_t *client_rio, const char *host, int serverfd, const char *path)
{
    char buf[MAXBUF];

    snprintf(buf, MAXBUF,
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "%s"
            "Connection: close\r\n"
            "Proxy-Connection: close\r\n"
            "\r\n",
            path, host, user_agent_hdr);

    rio_writen(serverfd, buf, strlen(buf));
}

static int parse_uri(char *uri, char *host, char *port, char *path)
{
    char *p = uri;
    if (!strncasecmp(p, "http://", 7)) p += 7;
    else return -1;

    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < MAXLINE) host[i++] = *p++;
    host[i] = '\0';
    if (i == 0) return -1;

    if (*p == ':')
    {
        p++;
        i = 0;
        while (*p && *p != '/' && i < 15) port[i++] = *p++;
        port[i] = '\0';
    }
    else strcpy(port, "80");

    if (*p == '/')
    {
        i = 0;
        while (*p && i < MAXLINE) path[i++] = *p++;
        path[i] = '\0';
    }
    else strcpy(path, "/");

    return 0;
}

static void serve_client(int connfd)
{
    rio_t client_rio;
    rio_readinitb(&client_rio, connfd);

    char qline[MAXLINE];
    if (rio_readlineb(&client_rio, qline, MAXLINE) <= 0) return;

    char method[16], uri[MAXLINE], version[32];
    if (sscanf(qline, "%s %s %s", method, uri, version) != 3)
    {
        clienterror(connfd, qline, "400", "Bad Request", "Cannot parse request line");
        return;
    }

    if (strcasecmp(method, "GET") != 0)
    {
        clienterror(connfd, method, "501", "Not Implemented", "Proxy only supports GET");
        return;
    }

    char host[MAXLINE], port[16], path[MAXLINE];
    if (parse_uri(uri, host, port, path) < 0)
    {
        clienterror(connfd, uri, "400", "Bad Request", "Malformed URI");
        return;
    }

    char *cached_obj = NULL;
    size_t cached_len = 0;
    if (cache_get(uri, &cached_obj, &cached_len))
    {
        Rio_writen(connfd, cached_obj, cached_len);
        Free(cached_obj);
        return;
    }

    //strcpy(host, "www.seu.edu.cn");
    //strcpy(port, "80");

    int serverfd = open_clientfd(host, port);
    if (serverfd < 0)
    {
        clienterror(connfd, host, "502", "Bad Gateway", "Cannot connect to server");
        return;
    }

    build_request(&client_rio, host, serverfd, path);

    rio_t server_rio;
    rio_readinitb(&server_rio, serverfd);
    char buf[MAXBUF];
    ssize_t n;

    char *obj = Malloc(MAX_OBJECT_SIZE);
    size_t tot = 0;
    int cacheable = 1;

    while( (n = rio_readnb(&server_rio, buf, MAXBUF)) > 0)
    {
        Rio_writen(connfd, buf, n);

        if (cacheable)
        {
            if (tot + n <= MAX_OBJECT_SIZE)
            {
                memcpy(obj + tot, buf, n);
                tot += n;
            }
            else cacheable = 0;
        }
    }

    if (cacheable && tot > 0) cache_put(uri, obj, tot);
    Free(obj);

    close(serverfd);
}

static void *worker(void *vargp)
{
    int connfd = *((int *)vargp);
    free(vargp);

    pthread_detach(pthread_self());
    serve_client(connfd);
    close(connfd);

    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);

    int listenfd = open_listenfd(argv[1]);
    if (listenfd < 0)
    {
        fprintf(stderr, "open_listenfd(%s) failed\n", argv[1]);
        exit(1);
    }

    while (1)
    {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0)
        {
            fprintf(stderr, "accept failed\n");
            continue;
        }
        
        pthread_t tid;
        int *fdp = malloc(sizeof(int));
        if (!fdp)
        {
            close(connfd);
            continue;
        }
        *fdp = connfd;
        pthread_create(&tid, NULL, worker, fdp);
    }

    printf("%s", user_agent_hdr);
    return 0;
}
