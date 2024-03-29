#ifndef _COMMON_H
#define _COMMON_H

#include <openssl/rsa.h>
#include <stdint.h>
#include <sys/socket.h>
#include "audio.h"
#include "mdns.h"

// struct sockaddr_in6 is bigger than struct sockaddr. derp
#ifdef AF_INET6
    #define SOCKADDR struct sockaddr_storage
    #define SAFAMILY ss_family
#else
    #define SOCKADDR struct sockaddr
    #define SAFAMILY sa_family
#endif


typedef struct {
//    char *apname;
    uint8_t hw_addr[6];
    int port;
//    char *output_name;
    audio_output *output;
//    char *mdns_name;
    int buffer_start_fill;
//    char *cmd_start, *cmd_stop;
//    int cmd_blocking;
} shairport_cfg;

/* functions that ignore return values without compiler warnings.
 * for use only where return values really don't matter!
 */
#define write_unchecked(...) (void)(write(__VA_ARGS__)+1)
#define read_unchecked(...)  (void)(read (__VA_ARGS__)+1)
#define lockf_unchecked(...) (void)(lockf(__VA_ARGS__)+1)

uint8_t *base64_dec(char *input, int *outlen);
char *base64_enc(uint8_t *input, int length);

#define RSA_MODE_AUTH (0)
#define RSA_MODE_KEY  (1)
uint8_t *rsa_apply(uint8_t *input, int inlen, int *outlen, int mode);

extern shairport_cfg config;
extern char* hostname;

void shairport_shutdown(int retval);

#endif // _COMMON_H
