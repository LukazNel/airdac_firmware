/*
 * RTSP protocol handler. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "esp_rom_md5.h"

#include "common.h"
#include "player.h"
#include "rtp.h"
#include "shairport_mdns.h"

#include "esp_log.h"

static const char* TAG = "RTSP";

#ifdef AF_INET6
#define INETx_ADDRSTRLEN INET6_ADDRSTRLEN
#else
#define INETx_ADDRSTRLEN INET_ADDRSTRLEN
#endif

static int please_shutdown = 0;
TaskHandle_t* listen_thread;

typedef struct {
    int fd;
    stream_cfg stream;
    SOCKADDR remote;
    int running;
    TaskHandle_t thread;
} rtsp_conn_info;

// keep track of the threads we have spawned so we can join() them
static rtsp_conn_info **conns = NULL;
static int nconns = 0;
static void track_thread(rtsp_conn_info *conn) {
    conns = realloc(conns, sizeof(rtsp_conn_info*) * (nconns + 1));
    conns[nconns] = conn;
    nconns++;
}

static void cleanup_threads(void) {
    int i;
    ESP_LOGD(TAG, "culling threads.\n");
    for (i=0; i<nconns; ) {
        if (conns[i]->thread == NULL) { //conns[i]->running == 0) {
            //vTaskDelete(conns[i]->thread);
            free(conns[i]);
            ESP_LOGD(TAG, "one joined\n");
            nconns--;
            if (nconns)
                conns[i] = conns[nconns];
        } else {
            i++;
        }
    }
}

// park a null at the line ending, and return the next line pointer
// accept \r, \n, or \r\n
static char *nextline(char *in, int inbuf) {
    char *out = NULL;
    while (inbuf) {
        if (*in == '\r') {
            *in++ = 0;
            out = in;
        }
        if (*in == '\n') {
            *in++ = 0;
            out = in;
        }

        if (out)
            break;

        in++;
        inbuf--;
    }
    return out;
}

typedef struct {
    int nheaders;
    char *name[16];
    char *value[16];

    int contentlength;
    char *content;

    // for requests
    char method[16];

    // for responses
    int respcode;
} rtsp_message;

static rtsp_message * msg_init(void) {
    rtsp_message *msg = malloc(sizeof(rtsp_message));
    memset(msg, 0, sizeof(rtsp_message));
    return msg;
}

static int msg_add_header(rtsp_message *msg, char *name, char *value) {
    if (msg->nheaders >= sizeof(msg->name)/sizeof(char*)) {
        ESP_LOGW(TAG, "too many headers?!");
        return 1;
    }

    msg->name[msg->nheaders] = strdup(name);
    msg->value[msg->nheaders] = strdup(value);
    msg->nheaders++;

    return 0;
}

static char *msg_get_header(rtsp_message *msg, char *name) {
    int i;
    for (i=0; i<msg->nheaders; i++)
        if (!strcasecmp(msg->name[i], name))
            return msg->value[i];
    return NULL;
}

static void msg_free(rtsp_message *msg) {
    int i;
    for (i=0; i<msg->nheaders; i++) {
        free(msg->name[i]);
        free(msg->value[i]);
    }
    if (msg->content)
        free(msg->content);
    free(msg);
}


static int msg_handle_line(rtsp_message **pmsg, char *line) {
    rtsp_message *msg = *pmsg;

    if (!msg) {
        msg = msg_init();
        *pmsg = msg;
        char *sp, *p;

        ESP_LOGV(TAG, "received request: %s\n", line);

        p = strtok_r(line, " ", &sp);
        if (!p)
            goto fail;
        strncpy(msg->method, p, sizeof(msg->method)-1);

        p = strtok_r(NULL, " ", &sp);
        if (!p)
            goto fail;

        p = strtok_r(NULL, " ", &sp);
        if (!p)
            goto fail;
        if (strcmp(p, "RTSP/1.0"))
            goto fail;

        return -1;
    }

    if (strlen(line)) {
        char *p;
        p = strstr(line, ": ");
        if (!p) {
            ESP_LOGW(TAG, "bad header: >>%s<<", line);
            goto fail;
        }
        *p = 0;
        p += 2;
        msg_add_header(msg, line, p);
        ESP_LOGV(TAG, "    %s: %s\n", line, p);
        return -1;
    } else {
        char *cl = msg_get_header(msg, "Content-Length");
        if (cl)
            return atoi(cl);
        else
            return 0;
    }

fail:
    *pmsg = NULL;
    msg_free(msg);
    return 0;
}

static rtsp_message * rtsp_read_request(int fd) {
    ssize_t buflen = 512;
    char *buf = malloc(buflen+1);

    rtsp_message *msg = NULL;

    ssize_t nread;
    ssize_t inbuf = 0;
    int msg_size = -1;

    while (msg_size < 0) {
        if (ulTaskNotifyTake(pdTRUE, 0)) {
            ESP_LOGD(TAG, "RTSP shutdown requested\n");
            goto shutdown;
        }
        nread = read(fd, buf+inbuf, buflen - inbuf);
        if (!nread) {
            ESP_LOGD(TAG, "RTSP connection closed\n");
            goto shutdown;
        }
        if (nread < 0) {
            if (errno==EINTR)
                continue;
            perror("read failure");
            goto shutdown;
        }
        inbuf += nread;

        char *next;
        while (msg_size < 0 && (next = nextline(buf, inbuf))) {
            msg_size = msg_handle_line(&msg, buf);

            if (!msg) {
                ESP_LOGW(TAG, "no RTSP header received");
                goto shutdown;
            }

            inbuf -= next-buf;
            if (inbuf)
                memmove(buf, next, inbuf);
        }
    }

    if (msg_size > buflen) {
        buf = realloc(buf, msg_size);
        if (!buf) {
            ESP_LOGW(TAG, "too much content");
            goto shutdown;
        }
        buflen = msg_size;
    }

    while (inbuf < msg_size) {
        nread = read(fd, buf+inbuf, msg_size-inbuf);
        if (!nread)
            goto shutdown;
        if (nread==EINTR)
            continue;
        if (nread < 0) {
            perror("read failure");
            goto shutdown;
        }
        inbuf += nread;
    }

    msg->contentlength = inbuf;
    msg->content = buf;
    return msg;

shutdown:
    free(buf);
    if (msg) {
        msg_free(msg);
    }
    return NULL;
}

static void msg_write_response(int fd, rtsp_message *resp) {
    char pkt[1024];
    int pktfree = sizeof(pkt);
    char *p = pkt;
    int i, n;

    n = snprintf(p, pktfree,
                 "RTSP/1.0 %d %s\r\n", resp->respcode,
                 resp->respcode==200 ? "OK" : "Error");
    ESP_LOGV(TAG, "sending response: %s", pkt);
    pktfree -= n;
    p += n;

    for (i=0; i<resp->nheaders; i++) {
        ESP_LOGV(TAG, "    %s: %s\n", resp->name[i], resp->value[i]);
        n = snprintf(p, pktfree, "%s: %s\r\n", resp->name[i], resp->value[i]);
        pktfree -= n;
        p += n;
        if (pktfree <= 0)
            ESP_LOGE(TAG, "Attempted to write overlong RTSP packet");
    }

    if (pktfree < 3)
        ESP_LOGE(TAG, "Attempted to write overlong RTSP packet");

    strcpy(p, "\r\n");
    write_unchecked(fd, pkt, p-pkt+2);
}

static void handle_options(rtsp_conn_info *conn,
                           rtsp_message *req, rtsp_message *resp) {
    resp->respcode = 200;
    msg_add_header(resp, "Public",
                   "ANNOUNCE, SETUP, RECORD, "
                   "PAUSE, FLUSH, TEARDOWN, "
                   "OPTIONS, GET_PARAMETER, SET_PARAMETER");
}

static void handle_teardown(rtsp_conn_info *conn,
                            rtsp_message *req, rtsp_message *resp) {
    resp->respcode = 200;
    msg_add_header(resp, "Connection", "close");
    please_shutdown = 1;
}

static void handle_flush(rtsp_conn_info *conn,
                         rtsp_message *req, rtsp_message *resp) {
    player_flush();
    resp->respcode = 200;
}

static void handle_setup(rtsp_conn_info *conn,
                         rtsp_message *req, rtsp_message *resp) {

    int cport, tport;
    char *hdr = msg_get_header(req, "Transport");
    if (!hdr)
        return;

    char *p;
    p = strstr(hdr, "control_port=");
    if (!p)
        return;
    p = strchr(p, '=') + 1;
    cport = atoi(p);

    p = strstr(hdr, "timing_port=");
    if (!p)
        return;
    p = strchr(p, '=') + 1;
    tport = atoi(p);

    player_play(&conn->stream);
    int sport = rtp_setup(&conn->remote, cport, tport);
    if (!sport)
        return;

    char resphdr[100];
    snprintf(resphdr, sizeof(resphdr),
             "RTP/AVP/UDP;unicast;mode=record;server_port=%d;control_port=%d;timing_port=%d",
             sport, sport, sport);
    msg_add_header(resp, "Transport", resphdr);

    msg_add_header(resp, "Session", "1");

    resp->respcode = 200;
}

static void handle_ignore(rtsp_conn_info *conn,
                          rtsp_message *req, rtsp_message *resp) {
    resp->respcode = 200;
}

static void handle_set_parameter_parameter(rtsp_conn_info *conn,
                                           rtsp_message *req, rtsp_message *resp) {
    char *cp = req->content;
    int cp_left = req->contentlength;
    char *next;
    while (cp_left && cp) {
        next = nextline(cp, cp_left);
        cp_left -= next-cp;

        if (!strncmp(cp, "volume: ", 8)) {
            float volume = atof(cp + 8);
            ESP_LOGV(TAG, "volume: %f\n", volume);
            player_volume(volume);
        } else if(!strncmp(cp, "progress: ", 10)) {
            char *progress = cp + 10;
            ESP_LOGV(TAG, "progress: %s\n", progress);
        } else {
            ESP_LOGV(TAG, "unrecognised parameter: >>%s<< (%d)\n", cp, strlen(cp));
        }
        cp = next;
    }
}

static void handle_set_parameter(rtsp_conn_info *conn,
                                 rtsp_message *req, rtsp_message *resp) {
    if (!req->contentlength)
        ESP_LOGW(TAG, "received empty SET_PARAMETER request\n");

    char *ct = msg_get_header(req, "Content-Type");

    if (ct) {
        ESP_LOGV(TAG, "SET_PARAMETER Content-Type: %s\n", ct);

         if (!strncmp(ct, "text/parameters", 15)) {
            ESP_LOGV(TAG, "received parameters in SET_PARAMETER request\n");

            handle_set_parameter_parameter(conn, req, resp);
        } else {
            ESP_LOGV(TAG, "received unknown Content-Type %s in SET_PARAMETER request\n", ct);
        }
    } else {
        ESP_LOGV(TAG, "missing Content-Type header in SET_PARAMETER request\n");
    }

    resp->respcode = 200;
}

static void handle_announce(rtsp_conn_info *conn,
                            rtsp_message *req, rtsp_message *resp) {

    char *paesiv = NULL;
    char *prsaaeskey = NULL;
    char *pfmtp = NULL;
    char *cp = req->content;
    int cp_left = req->contentlength;
    char *next;
    while (cp_left && cp) {
        next = nextline(cp, cp_left);
        cp_left -= next-cp;

        if (!strncmp(cp, "a=fmtp:", 7))
            pfmtp = cp+7;

        if (!strncmp(cp, "a=aesiv:", 8))
            paesiv = cp+8;

        if (!strncmp(cp, "a=rsaaeskey:", 12))
            prsaaeskey = cp+12;

        cp = next;
    }

    if (!paesiv || !prsaaeskey || !pfmtp) {
        ESP_LOGW(TAG, "required params missing from announce");
        return;
    }

    int len, keylen;
    uint8_t *aesiv = base64_dec(paesiv, &len);
    if (len != 16) {
        ESP_LOGW(TAG, "client announced aeskey of %d bytes, wanted 16", len);
        free(aesiv);
        return;
    }
    memcpy(conn->stream.aesiv, aesiv, 16);
    free(aesiv);

    uint8_t *rsaaeskey = base64_dec(prsaaeskey, &len);
    uint8_t *aeskey = rsa_apply(rsaaeskey, len, &keylen, RSA_MODE_KEY);
    free(rsaaeskey);
    if (keylen != 16) {
        ESP_LOGW(TAG, "client announced rsaaeskey of %d bytes, wanted 16", keylen);
        free(aeskey);
        return;
    }
    memcpy(conn->stream.aeskey, aeskey, 16);
    free(aeskey);

    int i;
    for (i=0; i<sizeof(conn->stream.fmtp)/sizeof(conn->stream.fmtp[0]); i++)
        conn->stream.fmtp[i] = atoi(strsep(&pfmtp, " \t"));

    resp->respcode = 200;
}


static struct method_handler {
    char *method;
    void (*handler)(rtsp_conn_info *conn, rtsp_message *req,
                    rtsp_message *resp);
} method_handlers[] = {
    {"OPTIONS",         handle_options},
    {"ANNOUNCE",        handle_announce},
    {"FLUSH",           handle_flush},
    {"TEARDOWN",        handle_teardown},
    {"SETUP",           handle_setup},
    {"GET_PARAMETER",   handle_ignore},
    {"SET_PARAMETER",   handle_set_parameter},
    {"RECORD",          handle_ignore},
    {NULL,              NULL}
};

static void apple_challenge(int fd, rtsp_message *req, rtsp_message *resp) {
    char *hdr = msg_get_header(req, "Apple-Challenge");
    if (!hdr)
        return;

    SOCKADDR fdsa;
    socklen_t sa_len = sizeof(fdsa);
    getsockname(fd, (struct sockaddr*)&fdsa, &sa_len);

    int chall_len;
    uint8_t *chall = base64_dec(hdr, &chall_len);
    uint8_t buf[48], *bp = buf;
    int i;
    memset(buf, 0, sizeof(buf));

    if (chall_len > 16) {
        ESP_LOGW(TAG, "oversized Apple-Challenge!");
        free(chall);
        return;
    }
    memcpy(bp, chall, chall_len);
    free(chall);
    bp += chall_len;

#ifdef AF_INET6
    if (fdsa.SAFAMILY == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)(&fdsa);
        memcpy(bp, sa6->sin6_addr.s6_addr, 16);
        bp += 16;
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)(&fdsa);
        memcpy(bp, &sa->sin_addr.s_addr, 4);
        bp += 4;
    }

    for (i=0; i<6; i++)
        *bp++ = config.hw_addr[i];

    int buflen, resplen;
    buflen = bp-buf;
    if (buflen < 0x20)
        buflen = 0x20;

    uint8_t *challresp = rsa_apply(buf, buflen, &resplen, RSA_MODE_AUTH);
    char *encoded = base64_enc(challresp, resplen);

    // strip the padding.
    char *padding = strchr(encoded, '=');
    if (padding)
        *padding = 0;

    msg_add_header(resp, "Apple-Response", encoded);
    free(challresp);
    free(encoded);
}

static char *make_nonce(void) {
    uint8_t random[8];
    esp_fill_random(random, 8);
    return base64_enc(random, 8);
}

static int rtsp_auth(char **nonce, rtsp_message *req, rtsp_message *resp) {

    return 0;
//    if (!*nonce) {
//        *nonce = make_nonce();
//        goto authenticate;
//    }
//
//    char *hdr = msg_get_header(req, "Authorization");
//    if (!hdr || strncmp(hdr, "Digest ", 7))
//        goto authenticate;
//
//    char *realm = strstr(hdr, "realm=\"");
//    char *username = strstr(hdr, "username=\"");
//    char *response = strstr(hdr, "response=\"");
//    char *uri = strstr(hdr, "uri=\"");
//
//    if (!realm || !username || !response || !uri)
//        goto authenticate;
//
//    char *quote;
//    realm = strchr(realm, '"') + 1;
//    if (!(quote = strchr(realm, '"')))
//        goto authenticate;
//    *quote = 0;
//    username = strchr(username, '"') + 1;
//    if (!(quote = strchr(username, '"')))
//        goto authenticate;
//    *quote = 0;
//    response = strchr(response, '"') + 1;
//    if (!(quote = strchr(response, '"')))
//        goto authenticate;
//    *quote = 0;
//    uri = strchr(uri, '"') + 1;
//    if (!(quote = strchr(uri, '"')))
//        goto authenticate;
//    *quote = 0;
//
//    uint8_t digest_urp[16], digest_mu[16], digest_total[16];
//    md5_context_t ctx;
//    esp_rom_md5_init(&ctx);
//    esp_rom_md5_update(&ctx, username, strlen(username));
//    esp_rom_md5_update(&ctx, ":", 1);
//    esp_rom_md5_update(&ctx, realm, strlen(realm));
//    esp_rom_md5_update(&ctx, ":", 1);
//    //esp_rom_md5_update(&ctx, config.password, strlen(config.password));
//    esp_rom_md5_final(digest_urp, &ctx);
//
//    esp_rom_md5_init(&ctx);
//    esp_rom_md5_update(&ctx, req->method, strlen(req->method));
//    esp_rom_md5_update(&ctx, ":", 1);
//    esp_rom_md5_update(&ctx, uri, strlen(uri));
//    esp_rom_md5_final(digest_mu, &ctx);
//
//    int i;
//    char buf[33];
//    for (i=0; i<16; i++)
//        sprintf(buf + 2*i, "%02x", digest_urp[i]);
//    esp_rom_md5_init(&ctx);
//    esp_rom_md5_update(&ctx, buf, 32);
//    esp_rom_md5_update(&ctx, ":", 1);
//    esp_rom_md5_update(&ctx, *nonce, strlen(*nonce));
//    esp_rom_md5_update(&ctx, ":", 1);
//    for (i=0; i<16; i++)
//        sprintf(buf + 2*i, "%02x", digest_mu[i]);
//    esp_rom_md5_update(&ctx, buf, 32);
//    esp_rom_md5_final(digest_total, &ctx);
//
//    for (i=0; i<16; i++)
//        sprintf(buf + 2*i, "%02x", digest_total[i]);
//
//    if (!strcmp(response, buf))
//        return 0;
//    ESP_LOGW(TAG, "auth failed");
//
//authenticate:
//    resp->respcode = 401;
//    int hdrlen = strlen(*nonce) + 40;
//    char *authhdr = malloc(hdrlen);
//    snprintf(authhdr, hdrlen, "Digest realm=\"taco\", nonce=\"%s\"", *nonce);
//    msg_add_header(resp, "WWW-Authenticate", authhdr);
//    free(authhdr);
//    return 1;
}

static void rtsp_conversation_thread_func(void *pconn) {
    ESP_LOGD(TAG, "RTSP thread started");
    rtsp_conn_info *conn = pconn;

    rtsp_message *req, *resp;
    char *hdr, *auth_nonce = NULL;
    while ((req = rtsp_read_request(conn->fd))) {
        resp = msg_init();
        resp->respcode = 400;

        apple_challenge(conn->fd, req, resp);
        hdr = msg_get_header(req, "CSeq");
        if (hdr)
            msg_add_header(resp, "CSeq", hdr);
        msg_add_header(resp, "Audio-Jack-Status", "connected; type=analog");

        if (rtsp_auth(&auth_nonce, req, resp))
            goto respond;

        struct method_handler *mh;
        for (mh=method_handlers; mh->method; mh++) {
            if (!strcmp(mh->method, req->method)) {
                mh->handler(conn, req, resp);
                break;
            }
        }


respond:
        msg_write_response(conn->fd, resp);
        msg_free(req);
        msg_free(resp);
    }

    ESP_LOGD(TAG, "closing RTSP connection\n");
    if (conn->fd > 0) {
        close(conn->fd);
    }
    rtp_shutdown();
    player_stop();
    if (auth_nonce)
        free(auth_nonce);
    ESP_LOGD(TAG, "terminating RTSP thread\n");
    conn->running = 0;
    conn->thread = NULL;
    vTaskDelete(NULL);
}

// this function is not thread safe.
static const char* format_address(struct sockaddr *fsa) {
    static char string[INETx_ADDRSTRLEN];
    void *addr;
#ifdef AF_INET6
    if (fsa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)(fsa);
        addr = &(sa6->sin6_addr);
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)(fsa);
        addr = &(sa->sin_addr);
    }
    return inet_ntop(fsa->sa_family, addr, string, sizeof(string));
}

void rtsp_listen_loop(void) {
    struct addrinfo hints, *info, *p;
    char portstr[6];
    int *sockfd = NULL;
    int nsock = 0;
    int i, ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    snprintf(portstr, 6, "%d", config.port);

    ret = getaddrinfo(NULL, portstr, &hints, &info);
    if (ret) {
        ESP_LOGE(TAG, "getaddrinfo failed: %d", ret);
    }

    for (p=info; p; p=p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, IPPROTO_TCP);
        int yes = 1;

        ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

#ifdef IPV6_V6ONLY
        // some systems don't support v4 access on v6 sockets, but some do.
        // since we need to account for two sockets we might as well
        // always.
        if (p->ai_family == AF_INET6)
            ret |= setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif

        if (!ret)
            ret = bind(fd, p->ai_addr, p->ai_addrlen);

        // one of the address families will fail on some systems that
        // report its availability. do not complain.
        if (ret) {
            ESP_LOGD(TAG, "Failed to bind to address %s\n", format_address(p->ai_addr));
            continue;
        }
        ESP_LOGD(TAG, "Bound to address %s\n", format_address(p->ai_addr));

        listen(fd, 5);
        nsock++;
        sockfd = realloc(sockfd, nsock*sizeof(int));
        sockfd[nsock-1] = fd;
    }

    freeaddrinfo(info);

    if (!nsock)
        ESP_LOGE(TAG, "could not bind any listen sockets!");


    int maxfd = -1;
    fd_set fds;
    FD_ZERO(&fds);
    for (i=0; i<nsock; i++) {
        if (sockfd[i] > maxfd)
            maxfd = sockfd[i];
    }

    mdns_register();

    printf("Listening for connections.\n");
    ESP_LOGD(TAG, "Testing");

    int acceptfd;
    struct timeval tv;
    while (1) {

        tv.tv_sec = 300;
        tv.tv_usec = 0;

        for (i=0; i<nsock; i++)
            FD_SET(sockfd[i], &fds);

        ret = select(maxfd+1, &fds, 0, 0, &tv);
        if (ret<0) {
            if (errno==EINTR)
                continue;
            break;
        }

        ESP_LOGI(TAG, "Going");
        cleanup_threads();

        acceptfd = -1;
        for (i=0; i<nsock; i++) {
            if (FD_ISSET(sockfd[i], &fds)) {
                acceptfd = sockfd[i];
                break;
            }
        }
        if (acceptfd < 0) // timeout
            continue;

        rtsp_conn_info *conn = malloc(sizeof(rtsp_conn_info));
        memset(conn, 0, sizeof(rtsp_conn_info));
        socklen_t slen = sizeof(conn->remote);

        ESP_LOGD(TAG, "new RTSP connection\n");
        conn->fd = accept(acceptfd, (struct sockaddr *)&conn->remote, &slen);

        if (conn->fd < 0) {
            perror("failed to accept connection");
            free(conn);
        } else {
            xTaskCreate(rtsp_conversation_thread_func, "RTSP Conversation", 4096, (void*)conn, 2, &(conn->thread));
            listen_thread = &(conn->thread);
            conn->running = 1;
            track_thread(conn);
        }
    }
    perror("select");
    ESP_LOGE(TAG, "fell out of the RTSP select loop");
}
