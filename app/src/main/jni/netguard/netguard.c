#include <jni.h>
#include <android/log.h>

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

// 3 way handshake
// -> SYN seq=x
// <- SYN-ACK ack=x+1 seq=y
// -> ACK=y+1 seq=x+1

// https://www.gasmi.net/hpd/
// Ethernet frame: 0800 2086 354b 00e0 f726 3fe9 0800

// TODO TCP fragmentation
// TODO TCP push
// TODO header file

#define TAG "NetGuard.JNI"
#define MAXPKT 32678
#define TIMEOUTPKT 30
#define TTL 64

struct arguments {
    jobject instance;
    int tun;
};

struct data {
    uint32_t seq; // host notation
    jbyte *data;
    struct data *next;
};

struct connection {
    time_t time;
    uint32_t remote_seq; // confirmed bytes received, host notation
    uint32_t local_seq; // confirmed bytes sent, host notation
    int32_t saddr; // network notation
    __be16 source; // network notation
    int32_t daddr; // network notation
    __be16 dest; // network notation
    uint8_t state;
    jint socket;
    uint32_t lport; // host notation
    struct data *received;
    struct data *sent;
    struct connection *next;
};

void *handle_events(void *);

void handle_tcp(JNIEnv *, jobject, const uint8_t *, const uint16_t);

void sendSYN(const struct connection *cur, int tun);

void decode(JNIEnv *env, jobject, const uint8_t *, const uint16_t);

jint getUid(const int, const int, const void *, const uint16_t);

unsigned short checksum(unsigned short *, int);

void nsleep(long);

char *hex(const u_int8_t *, const u_int16_t);

// Global variables

static JavaVM *jvm;
int running = 0;
int stopped = 1;
pthread_t thread_id;
struct connection *connection = NULL;

// JNI

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1start(JNIEnv *env, jobject instance, jint tun) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Starting tun=%d", tun);

    jint rs = (*env)->GetJavaVM(env, &jvm);
    if (rs == JNI_OK)
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "GetJavaVM OK");
    else
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GetJavaVM failed");

    running = 1;
    struct arguments *args = malloc(sizeof(struct arguments));
    args->instance = (*env)->NewGlobalRef(env, instance);
    args->tun = tun;
    if (pthread_create(&thread_id, NULL, handle_events, args) != 0) {
        running = 0;
        __android_log_print(ANDROID_LOG_ERROR, TAG, "pthread_create error %d: %s",
                            errno, strerror(errno));
    }
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1stop(JNIEnv *env, jobject instance, jint tun) {
    running = 0;
    while (!stopped) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopping");
        nsleep(100 * 1000L * 1000L);
    }
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopped");
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1reload(JNIEnv *env, jobject instance, jint tun) {
    // TODO seamless handover
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Reload tun=%d", tun);
    Java_eu_faircode_netguard_SinkholeService_jni_1stop(env, instance, tun);
    Java_eu_faircode_netguard_SinkholeService_jni_1start(env, instance, tun);
}

// Private functions

