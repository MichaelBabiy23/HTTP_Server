#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/stat.h>
#include <libgen.h>
#include <dirent.h>
#include <time.h>
#include "threadpool.h"

#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
#define FIRST_LINE_SIZE 4000
#define INITIAL_BUFFER_SIZE 8192

/* Toggle debug prints via DEBUG macro */
#define DEBUG 1
#if DEBUG
  #define DEBUG_PRINT(fmt, ...) fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__)
#else
  #define DEBUG_PRINT(fmt, ...) do {} while (0)
#endif

typedef struct {
    char method[FIRST_LINE_SIZE];
    char path[FIRST_LINE_SIZE];
    char version[FIRST_LINE_SIZE];
} request_st;

/* Function declarations (some are from your original code, lightly edited) */
int handle_request(void *arg);
int parse_request(const char *req_line, request_st *req);
int construct_OK_body(char *path, unsigned char **body, int *size);
int generate_directory_listing(const char *dir, char **html_body, int *is_alloc, int *body_size);
int open_read_file(unsigned char **body, char *path, int *size);
int find_index_html(const char *dir);
int has_execute_permissions(const char *full_path);
int is_directory(const char *p);
char *get_last_modified_date(const char *path);
char *get_mime_type(char *filename);
void get_current_date(char *buf, size_t len);
unsigned char *build_http_response(int code, char *path, const unsigned char *body, long b_len, int *resp_len);
void write_to_client(int fd, unsigned char *buf, int len);

static const char *USAGE = "Usage: server <port> <pool-size> <max-queue-size> <max-number-of-request>\n";

/* Main server routine: sets up socket, threadpool, accepts connections, etc. */
int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "%s", USAGE);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    int poolSize = atoi(argv[2]);
    int qMaxSize = atoi(argv[3]);
    int maxRequests = atoi(argv[4]);

    threadpool *tp = create_threadpool(poolSize, qMaxSize);
    if (!tp) {
        fprintf(stderr, "Cannot create thread pool\n");
        exit(EXIT_FAILURE);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        destroy_threadpool(tp);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        destroy_threadpool(tp);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        destroy_threadpool(tp);
        exit(EXIT_FAILURE);
    }

    DEBUG_PRINT("Server listening on port %d\n", port);

    int count = 0;
    while (count < maxRequests) {
        struct sockaddr_in client_addr;
        socklen_t c_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &c_len);
        if (client_fd < 0) {
            perror("accept");
            break;
        }
        int *arg_fd = malloc(sizeof(int));
        if (!arg_fd) {
            perror("malloc");
            close(client_fd);
            break;
        }
        *arg_fd = client_fd;
        dispatch(tp, handle_request, arg_fd);
        count++;
        DEBUG_PRINT("Accepted request #%d\n", count);
    }

    destroy_threadpool(tp);
    close(server_fd);
    return 0;
}

/* Thread function for handling a single client */
int handle_request(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);  /* Freed after pulling from queue */

    /* We read only the first line to parse "METHOD PATH VERSION" */
    char req_buffer[FIRST_LINE_SIZE + 1];
    memset(req_buffer, 0, sizeof(req_buffer));

    int total_read = 0;
    int found_line = 0;
    while (total_read < FIRST_LINE_SIZE) {
        ssize_t bytes = read(client_fd, req_buffer + total_read, 1);
        if (bytes < 1) { /* error or EOF */
            break;
        }
        total_read++;
        if (total_read >= 2 &&
            req_buffer[total_read-2] == '\r' &&
            req_buffer[total_read-1] == '\n') {
            /* Found "\r\n" -> isolate the line and stop */
            req_buffer[total_read - 2] = '\0';
            found_line = 1;
            break;
        }
    }

    /* If no line found or we overflowed, return 400 immediately */
    if (!found_line) {
        int tmpl;
        unsigned char *resp400 = build_http_response(400, NULL, NULL, 0, &tmpl);
        write_to_client(client_fd, resp400, tmpl);
        free(resp400);
        close(client_fd);
        return 0;
    }

    DEBUG_PRINT("Request line: %s\n", req_buffer);

    request_st req;
    memset(&req, 0, sizeof(req));
    int code = parse_request(req_buffer, &req);

    /* If parse gave us 200, we still need to build the body (file or directory). */
    unsigned char *body = NULL;
    int body_size = 0;
    if (code == 200) {
        if (construct_OK_body(req.path, &body, &body_size) != 0) {
            code = 500;
        }
    }

    int resp_len = 0;
    unsigned char *resp = build_http_response(code,
                                              req.path,
                                              body,
                                              body_size,
                                              &resp_len);

    write_to_client(client_fd, resp, resp_len);

    if (body) free(body);
    if (resp) free(resp);

    close(client_fd);
    return 0;
}

