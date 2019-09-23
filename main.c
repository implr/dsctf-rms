#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

unsigned RID = 0;

struct pending_req {
    unsigned rid;
    char *url;
    size_t url_len;
    pthread_t tid;
    struct pending_req *next;
};

struct done_req {
    unsigned rid;
    bool succ;
    char *response;
    size_t resp_len;
    struct done_req *next;
};

static inline int min(int a, int b) {
    return a > b ? b : a;
}

bool sendall(int fd, const void *buf, size_t len, int flags) {
    while(len > 0) {
        ssize_t r = send(fd, buf, len, flags);
        if(r < 0) {
            perror("send");
            return false;
        }
        len -= r;
    }
    return true;
}

bool make_request(struct sockaddr *ip, size_t addrlen, char *host, char *path, char **rbuf, size_t *rblen) {
    int sock = socket(ip->sa_family, SOCK_STREAM, 0);
    struct timeval tensec = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tensec, sizeof(tensec));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tensec, sizeof(tensec));
    if(sock < 0) {
        *rbuf = strerror(errno);
        return false;
    }
    if(connect(sock, ip, addrlen)) {
        *rbuf = strerror(errno);
        return false;
    }

    if(! (sendall(sock, "GET ", 4, MSG_MORE) && sendall(sock, path, strlen(path), MSG_MORE) && sendall(sock, " HTTP/1.1\r\n", 11, MSG_MORE)) ) {
        *rbuf = strerror(errno);
        return false;
    }
    if(! (sendall(sock, "Host: ", 6, MSG_MORE) && sendall(sock, host, strlen(host), MSG_MORE) && sendall(sock, "\r\n", 2, MSG_MORE))) {
        *rbuf = strerror(errno);
        return false;
    }
    const char endhdr[] = "Accept-Encoding: text/plain\r\nConnection: close\r\n\r\n";
    if(!sendall(sock, endhdr, sizeof(endhdr)-1, 0)) {
        *rbuf = strerror(errno);
        return false;
    }
    *rbuf = (char*) malloc(2048);
    size_t rbsz = 2048;
    *rblen = 0;
    while(1) {
        assert(*rbuf);
        ssize_t r = recv(sock, *rbuf + *rblen, rbsz - *rblen, 0);
        if(r < 0) {
            free(*rbuf);
            *rbuf = strerror(errno);
            return false;
        }
        *rblen += r;
        if(rbsz - *rblen < 1024) {
            if(rbsz + 1024 > 3* (1<<20)) {
                puts("memory limit exceeded");
                abort();
            }
            *rbuf = (char*) realloc(*rbuf, rbsz + 1024);
            rbsz += 1024;
        }
        if(r == 0)
            break;
    }
    (*rbuf)[*rblen] = '\0';
    return true;
}

void *fetch(void *rv){
    struct pending_req *r = (struct pending_req *) rv;
    struct done_req ret_s = {.rid = r->rid, .succ = true, .response = 0, .resp_len = 0, .next = 0};
    char *domain = 0;
    if(0 != strncmp("http://", r->url, min(r->url_len, 7))) {
        ret_s.response = "not http";
        goto fail;
    }
    char *host_str = r->url + 7;
    char *hostend = strchrnul(host_str, '/');
    if(hostend - host_str > 256) {
        ret_s.response = "host too long";
        goto fail;
    }
    char *colon = strchr(host_str, ':');
    int tport;
    if(colon >= hostend)
        colon = 0;
    if(!colon) {
        tport = 80;
        domain = strndup(host_str, hostend - host_str);
    } else {
        domain = strndup(host_str, colon - host_str);
        char *atstr = strndup(colon+1, hostend - (colon + 1));
        assert(atstr);
        tport = atoi(atstr);
        if(tport < 0 || tport > 0x10000) {
            puts("invalid port");
            abort();
        }
        free(atstr);
    }
    assert(domain);
    char *path;
    if(hostend[0] == '\0')
        path = "/";
    else
        path = hostend;
    assert(path[0] == '/');

    in_port_t port = htons(tport);
    struct hostent *hent6 = gethostbyname2(domain, AF_INET6), *hent4;
    struct sockaddr_storage ss = {0};
    struct sockaddr_in6 *ss6 = (struct sockaddr_in6*) &ss;
    struct sockaddr_in *ss4 = (struct sockaddr_in*) &ss;
    if(hent6) {
        assert(hent6->h_addrtype == AF_INET6);
        ss6->sin6_port = port;
        ss6->sin6_family = AF_INET6;
        memcpy(&ss6->sin6_addr, hent6->h_addr, sizeof(struct in6_addr));
        if( (0 == memcmp(&ss6->sin6_addr, &in6addr_loopback, sizeof(struct in6_addr))) || ss6->sin6_addr.s6_addr[0] == 0 ) {
            ret_s.response = "localhost not allowed";
            goto fail;
        }
    }
    hent4 = gethostbyname2(domain, AF_INET);
    if(hent4) {
        assert(hent4->h_addrtype == AF_INET);
        if(hent4->h_addr[0] == 127 || hent4->h_addr[0] == 0) {
            ret_s.response = "localhost not allowed";
            goto fail;
        }
    }
    if(hent6 && make_request((struct sockaddr*) &ss, sizeof(ss), domain, path, &ret_s.response, &ret_s.resp_len))
        goto out;
    if(hent4) {
        ss4->sin_family = AF_INET;
        ss4->sin_port = port;
        memcpy(&ss4->sin_addr, hent4->h_addr, sizeof(struct in_addr));
    } else {
        ret_s.response = (char*) hstrerror(h_errno);
        goto fail;
    }

    if(make_request((struct sockaddr*) &ss, sizeof(ss), domain, path, &ret_s.response, &ret_s.resp_len))
        goto out;

    struct done_req *ret;
fail:
    ret_s.succ = false;
    ret_s.resp_len = strlen(ret_s.response);
out:
    free(domain);
    ret = (struct done_req*) calloc(1, sizeof(struct done_req));
    memcpy(ret, &ret_s, sizeof(struct done_req));
    return ret;
}

