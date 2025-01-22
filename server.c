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
#include <errno.h>
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

static const char *USAGE = "Usage: server <port> <pool-size> <max-queue-size> <max-number-of-request>\n";

typedef struct {
    char method[FIRST_LINE_SIZE];
    char path[FIRST_LINE_SIZE];
    char version[FIRST_LINE_SIZE];
} request_st;

/* ----- Forward Declarations ----- */
int handle_request(void *arg);
int parse_request(const char *req_line, request_st *req);
int generate_directory_listing(const char *dir, char **html_body, int *is_alloc, int *body_size);
int find_index_html(const char *dir);
int has_execute_permissions(const char *full_path);
int is_directory(const char *p);
char *get_last_modified_date(const char *path);
char *get_mime_type(char *filename);
void get_current_date(char *buf, size_t len);
unsigned char *build_http_response(int code, char *path, const unsigned char *body, long b_len, int *resp_len);
void write_to_client(int fd, unsigned char *buf, int len);

/* New streaming function for files */
int send_file_in_chunks(int client_fd, const char *filepath);

/* ----- Main ----- */
int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "%s", USAGE);
        exit(EXIT_FAILURE);
    }

    int port        = atoi(argv[1]);
    int poolSize    = atoi(argv[2]);
    int qMaxSize    = atoi(argv[3]);
    int maxRequests = atoi(argv[4]);

    /* Create the threadpool */
    threadpool *tp = create_threadpool(poolSize, qMaxSize);
    if (!tp) {
        fprintf(stderr, "Cannot create thread pool\n");
        exit(EXIT_FAILURE);
    }

    /* Create the server socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        destroy_threadpool(tp);
        exit(EXIT_FAILURE);
    }

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        destroy_threadpool(tp);
        exit(EXIT_FAILURE);
    }

    /* Listen */
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        destroy_threadpool(tp);
        exit(EXIT_FAILURE);
    }

    DEBUG_PRINT("Server listening on port %d\n", port);

    /* Accept connections until maxRequests reached */
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

/* ----- Thread function to handle requests ----- */
int handle_request(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);  /* Freed after pulling from the queue */

    DEBUG_PRINT("handle_request: start\n");

    /* 1) Read only the first line to parse "METHOD PATH VERSION" */
    char req_buffer[FIRST_LINE_SIZE + 1];
    memset(req_buffer, 0, sizeof(req_buffer));

    int total_read = 0;
    int found_line = 0;

    while (total_read < FIRST_LINE_SIZE) {
        ssize_t bytes = read(client_fd, req_buffer + total_read, 1);
        if (bytes < 1) { /* error or EOF */
            DEBUG_PRINT("handle_request: read error or EOF while reading first line\n");
            break;
        }
        total_read++;
        if (total_read >= 2 &&
            req_buffer[total_read - 2] == '\r' &&
            req_buffer[total_read - 1] == '\n') {
            /* Found "\r\n" -> isolate the line and stop */
            req_buffer[total_read - 2] = '\0';
            found_line = 1;
            break;
        }
    }

    /* 2) If no line found or we overflowed, return 400 */
    if (!found_line) {
        DEBUG_PRINT("handle_request: no valid request line -> 400\n");
        int tmpl;
        unsigned char *resp400 = build_http_response(400, NULL, NULL, 0, &tmpl);
        write_to_client(client_fd, resp400, tmpl);
        free(resp400);
        close(client_fd);
        return 0;
    }

    DEBUG_PRINT("Request line: %s\n", req_buffer);

    /* 3) Parse request */
    request_st req;
    memset(&req, 0, sizeof(req));
    int code = parse_request(req_buffer, &req);
    DEBUG_PRINT("parse_request returned code %d\n", code);

    /* 4) Handle results:
       - 200 => normal
       - 302 => redirect
       - else => error
    */
    if (code == 200) {
        /* Check if directory */
        if (is_directory(req.path)) {
            /* Build directory listing in memory, then build response */
            DEBUG_PRINT("handle_request: directory requested -> generating listing\n");
            unsigned char *dir_body = NULL;
            int dir_body_size = 0;
            int dummy = 0;
            if (generate_directory_listing(req.path, (char **)&dir_body, &dummy, &dir_body_size) != 0) {
                DEBUG_PRINT("handle_request: directory listing error -> 500\n");
                code = 500;
                /* Build + send error */
                int resp_len;
                unsigned char *resp = build_http_response(code, req.path, NULL, 0, &resp_len);
                write_to_client(client_fd, resp, resp_len);
                free(resp);
                close(client_fd);
                return 0;
            } else {
                /* Build + send 200 response with that directory listing */
                int resp_len;
                unsigned char *resp = build_http_response(200, req.path, dir_body, dir_body_size, &resp_len);
                write_to_client(client_fd, resp, resp_len);
                free(resp);
                free(dir_body);
                close(client_fd);
                return 0;
            }
        }
        else {
            /* It's a normal file => stream it */
            DEBUG_PRINT("handle_request: file requested -> streaming in chunks\n");
            if (send_file_in_chunks(client_fd, req.path) < 0) {
                DEBUG_PRINT("handle_request: error while streaming file -> close\n");
            }
            close(client_fd);
            return 0;
        }
    }
    else if (code == 302) {
        /* We have to redirect (missing slash for directory) */
        DEBUG_PRINT("handle_request: redirect -> 302\n");
        int resp_len = 0;
        unsigned char *resp = build_http_response(302, req.path, NULL, 0, &resp_len);
        write_to_client(client_fd, resp, resp_len);
        free(resp);
        close(client_fd);
        return 0;
    }
    else {
        /* Some error code */
        DEBUG_PRINT("handle_request: error code %d\n", code);
        int resp_len = 0;
        unsigned char *resp = build_http_response(code, req.path, NULL, 0, &resp_len);
        write_to_client(client_fd, resp, resp_len);
        free(resp);
        close(client_fd);
        return 0;
    }
}