/* Parses "METHOD PATH VERSION" from the request line */
int parse_request(const char *req_line, request_st *req) {
    if (sscanf(req_line, "%s %s %s", req->method, req->path, req->version) != 3) {
        return 400; /* Bad request if not exactly 3 tokens */
    }

    /* Only GET is supported */
    if (strcasecmp(req->method, "GET") != 0) {
        return 501;
    }

    /* Must be HTTP/1.x basically */
    if (strstr(req->version, "HTTP/") == NULL) {
        return 400;
    }

    /* Strip leading slash for local usage (like "./some
     * file") */
    if (req->path[0] == '/') {
        memmove(req->path, req->path+1, strlen(req->path));
    }

    struct stat s;
    if (stat(req->path, &s) != 0) {
        return 404;
    }

    /* Directory logic: if missing slash, return 302.  If slash is present, maybe check index.html. */
    if (S_ISDIR(s.st_mode)) {
        size_t plen = strlen(req->path);
        if (plen == 0) {
            /* It's basically the root "./" => just handle it as directory */
            return 200;
        }
        if (req->path[plen - 1] != '/') {
            return 302; /* Must end with '/' => redirect */
        }
        /* If the directory has index.html, use that. */
        if (find_index_html(req->path)) {
            strcat(req->path, "index.html");
        }
        return 200;
    }

    /* File logic: must be regular file + have world-read perms */
    if (!S_ISREG(s.st_mode) || !(s.st_mode & S_IROTH)) {
        return 403;
    }

    /* If file in subdirectory, check +x perms on the directories */
    if (strchr(req->path, '/') != NULL) {
        return has_execute_permissions(req->path);
    }

    return 200;
}

/* If path ends with '/', treat as directory.  Build listing or read index.html if it exists. */
int construct_OK_body(char *path, unsigned char **body, int *size) {
    if (is_directory(path)) {
        /* Build directory listing if it's truly a directory. */
        int dummy = 0;
        if (generate_directory_listing(path, (char **)body, &dummy, size) != 0) {
            return -1;
        }
        return 0;
    }
    /* Read file into memory */
    return open_read_file(body, path, size);
}

/* Returns 1 if "dirname/index.html" exists, else 0 */
int find_index_html(const char *dir) {
    char tmp[FIRST_LINE_SIZE];
    snprintf(tmp, sizeof(tmp), "%sindex.html", dir);
    struct stat s;
    if (stat(tmp, &s) == 0 && S_ISREG(s.st_mode)) {
        return 1;
    }
    return 0;
}

/* Checks +x perms on each directory in path. 403 if any directory lacks x bits */
int has_execute_permissions(const char *full_path) {
    char copy[FIRST_LINE_SIZE];
    strncpy(copy, full_path, sizeof(copy));
    char *d = dirname(copy);

    struct stat ds;
    while (strcmp(d, ".") != 0 && strcmp(d, "/") != 0) {
        if (stat(d, &ds) != 0) {
            return 403;
        }
        if (!(ds.st_mode & S_IXOTH)) {
            return 403;
        }
        d = dirname(d);
    }
    return 200;
}

/* Reads file fully into memory.  Caller frees. */
int open_read_file(unsigned char **body, char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    *body = malloc(sz+1);
    if (!*body) {
        fclose(f);
        return -1;
    }
    fread(*body, 1, sz, f);
    (*body)[sz] = '\0'; /* Ensure it's null-terminated if text */
    fclose(f);

    *size = (int)sz;
    DEBUG_PRINT("open_read_file : body = %d \n", *size);
    return 0;
}