void addp(struct pending_req **ph) {
    struct pending_req *p;
    if(! *ph) {
        p = (struct pending_req*) calloc(1, sizeof(struct pending_req));
        *ph = p;
    } else {
        p = *ph;
        while(p->next != 0)
            p = p->next;
        p->next = (struct pending_req*) calloc(1, sizeof(struct pending_req));
        p = p->next;
    }
    p->rid = RID++;
    printf("url? ");
    ssize_t r = getline(&p->url, &p->url_len, stdin);
    if(r < 0) {
        perror("getline");
        abort();
    } else if(r < 8) {
        puts("url too short");
        abort();
    }
    p->url[r-1] = '\0';
    if(pthread_create(&p->tid, 0, fetch, p)) {
        puts("error: pthread_create");
        abort();
    }
}

void dopoll(struct pending_req **pending, struct done_req **done) {
    if(! *pending)
        return;
    struct done_req **dptr;
    struct done_req **exend = done;
    if(*done) {
        while(*exend != 0)
            exend = &((*exend)->next);
        dptr = exend;
    } else {
        dptr = done;
    }
    struct pending_req *tmp, **pit = pending;
    while(*pit != 0) {
        void *tret;
        int r = pthread_tryjoin_np((*pit)->tid, &tret);
        switch(r) {
            case 0:
                if(tret == PTHREAD_CANCELED) {
                    puts("pthread cancel unexpected");
                    abort();
                }
                *dptr = (struct done_req*) tret;
                dptr = &((*dptr)->next);
                tmp = *pit;
                *pit = (*pit)->next;
                free(tmp);
                break;
            case EBUSY:
            case ETIMEDOUT:
                pit = &((*pit)->next);
                break;
            default:
                puts("unexpected pthread_tryjoin_np err");
                abort();
        }
    }
}

int main(int argc, char **argv) {
    if(setvbuf(stdin, 0, _IONBF, 0)) {
        perror("setvbuf");
        abort();
    }
    if(setvbuf(stdout, 0, _IONBF, 0)) {
        perror("setvbuf");
        abort();
    }
    struct pending_req *pending = 0;
    struct done_req *done = 0;
    while(1) {
        puts("What do?");
        puts("\tlist [p]ending requests");
        puts("\tlist [f]inished requests");
        puts("\t[v]iew result of request");
        puts("\t[a]dd new request");
        puts("\t[q]uit");
        printf("Choice? [pfvaq] ");
        char c, *estatus;
        unsigned t_rid;
        struct pending_req *p_iter = 0;
        struct done_req *d_iter = 0;
        dopoll(&pending, &done);
        int scanret = scanf(" %c", &c);
        if(scanret == EOF) {
            perror("scanf");
            break;
        } else if(scanret != 1) {
            continue;
        }

        switch(c) {
            case 'p':
                p_iter = pending;
                puts("Pending:");
                while(p_iter != 0) {
                    printf("\t[%u] %s\n", p_iter->rid, p_iter->url);
                    p_iter = p_iter->next;
                }
                break;
            case 'f':
                d_iter = done;
                puts("Done:");
                while(d_iter != 0) {
                    if(d_iter->succ) {
                        estatus = strchr(d_iter->response, '\r');
                        if(estatus == 0 || estatus - d_iter->response > 100 || estatus > d_iter->response + d_iter->resp_len)
                            printf("\t[%u] OK, status unknown\n", d_iter->rid);
                        else
                            printf("\t[%u] OK, %.*s\n", d_iter->rid, estatus - d_iter->response, d_iter->response);
                    } else {
                            printf("\t[%u] FAIL: %s\n", d_iter->rid, d_iter->response);
                    }
                    d_iter = d_iter->next;
                }
                break;
            case 'v':
                printf("request id? ");
                if(scanf("%u", &t_rid) != 1) {
                    puts("invalid value");
                    break;
                }
                d_iter = done;
                while(d_iter != 0) {
                    if(d_iter->rid == t_rid)
                        break;
                    d_iter = d_iter->next;
                }
                if(d_iter && d_iter->rid == t_rid) {
                    if(d_iter->succ) {
                        printf("[%u] Response, %lu bytes:\n", d_iter->rid, d_iter->resp_len);
                        fwrite(d_iter->response, d_iter->resp_len, 1, stdout);
                        puts("\n");
                    } else {
                        printf("[%u] failed, error: %.*s\n", d_iter->rid, d_iter->resp_len, d_iter->response);
                    }
                } else {
                    printf("Request %u not found.\n", t_rid);
                }
                break;
            case 'a':
                while(getc(stdin) != '\n') {}
                addp(&pending);
                break;
            case 'q':
                return 0;
            default:
                printf("invalid choice '%c'\n", c);
        }
    }
    return 0;
}