/* ----- Parses "METHOD PATH VERSION" from the request line ----- */
int parse_request(const char *req_line, request_st *req) {
    if (sscanf(req_line, "%s %s %s", req->method, req->path, req->version) != 3) {
        return 400; // Bad request if not exactly 3 tokens
    }

    // Only GET is supported
    if (strcasecmp(req->method, "GET") != 0) {
        return 501;
    }

    // Must be HTTP/1.x basically
    if (strstr(req->version, "HTTP/") == NULL) {
        return 400;
    }

    // Strip leading slash from path for local usage
    if (req->path[0] == '/') {
        memmove(req->path, req->path + 1, strlen(req->path));
    }

    // If no path => stat current directory (".")
    if (strlen(req->path) == 0) {
        strcpy(req->path, ".");
    }

    struct stat s;
    if (stat(req->path, &s) != 0) {
        return 404;
    }

    if (S_ISDIR(s.st_mode)) {
        // <== FIX: If req->path == ".", user typed GET / => skip the 302
        if (strcmp(req->path, ".") == 0) {
            return has_execute_permissions(req->path);
        }

        // Else normal directory check: if no slash at end, do redirect
        size_t plen = strlen(req->path);
        if (plen > 0 && req->path[plen - 1] != '/') {
            return 302;
        }

        // If the directory has index.html, serve that
        if (find_index_html(req->path)) {
            strcat(req->path, "index.html");
            // Re-stat to confirm
            if (stat(req->path, &s) != 0) {
                return 404;
            }
            if (!S_ISREG(s.st_mode) || !(s.st_mode & S_IROTH)) {
                return 403;
            }
            return has_execute_permissions(req->path);
        }

        // Otherwise, just a directory listing
        return has_execute_permissions(req->path);
    }

    // File logic: must be reg file + have world read
    if (!S_ISREG(s.st_mode) || !(s.st_mode & S_IROTH)) {
        return 403;
    }

    // Check +x perms on the directories leading to it
    if (strchr(req->path, '/') != NULL) {
        return has_execute_permissions(req->path);
    }

    return 200;
}