void *handle_events(void *a) {
    struct arguments *args = (struct arguments *) a;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Start events tun=%d", args->tun);

    JNIEnv *env;
    jint rs = (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    if (rs == JNI_OK)
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "AttachCurrentThread OK");
    else
        __android_log_print(ANDROID_LOG_ERROR, TAG, "AttachCurrentThread failed");

    int max;
    int out;
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    fd_set swfds;
    struct timeval tv;
    char dest[20];

    stopped = 0;
    while (running) {
        time_t now = time(NULL);
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Select");

        // Select
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);

        FD_SET(args->tun, &rfds);
        FD_SET(args->tun, &efds);

        max = args->tun;

        out = 0;
        struct connection *last = NULL;
        struct connection *cur = connection;
        while (cur != NULL) {
            if (cur->time + TIMEOUTPKT < now) {
                // Log
                inet_ntop(AF_INET, &(cur->daddr), dest, sizeof(dest));
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Idle %s/%u lport=%u",
                                    dest, ntohs(cur->dest), cur->lport);

                // TODO check if open
                shutdown(cur->socket, SHUT_RDWR);
                // TODO check for errors

                if (last == NULL)
                    connection = cur->next;
                else
                    last->next = cur->next;

                struct data *prev;

                struct data *received = cur->received;
                while (received != NULL) {
                    prev = received;
                    received = received->next;
                    if (prev->data != NULL)
                        free(prev->data);
                    free(prev);
                }

                struct data *sent = cur->sent;
                while (sent != NULL) {
                    prev = sent;
                    sent = sent->next;
                    if (prev->data != NULL)
                        free(prev->data);
                    free(prev);
                }

                free(cur);

            } else {
                if (cur->state == TCP_SYN_RECV) {
                    FD_SET(cur->socket, &wfds);
                    if (cur->socket > max)
                        max = cur->socket;
                }
            }

            last = cur;
            cur = cur->next;
        }

        if (out) {
            FD_SET(args->tun, &wfds);
            if (args->tun > max)
                max = args->tun;
        }

        tv.tv_sec = 10;
        tv.tv_usec = 0;
        int ready = select(max + 1, &rfds, &wfds, &efds, &tv);
        if (ready < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "select error %d: %s",
                                errno, strerror(errno));
            nsleep(1000 * 1000L * 1000L);
            continue;
        }

        if (ready == 0)
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Yield");
        else {
            // Check tun
            if (FD_ISSET(args->tun, &efds)) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "tun exception");
                running = 0;
                break;
            }

            if (FD_ISSET(args->tun, &rfds)) {
                uint8_t buffer[MAXPKT];
                ssize_t length = read(args->tun, buffer, MAXPKT);
                if (length < 0) {
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "tun read error %d: %s",
                                        errno, strerror(errno));
                    running = 0;
                    break;
                }
                if (length > 0)
                    decode(env, args->instance, buffer, length);
                else
                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "tun empty read");
            }

            if (FD_ISSET(args->tun, &wfds)) {
                // TODO: send buffered packets
            }

            // Check sockets
            struct connection *cur = connection;
            while (cur != NULL) {
                // Check exceptions
                if (FD_ISSET(cur->socket, &efds)) {
                    int serr;
                    socklen_t optlen = sizeof(serr);
                    if (getsockopt(cur->socket, SOL_SOCKET, SO_ERROR, &serr, &optlen) < 0) {
                        // TODO
                        __android_log_print(ANDROID_LOG_ERROR, TAG, "getsockopt error %d: %s",
                                            errno, strerror(errno));
                        cur->state = TCP_CLOSE;
                        continue;
                    }
                    if (serr) {
                        // TODO
                        __android_log_print(ANDROID_LOG_ERROR, TAG, "SO_ERROR %d: %s",
                                            serr, strerror(serr));
                        cur->state = TCP_CLOSE;
                        continue;
                    }
                }

                // Check connects
                if (cur->state == TCP_SYN_RECV) {
                    if (FD_ISSET(cur->socket, &wfds)) {
                        // Log
                        char dest[20];
                        inet_ntop(AF_INET, &(cur->daddr), dest, sizeof(dest));
                        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Established %s/%u lport=%u",
                                            dest, ntohs(cur->dest), cur->lport);
                        sendSYN(cur, args->tun);
                        cur->state = TCP_SYN_SENT;
                    }
                }
                cur = cur->next;
            }
        }
    }

    (*env)->DeleteGlobalRef(env, args->instance);
    rs = (*jvm)->DetachCurrentThread(jvm);
    if (rs == JNI_OK)
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "DetachCurrentThread OK");
    else
        __android_log_print(ANDROID_LOG_ERROR, TAG, "DetachCurrentThread failed");
    free(args);

    stopped = 1;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopped events tun=%d", args->tun);
}