/* Builds an HTML listing of a directory with last-mod times and sizes. */
int generate_directory_listing(const char *dir, char **html_body, int *is_alloc, int *body_size) {
    DIR *dp = opendir(dir);
    if (!dp) return -1;

    *html_body = malloc(INITIAL_BUFFER_SIZE);
    if (!*html_body) {
        closedir(dp);
        return -1;
    }
    *is_alloc = 1;

    size_t cap = INITIAL_BUFFER_SIZE;
    size_t used = 0;
    used += snprintf(*html_body + used, cap - used,
                     "<HTML><HEAD><TITLE>Index of %s</TITLE></HEAD><BODY>\n"
                     "<H4>Index of %s</H4>\n"
                     "<table CELLSPACING=8>"
                     "<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n",
                     dir, dir);

    struct dirent *de;
    char path_buf[1024];
    while ((de = readdir(dp)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir, de->d_name);

        struct stat st;
        if (stat(path_buf, &st) != 0) continue;

        /* Last-mod time in RFC1123 */
        char time_buf[128];
        strftime(time_buf, sizeof(time_buf), RFC1123FMT, gmtime(&st.st_mtime));

        char sz_buf[64] = "";
        char name_buf[1024];
        snprintf(name_buf, sizeof(name_buf), "%s", de->d_name);

        if (S_ISREG(st.st_mode)) {
            snprintf(sz_buf, sizeof(sz_buf), "%ld", (long)st.st_size);
        } else if (S_ISDIR(st.st_mode)) {
            /* Add a '/' to directory names */
            strncat(name_buf, "/", sizeof(name_buf) - strlen(name_buf) - 1);
        }

        /* Expand buffer if needed */
        size_t row_len = snprintf(NULL, 0,
            "<tr><td><A HREF=\"%s\">%s</A></td><td>%s</td><td>%s</td></tr>\n",
            name_buf, name_buf, time_buf, sz_buf);

        if (used + row_len + 2 > cap) {
            cap *= 2;
            char *grow = realloc(*html_body, cap);
            if (!grow) {
                free(*html_body);
                closedir(dp);
                return -1;
            }
            *html_body = grow;
        }

        used += snprintf(*html_body + used, cap - used,
                         "<tr><td><A HREF=\"%s\">%s</A></td><td>%s</td><td>%s</td></tr>\n",
                         name_buf, name_buf, time_buf, sz_buf);
    }
    closedir(dp);

    /* Footer */
    if (used + 128 > cap) {
        cap += 128;
        char *grow = realloc(*html_body, cap);
        if (!grow) {
            free(*html_body);
            return -1;
        }
        *html_body = grow;
    }
    used += snprintf(*html_body + used, cap - used,
                     "</table>\n<HR>\n<ADDRESS>webserver/1.0</ADDRESS>\n</BODY></HTML>\n");

    *body_size = (int)used;
    return 0;
}

/* Checks if path ends with '/' so we treat it as a directory path. */
int is_directory(const char *p) {
    int len = strlen(p);
    return (len > 0 && p[len-1] == '/');
}

/* Gets RFC1123 last-mod date for a file. Returns static string or NULL on error. */
char *get_last_modified_date(const char *path) {
    static char buf[128];
    struct stat s;
    if (stat(path, &s) != 0) return NULL;

    struct tm tmv;
    gmtime_r(&s.st_mtime, &tmv);
    strftime(buf, sizeof(buf), RFC1123FMT, &tmv);
    return buf;
}

/* Quick check for known MIME types */
char *get_mime_type(char *filename) {
    char *dot = strrchr(filename, '.');
    if (!dot) return NULL;
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html";
    if (!strcasecmp(dot, ".jpg")  || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".gif"))  return "image/gif";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".au"))   return "audio/basic";
    if (!strcasecmp(dot, ".wav"))  return "audio/wav";
    if (!strcasecmp(dot, ".avi"))  return "video/x-msvideo";
    if (!strcasecmp(dot, ".mpeg") || !strcasecmp(dot, ".mpg")) return "video/mpeg";
    if (!strcasecmp(dot, ".mp3"))  return "audio/mpeg";
    return NULL;
}