/* ----- For a directory, build an HTML listing ----- */
int generate_directory_listing(const char *dir, char **html_body, int *is_alloc, int *body_size) {
    DIR *dp = opendir(dir);
    if (!dp) {
        DEBUG_PRINT("generate_directory_listing: cannot open dir %s\n", dir);
        return -1;
    }

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

/* ----- Helper to see if path ends with '/' so we treat it as directory ----- */
int is_directory(const char *p) {
    struct stat s;
    if (stat(p, &s) == 0 && S_ISDIR(s.st_mode)) {
        return 1;
    }
    return 0;
}


/* ----- Check if a directory has index.html (return 1 if yes, 0 if no) ----- */
int find_index_html(const char *dir) {
    char tmp[FIRST_LINE_SIZE];
    snprintf(tmp, sizeof(tmp), "%sindex.html", dir);
    struct stat s;
    if (stat(tmp, &s) == 0 && S_ISREG(s.st_mode)) {
        return 1;
    }
    return 0;
}

/* ----- Check +x perms on each directory in path. Return 403 if any directory lacks x bits ----- */
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

/* ----- Returns a static buffer with the last-mod date in RFC1123, or NULL on error ----- */
char *get_last_modified_date(const char *path) {
    static char buf[128];
    struct stat s;
    if (stat(path, &s) != 0) return NULL;

    struct tm tmv;
    gmtime_r(&s.st_mtime, &tmv);
    strftime(buf, sizeof(buf), RFC1123FMT, &tmv);
    return buf;
}

/* ----- Basic MIME type guesser ----- */
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

/* ----- Current date in RFC1123 format ----- */
void get_current_date(char *buf, size_t len) {
    time_t now = time(NULL);
    strftime(buf, len, RFC1123FMT, gmtime(&now));
}

/* ----- Write all data to client ----- */
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

/*
 * Builds full HTTP response for:
 * - Errors (non-200), with a small HTML body
 * - Directory listing (200) stored in memory
 */
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
    DEBUG_PRINT("build_http_response: actual_body_len = %li\n", actual_body_len);

    /* Make a small buffer for headers. */
    char head[512];
    char date[128];
    get_current_date(date, sizeof(date));

    int used = snprintf(head, sizeof(head),
        "HTTP/1.0 %d %s\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n",
        code, text, date);

    /* ----- The Fix: if code=302, handle path="." vs others ----- */
    if (code == 302) {
        if (!path) {
            path = "";
        }
        if (strcmp(path, ".") == 0) {
            /* Redirect to root slash */
            used += snprintf(head + used, sizeof(head) - used,
                             "Location: /\r\n");
        } else {
            /* e.g. path="subdir" -> Location: /subdir/ */
            used += snprintf(head + used, sizeof(head) - used,
                             "Location: /%s/\r\n", path);
        }
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

/*
 * New function: send a file in chunks using HTTP/1.0 style.
 * 1) We do a stat() to get file size + last modified.
 * 2) Write headers first (with Content-Length).
 * 3) Then read in chunks (e.g. 1000 bytes) and write each chunk immediately.
 * 4) Return 0 on success, -1 on any error.
 */
int send_file_in_chunks(int client_fd, const char *filepath) {
    DEBUG_PRINT("send_file_in_chunks: start with path=%s\n", filepath);

    /* 1) Stat the file */
    struct stat st;
    if (stat(filepath, &st) < 0) {
        perror("stat");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        DEBUG_PRINT("send_file_in_chunks: not a regular file\n");
        return -1;
    }
    long file_size = st.st_size;

    /* 2) Open the file */
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    /* 3) Build and send headers */
    char head[1024];
    char date[128];
    get_current_date(date, sizeof(date));

    char *lmod = get_last_modified_date(filepath);
    const char *mime = get_mime_type((char *)filepath);
    if (!mime) {
        mime = "application/octet-stream";  /* fallback */
    }

    int used = snprintf(head, sizeof(head),
                        "HTTP/1.0 200 OK\r\n"
                        "Server: webserver/1.0\r\n"
                        "Date: %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %ld\r\n",
                        date, mime, file_size);

    if (lmod) {
        used += snprintf(head + used, sizeof(head) - used,
                         "Last-Modified: %s\r\n", lmod);
    }

    used += snprintf(head + used, sizeof(head) - used,
                     "Connection: close\r\n\r\n");

    DEBUG_PRINT("send_file_in_chunks: sending headers (len=%d)\n", used);

    /* Send the headers */
    write_to_client(client_fd, (unsigned char *)head, used);

    /* 4) Read file in chunks and send each */
    unsigned char buffer[1000];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        DEBUG_PRINT("send_file_in_chunks: read %zu bytes, sending to client\n", n);
        int sent = 0;
        while (sent < (int)n) {
            int w = write(client_fd, buffer + sent, n - sent);
            if (w < 0) {
                perror("write");
                fclose(fp);
                return -1;
            }
            sent += w;
        }
    }

    /* 5) Check for read errors */
    if (ferror(fp)) {
        perror("fread");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    DEBUG_PRINT("send_file_in_chunks: done streaming file\n");
    return 0;
}