void handle_tcp(JNIEnv *env, jobject instance, const uint8_t *buffer, uint16_t length) {
    // Check version
    jbyte version = (*buffer) >> 4;
    if (version != 4)
        return;

    // Get headers
    struct iphdr *iphdr = buffer;
    uint8_t optlen = (iphdr->ihl - 5) * 4;
    struct tcphdr *tcphdr = buffer + sizeof(struct iphdr) + optlen;

    if (ntohs(iphdr->tot_len) != length)
        __android_log_print(ANDROID_LOG_WARN, TAG, "Invalid length %u/%d", iphdr->tot_len, length);

    // Get data
    uint16_t dataoff = sizeof(struct iphdr) + optlen + sizeof(struct tcphdr);
    uint16_t datalen = length - dataoff;
    struct data *data = malloc(sizeof(struct data));
    data->seq = ntohl(tcphdr->seq);
    data->data = malloc(datalen); // TODO free
    data->next = NULL;
    if (datalen)
        memcpy(data->data, buffer + dataoff, datalen);

    // Search connection
    struct connection *last = NULL;
    struct connection *cur = connection;
    while (cur != NULL && !(cur->saddr == iphdr->saddr && cur->source == tcphdr->source)) {
        last = cur;
        cur = cur->next;
    }

    // Log
    char dest[20];
    inet_ntop(AF_INET, &(iphdr->daddr), dest, sizeof(dest));
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "%s/%u seq %u ack %u data %d",
                        dest, ntohs(tcphdr->dest),
                        ntohl(tcphdr->seq), ntohl(tcphdr->ack_seq),
                        datalen);

    if (cur == NULL) {
        if (tcphdr->syn) {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "New SYN");

            // Register connection
            struct connection *syn = malloc(sizeof(struct connection)); // TODO check/free
            syn->time = time(NULL);
            syn->remote_seq = ntohl(tcphdr->seq);
            syn->local_seq = 123;  // TODO randomize
            syn->saddr = iphdr->saddr;
            syn->source = tcphdr->source;
            syn->daddr = iphdr->daddr;
            syn->dest = tcphdr->dest;
            syn->state = TCP_SYN_RECV;
            syn->received = data;
            syn->sent = NULL;
            syn->next = NULL;

            if (last == NULL)
                connection = syn;
            else
                last->next = syn;

            // Get TCP socket
            if ((syn->socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                // TODO
                __android_log_print(ANDROID_LOG_ERROR, TAG, "socket error %d: %s",
                                    errno, strerror(errno));
                syn->state = TCP_CLOSE;
                return;
            }

            // Set non blocking
            uint8_t flags = fcntl(syn->socket, F_GETFL, 0);
            if (flags < 0 || fcntl(syn->socket, F_SETFL, flags | O_NONBLOCK) < 0) {
                // TODO
                __android_log_print(ANDROID_LOG_ERROR, TAG, "fcntl error %d: %s",
                                    errno, strerror(errno));
                syn->state = TCP_CLOSE;
                return;
            }

            // Protect
            jclass cls = (*env)->GetObjectClass(env, instance);
            jmethodID mid = (*env)->GetMethodID(env, cls, "protect", "(I)Z");
            if (mid == 0) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "protect not found");
                syn->state = TCP_CLOSE;
                return;
            }
            else {
                jboolean isProtected = (*env)->CallBooleanMethod(env, instance, mid, syn->socket);
                if (!isProtected)
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "protect failed");

                jthrowable ex = (*env)->ExceptionOccurred(env);
                if (ex) {
                    (*env)->ExceptionDescribe(env);
                    (*env)->ExceptionClear(env);
                    (*env)->DeleteLocalRef(env, ex);
                }
            }

            // Build target address
            struct sockaddr_in a;
            memset(&a, 0, sizeof(struct sockaddr_in));
            a.sin_family = AF_INET;
            a.sin_port = tcphdr->dest;
            a.sin_addr.s_addr = iphdr->daddr;

            // Initiate connect
            int err = connect(syn->socket, &a, sizeof(struct sockaddr_in));
            if (err < 0 && errno != EINPROGRESS) {
                // TODO
                __android_log_print(ANDROID_LOG_ERROR, TAG, "connect error %d: %s",
                                    errno, strerror(errno));
                syn->state = TCP_CLOSE;
                return;
            }

            // Get local port
            struct sockaddr_in sin;
            int sinlen = sizeof(sin);
            if (getsockname(syn->socket, &sin, &sinlen) < 0)
                __android_log_print(ANDROID_LOG_ERROR, TAG, "getsockname error %d: %s",
                                    errno, strerror(errno));
            else
                syn->lport = ntohs(sin.sin_port);

            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Connecting to %s/%u lport %u",
                                dest, ntohs(tcphdr->dest), syn->lport);
        }
    }
    else {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Existing connection lport %u", cur->lport);

        if (tcphdr->syn)
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Ignoring repeated SYN");

        else if (tcphdr->ack) {
            cur->time = time(NULL);

            if (cur->state == TCP_SYN_SENT) {
                // TODO check seq
                if (ntohl(tcphdr->ack_seq) == cur->local_seq + 1 &&
                    ntohl(tcphdr->seq) == cur->remote_seq + 1) {
                    cur->local_seq++;
                    cur->remote_seq++;

                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Established");
                    cur->state = TCP_ESTABLISHED;
                }
            }
        }

        shutdown(cur->socket, SHUT_RDWR);
    }
}