/* Current date in RFC1123 format */
void get_current_date(char *buf, size_t len) {
    time_t now = time(NULL);
    strftime(buf, len, RFC1123FMT, gmtime(&now));
}

/* Writes all data in 'buf' to 'fd' until done or error */
void write_to_client(int fd, unsigned char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int w = write(fd, buf + sent, len - sent);
        if (w < 0) {
            perror("write_to_client");
            return;
        }
        sent += w;
    }
}

/* Builds headers + body. If not 200, we embed a small HTML body describing the error/redirect. */
unsigned char *build_http_response(int code, char *path, const unsigned char *body, long b_len, int *resp_len) {
    static const char *err_tmpl =
      "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\r\n"
      "<BODY><H4>%d %s</H4>\r\n%s\r\n</BODY></HTML>";

    const char *text = "OK";
    const char *msg  = "OK";
    switch (code) {
        case 200: text = "OK";                   msg = "OK"; break;
        case 302: text = "Found";                msg = "Directories must end with a slash."; break;
        case 400: text = "Bad Request";          msg = "Bad Request."; break;
        case 403: text = "Forbidden";            msg = "Access denied."; break;
        case 404: text = "Not Found";            msg = "File not found."; break;
        case 500: text = "Internal Server Error";msg = "Some server side error."; break;
        case 501: text = "Not supported";        msg = "Method is not supported."; break;
        default:  text = "Unknown";              msg = "Unknown problem."; break;
    }

    /* If non-200, we build an HTML error body. */
    char fallback[512];
    memset(fallback, 0, sizeof(fallback));
    long actual_body_len = 0;
    if (code != 200) {
        snprintf(fallback, sizeof(fallback), err_tmpl, code, text, code, text, msg);
        actual_body_len = (long)strlen(fallback);
    } else {
        actual_body_len = b_len;  // Use provided body length for 200 responses
    }
    DEBUG_PRINT("build_http_response : actual_body_len = %li \n", actual_body_len);
    /* Make a small buffer for headers. */
    char head[512];
    char date[128];
    get_current_date(date, sizeof(date));

    int used = snprintf(head, sizeof(head),
        "HTTP/1.0 %d %s\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n",
        code, text, date);

    if (code == 302) {
        if (!path)
            path = "";
        used += snprintf(head + used, sizeof(head) - used,
                         "Location: %s/\r\n", path);
    }

    /* Set Content-Type */
    const char *mtype = NULL;
    if (code == 200 && path) {
        mtype = is_directory(path) ? "text/html" : get_mime_type(path);
    } else if (code != 200) {
        mtype = "text/html";  // Error responses are always HTML
    }

    if (mtype) {
        used += snprintf(head + used, sizeof(head) - used,
                         "Content-Type: %s\r\n", mtype);
    }

    /* Add Content-Length */
    used += snprintf(head + used, sizeof(head) - used,
                     "Content-Length: %ld\r\n", actual_body_len);

    if (code == 200 && path) {
        const char *lmod = get_last_modified_date(path);
        if (lmod) {
            used += snprintf(head + used, sizeof(head) - used,
                             "Last-Modified: %s\r\n", lmod);
        }
    }

    used += snprintf(head + used, sizeof(head) - used,
                     "Connection: close\r\n\r\n");

    /* Allocate memory for the complete response: headers + body */
    unsigned char *resp = malloc(used + actual_body_len);
    if (!resp) {
        *resp_len = 0;
        return NULL;
    }

    /* Copy headers and body into the response buffer */
    memcpy(resp, head, used);
    if (code == 200) {
        memcpy(resp + used, body, actual_body_len);
    } else {
        memcpy(resp + used, fallback, actual_body_len);
    }

    *resp_len = used + actual_body_len; // Full response size
    return resp;
}