void sendSYN(const struct connection *cur, int tun) {
    // Build packet
    uint16_t len = sizeof(struct iphdr) + sizeof(struct tcphdr); // no data
    u_int8_t *buffer = calloc(len, 1);
    struct iphdr *ip = buffer;
    struct tcphdr *tcp = buffer + sizeof(struct iphdr);

    // Build IP header
    ip->version = 4;
    ip->ihl = sizeof(struct iphdr) >> 2;
    ip->tot_len = htons(len);
    ip->ttl = TTL;
    ip->protocol = IPPROTO_TCP;
    ip->saddr = cur->daddr;
    ip->daddr = cur->saddr;

    // Calculate IP checksum
    ip->check = checksum(ip, sizeof(struct iphdr));

    // Build TCP header
    tcp->source = cur->dest;
    tcp->dest = cur->source;
    tcp->seq = htonl(cur->local_seq);
    tcp->ack_seq = htonl(cur->remote_seq + 1); // TODO proper wrap around
    tcp->doff = sizeof(struct tcphdr) >> 2;
    tcp->syn = 1;
    tcp->ack = 1;

    // Calculate TCP checksum
    uint16_t clen = sizeof(struct ippseudo) + sizeof(struct tcphdr);
    jbyte csum[clen];

    // Build pseudo header
    struct ippseudo *pseudo = csum;
    pseudo->ippseudo_src.s_addr = ip->saddr;
    pseudo->ippseudo_dst.s_addr = ip->daddr;
    pseudo->ippseudo_pad = 0;
    pseudo->ippseudo_p = ip->protocol;
    pseudo->ippseudo_len = htons(sizeof(struct tcphdr)); // no data

    // Copy TCP header
    memcpy(csum + sizeof(struct ippseudo), tcp, sizeof(struct tcphdr));

    tcp->check = checksum(csum, clen);

    char to[20];
    inet_ntop(AF_INET, &(ip->daddr), to, sizeof(to));

    // Send packet
    __android_log_print(ANDROID_LOG_DEBUG, TAG,
                        "Sending SYN+ACK to tun %s/%u seq %u ack %u",
                        to, ntohs(tcp->dest),
                        ntohl(tcp->seq), ntohl(tcp->ack_seq));
    if (write(tun, buffer, len) < 0) {
        // TODO
        __android_log_print(ANDROID_LOG_ERROR, TAG, "write error %d: %s",
                            errno, strerror(errno));
    }

    free(buffer);
}

void decode(JNIEnv *env, jobject instance, const uint8_t *buffer, const uint16_t length) {
    uint8_t protocol;
    void *saddr;
    void *daddr;
    char source[40];
    char dest[40];
    char flags[10];
    int flen = 0;
    uint8_t *payload;

    // Get protocol, addresses & payload
    jbyte version = (*buffer) >> 4;
    if (version == 4) {
        struct iphdr *ip4hdr = buffer;

        protocol = ip4hdr->protocol;
        saddr = &ip4hdr->saddr;
        daddr = &ip4hdr->daddr;

        if (ip4hdr->frag_off & IP_MF)
            flags[flen++] = '+';

        uint8_t optlen = (ip4hdr->ihl - 5) * 4;
        payload = buffer + 20 + optlen;

        uint16_t csum = checksum(ip4hdr, sizeof(struct iphdr));
        if (csum != 0) {
            // TODO checksum
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP checksum");
        }
    }
    else if (version == 6) {
        struct ip6_hdr *ip6hdr = buffer;

        protocol = ip6hdr->ip6_nxt;
        saddr = &ip6hdr->ip6_src;
        daddr = &ip6hdr->ip6_dst;

        payload = buffer + 40;

        // TODO checksum
    }
    else {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Unknown version %d", version);
        return;
    }

    inet_ntop(version == 4 ? AF_INET : AF_INET6, saddr, source, sizeof(source));
    inet_ntop(version == 4 ? AF_INET : AF_INET6, daddr, dest, sizeof(dest));

    // Get ports & flags
    uint16_t sport = -1;
    uint16_t dport = -1;
    if (protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = payload;

        sport = ntohs(tcp->source);
        dport = ntohs(tcp->dest);

        if (tcp->syn)
            flags[flen++] = 'S';
        if (tcp->ack)
            flags[flen++] = 'A';
        if (tcp->psh)
            flags[flen++] = 'P';
        if (tcp->fin)
            flags[flen++] = 'F';
        if (tcp->fin)
            flags[flen++] = 'R';

        // TODO checksum
    } else if (protocol == IPPROTO_UDP) {
        struct udphdr *udp = payload;

        sport = ntohs(udp->source);
        dport = ntohs(udp->dest);

        // TODO checksum
    }
    flags[flen] = 0;

    // Get uid
    jint uid = -1;
    if (protocol == IPPROTO_TCP || protocol == IPPROTO_UDP) {
        // Sleep 10 ms
        struct timespec tim, tim2;
        tim.tv_sec = 0;
        tim.tv_nsec = 10000000L;
        nanosleep(&tim, &tim2);

        // Lookup uid
        uid = getUid(protocol, version, saddr, sport);
        if (uid < 0 && version == 4) {
            int8_t saddr128[16];
            memset(saddr128, 0, 10);
            saddr128[10] = 0xFF;
            saddr128[11] = 0xFF;
            memcpy(saddr128 + 12, saddr, 4);
            uid = getUid(protocol, 6, saddr128, sport);
        }
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG,
                        "Packet v%d %s/%u -> %s/%u proto %d flags %s uid %d",
                        version, source, sport, dest, dport, protocol, flags, uid);

    if (protocol == IPPROTO_TCP)
        handle_tcp(env, instance, buffer, length);

    // Call back
    jclass cls = (*env)->GetObjectClass(env, instance);
    jmethodID mid = (*env)->GetMethodID(env, cls, "logPacket",
                                        "(ILjava/lang/String;ILjava/lang/String;IILjava/lang/String;IZ)V");
    if (mid == 0)
        __android_log_print(ANDROID_LOG_ERROR, TAG, "logPacket not found");
    else {
        jboolean allowed = 0;
        jstring jsource = (*env)->NewStringUTF(env, source);
        jstring jdest = (*env)->NewStringUTF(env, dest);
        jstring jflags = (*env)->NewStringUTF(env, flags);
        (*env)->CallVoidMethod(env, instance, mid,
                               version,
                               jsource, sport,
                               jdest, dport,
                               protocol, jflags,
                               uid, allowed);
        (*env)->DeleteLocalRef(env, jsource);
        (*env)->DeleteLocalRef(env, jdest);
        (*env)->DeleteLocalRef(env, jflags);

        jthrowable ex = (*env)->ExceptionOccurred(env);
        if (ex) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ex);
        }
    }
}

jint getUid(const int protocol, const int version, const void *saddr, const uint16_t sport) {
    char line[250];
    int fields;
    int32_t addr32;
    int8_t addr128[16];
    uint16_t port;
    jint uid = -1;

    // Get proc file name
    char *fn = NULL;
    if (protocol == IPPROTO_TCP)
        fn = (version == 4 ? "/proc/net/tcp" : "/proc/net/tcp6");
    else if (protocol == IPPROTO_UDP)
        fn = (version == 4 ? "/proc/net/udp" : "/proc/net/udp6");
    else
        return -1;

    // Open proc file
    FILE *fd = fopen(fn, "r");
    if (fd == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error opening %s", fn);
        return -1;
    }

    // Scan proc file
    int i = 0;
    while (fgets(line, sizeof(line), fd) != NULL) {
        if (i++) {
            if (version == 4)
                fields = sscanf(line,
                                "%*d: %X:%X %*X:%*X %*X %*lX:%*lX %*X:%*X %*X %d %*d %*ld ",
                                &addr32, &port, &uid);
            else
                fields = sscanf(line,
                                "%*d: %8X%8X%8X%8X:%X %*X:%*X %*X %*lX:%*lX %*X:%*X %*X %d %*d %*ld ",
                                addr128, addr128 + 4, addr128 + 8, addr128 + 12, &port, &uid);

            if (fields < 3) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid field #%d: %s", fields, line);
                break;
            }

            if (port == sport) {
                if (version == 4) {
                    if (addr32 == *((int32_t *) saddr))
                        return uid;
                }
                else {
                    if (memcmp(addr128, saddr, (size_t) 16) == 0)
                        return uid;
                }
            }
        }
    }

    fclose(fd);

    return -1;
}

// TODO data types
// TODO endianess?
unsigned short checksum(unsigned short *addr, int len) {
    register int sum = 0;
    u_short answer = 0;
    register u_short *w = addr;
    register int nleft = len;

    /*
    * Our algorithm is simple, using a 32-bit accumulator (sum),
    * we add sequential 16-bit words to it, and at the end, fold back
    * all the carry bits from the top 16 bits into the lower 16 bits.
    */

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    /* mop up an odd byte, if necessary */
    if (nleft == 1) {
        *(u_char *) (&answer) = *(u_char *) w;
        sum += answer;
    }

    /* add back carry outs from top 16 bits to low 16 bits */
    sum = (sum >> 16) + (sum & 0xffff); /* add hi 16 to low 16 */
    sum += (sum >> 16); /* add carry */
    answer = ~sum; /* truncate to 16 bits */
    return (answer);
}

void nsleep(long ns) {
    struct timespec tim, tim2;
    tim.tv_sec = ns / 1000000000L;
    tim.tv_nsec = ns % 1000000000L;
    nanosleep(&tim, &tim2);
}

char *hex(const u_int8_t *data, const u_int16_t len) {
    char hex_str[] = "0123456789ABCDEF";

    char *out;
    out = (char *) malloc(len * 2 + 1); // TODO free
    (out)[len * 2] = 0;

    if (!len) return NULL;

    for (size_t i = 0; i < len; i++) {
        (out)[i * 2 + 0] = hex_str[(data[i] >> 4) & 0x0F];
        (out)[i * 2 + 1] = hex_str[(data[i]) & 0x0F];
    }
    return out;
}