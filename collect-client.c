/*
collect-client - a client for ISPApp
Copyright 2022 Andrew Hodel

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

// required for basic wss support
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbedtls/base64.h"
#include "mbedtls/certs.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha1.h"
#include "mbedtls/ssl.h"

// required for collector data
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include <linux/if_link.h>
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/sysinfo.h>
#include <mntent.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>

#define _XOPEN_SOURCE 700
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <netlink/cache.h>
#include <netlink/genl/ctrl.h>  // genl_ctrl_resolve
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>  // genl_connect, genlmsg_put
#include <netlink/netlink.h>
#include <netlink/route/link.h>

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#define mbedtls_time time
#define mbedtls_time_t time_t
#define mbedtls_fprintf fprintf
#define mbedtls_printf printf
#define mbedtls_exit exit
#define MBEDTLS_EXIT_SUCCESS EXIT_SUCCESS
#define MBEDTLS_EXIT_FAILURE EXIT_FAILURE
#endif /* MBEDTLS_PLATFORM_C */

#include "utility.h"
#include "webshell.h"

// the level of debug logs you require. Its values can be between 0 and 5, where 5 is the most logs
#define DEBUG_LEVEL 1

// setup the ssl object outside main so it can be accessed by threads
mbedtls_ssl_context ssl;
mbedtls_net_context server_fd;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;
mbedtls_ssl_config conf;
mbedtls_x509_crt cacert;

pthread_t thread_id = 0;
pthread_t ping_thread_id = 0;
char *ping_json_string;
int thread_cancel = 0;
int update_wait;
int collector_wait = 0;
int send_col_data = 1;
int listener_update_interval_seconds = 60;
int listener_outage_interval_seconds = 300;
int authed_flag = 0;
time_t last_response;
char *root_address;
char *root_port;
char *root_wlan_if;
char *root_collect_key;
char *root_client_info = "collect-client-2.21";
char *root_hardware_make;
char *root_hardware_model;
char *root_hardware_model_number;
char *root_hardware_cpu_info;
char *root_hardware_serial;
char *root_os_build_date;
char *root_fw;
char root_mac[18];
char *root_cert_path;
char *root_config_file;
int wss_recv = -1;

static int get_mac(char *ifname, char *mac) {
  // printf("getting mac for %s\n", ifname);
  struct ifreq s;
  int fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);

  strcpy(s.ifr_name, ifname);
  if (0 == ioctl(fd, SIOCGIFHWADDR, &s)) {
    sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned char)s.ifr_addr.sa_data[0], (unsigned char)s.ifr_addr.sa_data[1], (unsigned char)s.ifr_addr.sa_data[2], (unsigned char)s.ifr_addr.sa_data[3], (unsigned char)s.ifr_addr.sa_data[4], (unsigned char)s.ifr_addr.sa_data[5]);
    return 1;
  }
  return -1;
}

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte) (byte & 0x80 ? '1' : '0'), (byte & 0x40 ? '1' : '0'), (byte & 0x20 ? '1' : '0'), (byte & 0x10 ? '1' : '0'), (byte & 0x08 ? '1' : '0'), (byte & 0x04 ? '1' : '0'), (byte & 0x02 ? '1' : '0'), (byte & 0x01 ? '1' : '0')

int wss_frame_encode_message(char *output_buf, int type, char *buf) {
  // returns the message length

  if (sizeof(buf) > UINT64_MAX) {
    // this message is too long to encode, this library does not currently support sending messages that require multiple frames
    printf("message is too long to encode into a WebSocket frame.\n");
    return -1;
  }

  // allocate space in output_buf
  // char *output_buf = calloc(strlen(buf) + 14, sizeof(char));
  // output_buf = calloc(strlen(buf) + 14, sizeof(char));

  // the first byte of the frame, bits 1-8
  //      bit 1           = FIN (1 indicates last message in a series)
  //      bit 2           = RSV1 (NA)
  //      bit 3           = RSV2 (NA)
  //      bit 4           = RSV3 (NA)
  //      bits 5-8        = opcode (0x0 continuation, 0x1 text, 0x2 binary)

  // decimal 129 is FIN=1 OPCODE=0x1
  output_buf[0] = 129;

  // printf("bits in first byte: "BYTE_TO_BINARY_PATTERN"\n", BYTE_TO_BINARY((unsigned char) output_buf[0]));

  // the second byte of the frame
  //      bit 1           = MASK (1 indicates message is XOR encoded, messages from the client must be masked)
  //      bit 2-8         = payload length (read bits 2-8 as an unsigned int up to 125, if == 126 read the next 16 bits as unsigned int, if == 127 read the next 64 bits as unsigned int)

  // set bit 1 to 1 (that is also decimal 1)
  output_buf[1] = 128;  // or output_buf[1] |= (unsigned char) 1 << 7;

  // the third and fourth byte of the frame if payload length <= UINT16_MAX
  // payload length == 126, will set payload length
  //      read 16 bits (bytes 3, 4) as unsigned int

  // the 3rd, 4th, 5th, 6th, 7th, 8th, 9th, 10th byte of the frame if payload length <= UINT64_MAX
  // payload length == 127, will set payload length
  //      read 64 bits (bytes 3, 4, 5, 6, 7, 8, 9, 10) as unsigned int

  // printf("encoding message with length: %llu\n", strlen(buf));

  // the masking key starts at byte 2
  //      if the payload is <= UINT16_MAX then it starts at byte 4
  //      if the payload is <= UINT64_MAX then it starts at byte 10
  unsigned int start_masking_key = 2;

  // basically
  //      125 = 01111101
  //      126 = 01111110
  //      127 = 01111111
  //      128 = 10000000
  if (strlen(buf) <= 125) {
    // set bits 2-8 from byte 2 with the length
    output_buf[1] = (unsigned char)strlen(buf);

  } else if (strlen(buf) <= UINT16_MAX) {
    // set payload len to 126, saying use 16 bits for the payload length
    output_buf[1] = 126;

    // set bytes 3-4 (2 bytes) as a UINT16 with the length
    unsigned int l = (unsigned int)strlen(buf);

    output_buf[2] = (char)((l >> 8) & 0xFF);
    output_buf[3] = (char)((l & 0xFF));

    start_masking_key = 4;

  } else {
    // set payload len to 127, saying use 64 bits for the payload length
    output_buf[1] = 127;

    // set bytes 3-10 (8 bytes) as a UINT64 with the length
    unsigned long long int l = (unsigned long long int)strlen(buf);

    output_buf[2] = (char)((l >> 56) & 0xFF);
    output_buf[3] = (char)((l >> 48) & 0xFF);
    output_buf[4] = (char)((l >> 40) & 0xFF);
    output_buf[5] = (char)((l >> 32) & 0xFF);
    output_buf[6] = (char)((l >> 24) & 0xFF);
    output_buf[7] = (char)((l >> 16) & 0xFF);
    output_buf[8] = (char)((l >> 8) & 0xFF);
    output_buf[9] = (char)((l & 0xFF));

    start_masking_key = 10;
  }

  // set the first bit to 1 again to enable MASK
  output_buf[1] |= (unsigned char)1 << 7;

  // printf("bits in 2nd byte: "BYTE_TO_BINARY_PATTERN"\n", BYTE_TO_BINARY((unsigned char) output_buf[1]));

  // next 4 bytes starting with start_masking_key
  // the masking key (if MASK == 1)
  // var DECODED = "";
  // for (var i = 0; i < ENCODED.length; i++) {
  //      DECODED[i] = ENCODED[i] ^ MASK[i % 4];
  //}

  // generate 4 random bytes
  char *randomBytes = calloc(4, sizeof(char));
  int rngfd = open("/dev/urandom", O_RDONLY);
  if (rngfd < 0) {
    // error opening /dev/urandom
    printf("error opening /dev/urandom!\n");
    free(randomBytes);
    return -1;
  } else {
    ssize_t result = read(rngfd, randomBytes, 4);
    if (result < 0) {
      // error getting data from /dev/urandom
      printf("error getting data from /dev/urandom.\n");
      free(randomBytes);
      close(rngfd);
      return -1;
    }
    output_buf[start_masking_key] = randomBytes[0];
    output_buf[start_masking_key + 1] = randomBytes[1];
    output_buf[start_masking_key + 2] = randomBytes[2];
    output_buf[start_masking_key + 3] = randomBytes[3];
  }

  // payload bytes
  // the payload data encoded with the masking key
  int c = 0;
  // printf("string to send:\n");
  while (c < strlen(buf)) {
    output_buf[start_masking_key + 4 + c] = randomBytes[c % 4] ^ buf[c];
    // printf("%c", buf[c], (unsigned char) buf[c]);
    c++;
  }
  // printf("\n\n");

  free(randomBytes);
  close(rngfd);

  return start_masking_key + 4 + c;
}

int get_wan(char *wan_ip) {
  FILE *f;
  char line[100], *p, *c;

  f = fopen("/proc/net/route", "r");

  while (fgets(line, 100, f)) {
    p = strtok(line, "\t");
    c = strtok(NULL, "\t");

    if (p != NULL && c != NULL) {
      if (strcmp(c, "00000000") == 0) {
        // printf("Default interface is : %s \n", p);
        break;
      }
    }
  }

  fclose(f);

  // which family do we require , AF_INET or AF_INET6
  int fm = AF_INET;
  struct ifaddrs *ifaddr, *ifa;
  int family, s;
  char host[NI_MAXHOST];

  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    return -1;
  }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL) {
      continue;
    }

    family = ifa->ifa_addr->sa_family;

    if (strcmp(ifa->ifa_name, p) == 0) {
      if (family == fm) {
        s = getnameinfo(ifa->ifa_addr, (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

        if (s != 0) {
          printf("getnameinfo() failed: %s\n", gai_strerror(s));
          return -1;
        }
        // printf("address: %s", host);
        strcpy(wan_ip, host);
      }
      // printf("\n");
    }
  }

  freeifaddrs(ifaddr);

  return 0;
}

char *dns_lookup(char *addr_host, struct sockaddr_in *addr_con) {
  // printf("\nResolving DNS..\n");
  char *ip = (char *)malloc(NI_MAXHOST * sizeof(char));
  struct hostent *host_entity;

  if ((host_entity = gethostbyname(addr_host)) == NULL) {
    // No ip found for hostname
    free(ip);
    return NULL;
  }
  // filling up address structure
  strcpy(ip, inet_ntoa(*(struct in_addr *)host_entity->h_addr));

  (*addr_con).sin_family = host_entity->h_addrtype;
  (*addr_con).sin_port = htons(0);
  (*addr_con).sin_addr.s_addr = *(long *)host_entity->h_addr;

  // printf("dns result for %s: %s\n", addr_host, host_entity->h_addr);
  // printf("%s resolved address: %s\n", addr_host, ip);

  return ip;
}

unsigned short ping_checksum(void *b, int len) {
  unsigned short *buf = b;
  unsigned int sum = 0;
  unsigned short result;

  for (sum = 0; len > 1; len -= 2) sum += *buf++;
  if (len == 1) sum += *(unsigned char *)buf;
  sum = (sum >> 16) + (sum & 0xFFFF);
  sum += (sum >> 16);
  result = ~sum;
  return result;
}

// ping packet size
#define PING_PKT_S 64

// ping packet structure
struct ping_pkt {
  struct icmphdr hdr;
  char msg[PING_PKT_S - sizeof(struct icmphdr)];
};

struct ping_response {
  char host[255];
  double avgRtt;
  double minRtt;
  double maxRtt;
  int loss;
};

void send_ping(struct sockaddr_in *ping_addr, char *ping_dom, char *ping_ip, char *hostname_str, struct ping_response *pr) {
  // ttl_val is the max number of hops
  int ttl_val = 64;
  int msg_count = 0;
  int i;
  int addr_len;
  int flag = 1;
  int msg_received_count = 0;

  struct ping_pkt pckt;
  struct sockaddr_in r_addr;
  struct timespec time_start, time_end;
  double rtt_msec = 0;
  struct timeval tv_out;
  tv_out.tv_sec = 2;
  tv_out.tv_usec = 0;

  pr->avgRtt = -1;
  pr->minRtt = 0.0;
  pr->maxRtt = 0.0;
  pr->loss = 0;

  // send icmp packets
  int sent = 0;
  while (sent < 5) {
    // printf("ping opening socket to '%s' IP: %s\n", pr->host, ping_ip);

    int sockfd;

    // root access required to do this
    sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0) {
      printf("\nping socket file descriptor not received!! Got %i\n", sockfd);
      return;
    } else {
      // printf("\nping socket file descriptor %d received\n", sockfd);
    }

    // set the socket options
    if (setsockopt(sockfd, SOL_IP, IP_TTL, &ttl_val, sizeof(ttl_val)) != 0) {
      printf("ping, setting socket options to TTL failed!\n");
      close(sockfd);
      return;
    } else {
      // printf("ping socket set to TTL..\n");
    }

    // setting timeout of recv setting
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_out, sizeof tv_out);

    // flag is whether packet was sent or not
    flag = 1;

    // filling packet
    bzero(&pckt, sizeof(pckt));

    pckt.hdr.type = ICMP_ECHO;
    pckt.hdr.un.echo.id = getpid();

    for (i = 0; i < sizeof(pckt.msg) - 1; i++) {
      pckt.msg[i] = i + '0';
    }

    pckt.msg[i] = 0;
    pckt.hdr.un.echo.sequence = msg_count++;
    pckt.hdr.checksum = ping_checksum(&pckt, sizeof(pckt));

    // send packet
    clock_gettime(CLOCK_MONOTONIC, &time_start);
    if (sendto(sockfd, &pckt, sizeof(pckt), 0, (struct sockaddr *)ping_addr, sizeof(*ping_addr)) <= 0) {
      printf("ping, packet send failure.\n");
      flag = 0;
    }

    // receive packet
    addr_len = sizeof(r_addr);

    if (recvfrom(sockfd, &pckt, sizeof(pckt), 0, (struct sockaddr *)&r_addr, (socklen_t *)&addr_len) <= 0 && msg_count > 1) {
      printf("ping packet receive failed.\n");
    } else {
      clock_gettime(CLOCK_MONOTONIC, &time_end);

      double timeElapsed = ((double)(time_end.tv_nsec - time_start.tv_nsec)) / 1000000.0;
      rtt_msec = (double)(time_end.tv_sec - time_start.tv_sec) * 1000.0 + timeElapsed;

      // if packet was not sent, don't receive
      if (flag) {
        if (pckt.hdr.type == 69) {
          // printf("ping: %d bytes from %s (h: %s) (%s) msg_seq=%d ttl=%d rtt = %lf ms\n", PING_PKT_S, ping_dom, hostname_str, ping_ip, msg_count, ttl_val, rtt_msec);

          // if there was a successful response, set avgRtt to 0 so the average can be calculated correctly
          if (pr->avgRtt == -1) {
            pr->avgRtt = 0;
          }

          // calculate the ping results
          pr->avgRtt += rtt_msec;

          // printf("avgRtt=%lf\n", pr->avgRtt);

          if (pr->minRtt == 0.0 || pr->minRtt > rtt_msec) {
            pr->minRtt = rtt_msec;
          }

          if (pr->maxRtt == 0.0 || pr->maxRtt < rtt_msec) {
            pr->maxRtt = rtt_msec;
          }

          msg_received_count++;

        }

        else

        {
          printf("ping Error..Packet received with ICMP type %d code %d\n", pckt.hdr.type, pckt.hdr.code);
        }
      }
    }

    sent++;

    close(sockfd);
  }

  // loss is an integer between 0 and 100 that represents percentage
  pr->loss = 100 - ((msg_received_count / sent) * 100);

  if (msg_received_count > 0) {
    // calculate the average from all the ping responses
    pr->avgRtt = pr->avgRtt / msg_received_count;
  }

  //printf("ping avgRtt: %lf, msg_received_count: %i, sent: %i\n", pr->avgRtt, msg_received_count, sent);

}

typedef struct {
  int id;
  struct nl_sock *socket;
  struct nl_cb *cb1, *cb2;
  int result1, result2;
} Netlink;

json_object *wap_json;

// surely one device doesn't have more than 100 interfaces, haha
unsigned long wifi_index_count = 0;
unsigned long all_wifi_indexes[100];

static struct nla_policy stats_policy[NL80211_STA_INFO_MAX + 1] = {
    [NL80211_STA_INFO_INACTIVE_TIME] = {.type = NLA_U32}, [NL80211_STA_INFO_RX_BYTES] = {.type = NLA_U32}, [NL80211_STA_INFO_TX_BYTES] = {.type = NLA_U32}, [NL80211_STA_INFO_RX_PACKETS] = {.type = NLA_U32}, [NL80211_STA_INFO_TX_PACKETS] = {.type = NLA_U32}, [NL80211_STA_INFO_SIGNAL] = {.type = NLA_U8}, [NL80211_STA_INFO_TX_BITRATE] = {.type = NLA_NESTED}, [NL80211_STA_INFO_LLID] = {.type = NLA_U16}, [NL80211_STA_INFO_PLID] = {.type = NLA_U16}, [NL80211_STA_INFO_PLINK_STATE] = {.type = NLA_U8},
};

static struct nla_policy rate_policy[NL80211_RATE_INFO_MAX + 1] = {
    [NL80211_RATE_INFO_BITRATE] = {.type = NLA_U16},
    [NL80211_RATE_INFO_MCS] = {.type = NLA_U8},
    [NL80211_RATE_INFO_40_MHZ_WIDTH] = {.type = NLA_FLAG},
    [NL80211_RATE_INFO_SHORT_GI] = {.type = NLA_FLAG},
};

static int initNl80211(Netlink *nl);
static int finish_handler(struct nl_msg *msg, void *arg);
static int getWifiName_callback(struct nl_msg *msg, void *arg);
static int getWifiInfo_callback(struct nl_msg *msg, void *arg);
static int getWifiStatus(Netlink *nl);

static int initNl80211(Netlink *nl) {
  nl->socket = nl_socket_alloc();
  if (!nl->socket) {
    fprintf(stderr, "Failed to allocate netlink socket.\n");
    return -ENOMEM;
  }

  nl_socket_set_buffer_size(nl->socket, 8192, 8192);

  if (genl_connect(nl->socket)) {
    fprintf(stderr, "Failed to connect to netlink socket.\n");
    nl_close(nl->socket);
    nl_socket_free(nl->socket);
    return -ENOLINK;
  }

  nl->id = genl_ctrl_resolve(nl->socket, "nl80211");
  if (nl->id < 0) {
    fprintf(stderr, "Nl80211 interface not found.\n");
    nl_close(nl->socket);
    nl_socket_free(nl->socket);
    return -ENOENT;
  }

  nl->cb1 = nl_cb_alloc(NL_CB_DEFAULT);
  nl->cb2 = nl_cb_alloc(NL_CB_DEFAULT);
  if ((!nl->cb1) || (!nl->cb2)) {
    fprintf(stderr, "Failed to allocate netlink callback.\n");
    nl_close(nl->socket);
    nl_socket_free(nl->socket);
    return ENOMEM;
  }
  // the last argument was a Wifi struct, it just passes that to the
  // callback so it's not needed
  nl_cb_set(nl->cb1, NL_CB_VALID, NL_CB_CUSTOM, getWifiName_callback, 0);
  nl_cb_set(nl->cb1, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &(nl->result1));
  nl_cb_set(nl->cb2, NL_CB_VALID, NL_CB_CUSTOM, getWifiInfo_callback, 0);
  nl_cb_set(nl->cb2, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &(nl->result2));

  return nl->id;
}

static int finish_handler(struct nl_msg *msg, void *arg) {
  int *ret = arg;
  *ret = 0;
  return NL_SKIP;
}

void mac_addr_n2a(char *mac_addr, unsigned char *arg) {
  int i, l;

  l = 0;
  for (i = 0; i < 6; i++) {
    if (i == 0) {
      sprintf(mac_addr + l, "%02x", arg[i]);
      l += 2;
    } else {
      sprintf(mac_addr + l, ":%02x", arg[i]);
      l += 3;
    }
  }
}

static int getWifiName_callback(struct nl_msg *msg, void *arg) {
  // all the data arrives in msg

  struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

  struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];

  // printf("getWifiName_callback()\n");
  // it can be dumped to the screen with nl_msg_dump
  // nl_msg_dump(msg, stdout);

  // parse the data into tb_msg: a struct nlattr
  nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

  if (tb_msg[NL80211_ATTR_IFNAME]) {
    // nla_get_string(tb_msg[NL80211_ATTR_IFNAME]));
  }

  if (tb_msg[NL80211_ATTR_IFINDEX]) {
    // nla_get_u32(tb_msg[NL80211_ATTR_IFINDEX]);
  }

  // this happens for each local (to the device) interface
  char dev[20];
  if_indextoname(nla_get_u32(tb_msg[NL80211_ATTR_IFINDEX]), dev);
  // printf("interface %s with index: %lu\n", dev, (unsigned long) nla_get_u32(tb_msg[NL80211_ATTR_IFINDEX]));

  // add interface to wap_json
  json_object *iface = json_object_new_object();
  json_object_object_add(iface, "interface", json_object_new_string(dev));

  json_object *stations = json_object_new_array();
  json_object_object_add(iface, "stations", stations);

  json_object_array_add(wap_json, iface);

  all_wifi_indexes[wifi_index_count] = nla_get_u32(tb_msg[NL80211_ATTR_IFINDEX]);
  wifi_index_count++;

  // this means this msg is done
  return NL_SKIP;
}

static int getWifiInfo_callback(struct nl_msg *msg, void *arg) {
  struct nlattr *tb[NL80211_ATTR_MAX + 1];
  struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
  struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];
  struct nlattr *rinfo[NL80211_RATE_INFO_MAX + 1];
  // printf("getWifiInfo_callback()\n");
  // nl_msg_dump(msg, stdout);

  // parse the msg to tb
  nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

  if (!tb[NL80211_ATTR_STA_INFO]) {
    fprintf(stderr, "sta stats missing!\n");
    return NL_SKIP;
  }
  // this
  if (nla_parse_nested(sinfo, NL80211_STA_INFO_MAX, tb[NL80211_ATTR_STA_INFO], stats_policy)) {
    fprintf(stderr, "failed to parse nested attributes!\n");
    return NL_SKIP;
  }

  if (sinfo[NL80211_STA_INFO_SIGNAL]) {
    // (int8_t) nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL]);
  }

  if (sinfo[NL80211_STA_INFO_TX_BITRATE]) {
    if (nla_parse_nested(rinfo, NL80211_RATE_INFO_MAX, sinfo[NL80211_STA_INFO_TX_BITRATE], rate_policy)) {
      fprintf(stderr, "failed to parse nested rate attributes!\n");
    } else {
      if (rinfo[NL80211_RATE_INFO_BITRATE]) {
        // nla_get_u16(rinfo[NL80211_RATE_INFO_BITRATE]);
      }
    }
  }

  // this happens for each connected station on this interface
  // and this callback function is called synchronously for each
  // interface
  char mac_addr[20], dev[20];
  mac_addr_n2a(mac_addr, nla_data(tb[NL80211_ATTR_MAC]));
  if_indextoname(nla_get_u32(tb[NL80211_ATTR_IFINDEX]), dev);

  // cast this as int8_t to get normal dBm values
  int8_t wlsignal = (int8_t)nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL_AVG]);

  // printf("Station %s (on %s): %u\n", mac_addr, dev, wlsignal);

  // add station to wap_json

  // first loop through wap_json and find the object that has interface set to dev

  int arraylen = json_object_array_length(wap_json);
  // printf("wap array length: %i\n", arraylen);

  int c = 0;
  while (c < arraylen) {
    json_object *ele = json_object_array_get_idx(wap_json, c);
    json_object *interface;
    json_object_object_get_ex(ele, "interface", &interface);

    if (strcmp(json_object_get_string(interface), dev) == 0) {
      // this ele object will contain all the stations for dev

      // get the current array of stations from ele
      json_object *stations;
      json_object_object_get_ex(ele, "stations", &stations);

      // create a new station object
      json_object *station = json_object_new_object();

      // add the mac address
      json_object_object_add(station, "mac", json_object_new_string(mac_addr));

      // add the rssi
      json_object_object_add(station, "rssi", json_object_new_int(wlsignal));

      // add the tx and rx bytes
      json_object_object_add(station, "sentBytes", json_object_new_int64(nla_get_u64(sinfo[NL80211_STA_INFO_TX_BYTES64])));
      json_object_object_add(station, "recBytes", json_object_new_int64(nla_get_u64(sinfo[NL80211_STA_INFO_RX_BYTES64])));

      // append this station to the existing stations
      json_object_array_add(stations, station);

      // printf("adding station: %s\n\t\t RSSI: %d\n", json_object_to_json_string(station), wlsignal);
    }

    c++;
  }

  // printf("wap_json after adding station: %s\n", json_object_to_json_string(wap_json));

  return NL_SKIP;
}

static int getWifiStatus(Netlink *nl) {
  nl->result1 = 1;

  struct nl_msg *msg1 = nlmsg_alloc();
  if (!msg1) {
    fprintf(stderr, "Failed to allocate netlink message.\n");
    return -2;
  }
  // this gets all interfaces
  genlmsg_put(msg1, NL_AUTO_PORT, NL_AUTO_SEQ, nl->id, 0, NLM_F_DUMP, NL80211_CMD_GET_INTERFACE, 0);

  nl_send_auto(nl->socket, msg1);

  while (nl->result1 > 0) {
    nl_recvmsgs(nl->socket, nl->cb1);
  }
  nlmsg_free(msg1);

  // get the interface index of each wifi interface
  unsigned long c = 0;
  while (c < wifi_index_count) {
    nl->result2 = 1;

    // printf("running GET_STATION for interface with index: %lu\n", all_wifi_indexes[c]);

    struct nl_msg *msg2 = nlmsg_alloc();

    if (!msg2) {
      fprintf(stderr, "Failed to allocate netlink message.\n");
      return -2;
    }
    // this should be run for each interface
    genlmsg_put(msg2, NL_AUTO_PORT, NL_AUTO_SEQ, nl->id, 0, NLM_F_DUMP, NL80211_CMD_GET_STATION, 0);

    // here set the index for each interface and the stations will be
    // returned in the second callback
    // all these callbacks are blocking and sequential of course, what
    // a joy to know javascript as well
    nla_put_u32(msg2, NL80211_ATTR_IFINDEX, all_wifi_indexes[c]);
    nl_send_auto(nl->socket, msg2);
    while (nl->result2 > 0) {
      nl_recvmsgs(nl->socket, nl->cb2);
    }
    nlmsg_free(msg2);

    c++;
  }

  return 0;
}

void *wsocket_kill() {
  // notify the peer that the connection is being closed
  printf("mbedtls_()\n");
  mbedtls_ssl_close_notify(&ssl);

  // unallocate all certificate data
  printf("mbedtls_x509_crt_free()\n");
  mbedtls_x509_crt_free(&cacert);
  // free referenced items in an SSL context and clear memory
  printf("mbedtls_ssl_free()\n");
  mbedtls_ssl_free(&ssl);
  // free the SSL configuration context
  printf("mbedtls_ssl_config_free()\n");
  mbedtls_ssl_config_free(&conf);
  // clear CTR_CRBG context data
  printf("mbedtls_ctr_drbg_free()\n");
  mbedtls_ctr_drbg_free(&ctr_drbg);
  // free the data in the entropy context
  printf("mbedtls_entropy_free()\n");
  mbedtls_entropy_free(&entropy);

  // gracefully shutdown the connection and free associated data
  printf("mbedtls_net_free()\n");
  mbedtls_net_free(&server_fd);

  printf("wsocket_kill() finished\n");
}

void *pingLoop() {

  char *ping_addresses[4];
  ping_addresses[0] = "aws-eu-west-2-ping.ispapp.co";
  ping_addresses[1] = "aws-sa-east-1-ping.ispapp.co";
  ping_addresses[2] = "aws-us-east-1-ping.ispapp.co";
  ping_addresses[3] = "aws-us-west-1-ping.ispapp.co";

  // allocate space for ping_json_string
  ping_json_string = calloc(800, sizeof(char));
  sprintf(ping_json_string, "%s", "[]");

  while (1) {

    // ping hosts
    //printf("PING LOOP\n");

    char *temp_string = calloc(800, sizeof(char));
    sprintf(temp_string, "%s", "[");

    int num_ping_hosts = 4;
    int c = 0;
    int valid_response_count = 0;

    while (c < num_ping_hosts) {

      char *ip_addr;
      struct sockaddr_in addr_con;

      struct ping_response pr;
      sprintf(pr.host, "%s", ping_addresses[c]);

      //printf("pinging %s 5 times.\n", pr.host);

      ip_addr = dns_lookup(pr.host, &addr_con);

      if (ip_addr == NULL) {
        printf("ping collector, DNS lookup failed with: %s\n", pr.host);
      } else {
        //printf("\nopening socket to '%s' IP: %s\n", pr.host, ip_addr);

        // send pings
        send_ping(&addr_con, pr.host, ip_addr, pr.host, &pr);

        //printf("ping result for host: %s, avgRtt: %lf\n", pr.host, pr.avgRtt);

        char *temp_ping_host_string = calloc(190, sizeof(char));
        sprintf(temp_ping_host_string, "{\"host\": \"%s\", \"avgRtt\": %lf, \"minRtt\": %lf, \"maxRtt\": %lf, \"loss\": %d}", pr.host, pr.avgRtt, pr.minRtt, pr.maxRtt, pr.loss);

        if (valid_response_count == 0) {
          // add without ", " at the start
        } else {
          strcat(temp_string, ", ");
        }

        strcat(temp_string, temp_ping_host_string);
        free(temp_ping_host_string);

        free(ip_addr);

        valid_response_count = 1;

      }

      c++;

    }

    // copy temp_string to ping_json_string
    strcat(temp_string, "]");
    memcpy(ping_json_string, temp_string, 800);
    free(temp_string);

    // pingLoop() sleep
    while (1) {

      if (collector_wait == 0) {
        // gather collector data
        collector_wait = 1;
        break;
      }
      // wait 1/10th of a second
      usleep(100000);
    }

  }

}

void *sendLoop(void *input) {
  int timeout_inc = 0;

  while (1) {
    if (thread_cancel == 1) {
      thread_cancel = 0;
      wsocket_kill();
      return;
    }

    if (authed_flag != 1) {
      printf("not authed, not sending update\n");

      sleep(1);
      continue;
    }

    // check if an update has been recieved
    if (wss_recv <= 0) {
      if (timeout_inc > 200) {
        authed_flag = 0;

        // force a reconnect
        thread_cancel = 1;

        // loop so the thread exits
        continue;
      }

      timeout_inc++;

      // wait 1/10th of a second
      usleep(100000);
      printf("waiting for response: %i/200\n", timeout_inc);
      continue;
    }

    //printf("sendLoop() iteration\n");

    // set wss_recv to 0
    wss_recv = 0;
    // reset the timeout counter
    timeout_inc = 0;

    time_t start = time(NULL);

    while (1) {
      // send an update every update_wait seconds
      // this sets the fastest update interval of the program
      // half a second is fast
      usleep(100000*5);

      time_t now = time(NULL);
      //printf("start: %u, now: %u, update_wait: %i; sending update in %u seconds.\n", start, now, update_wait, update_wait - (now - start));
      if (start + update_wait <= now) {
        // update_wait timeout has been reached
        break;
      }
    }

    // check if the socket is good
    int socket_poll = mbedtls_net_poll(&server_fd, MBEDTLS_NET_POLL_WRITE, 0);
    //printf("sendLoop() socket status: %i\n", socket_poll);

    // deauth if the socket isn't ready or it's been 4 rounds since the last response
    if ((socket_poll <= 0 || time(NULL) - last_response >= update_wait * 4) && update_wait != 0) {
      // stop trying to send until re-authed
      printf("deauthing session because the socket is dead or we have missed 4 responses\n");

      authed_flag = 0;

      // reconnect
      thread_cancel = 1;
      continue;
    }

    // get wan_ip
    char *wan_ip = calloc(20, sizeof(char));
    get_wan(wan_ip);
    //printf("got wan %s\n", wan_ip);

    // get collector interface
    char *interface_json_string = calloc(800, sizeof(char));

    struct ifaddrs *ifaddr, *ifa;
    int family, n;

    if (getifaddrs(&ifaddr) == -1) {
      perror("getifaddrs");
    } else {
      // Walk through linked list, maintaining head pointer so we can
      // free list later
      int real_iface_count = 0;
      for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
          continue;
        }

        family = ifa->ifa_addr->sa_family;

        if (family == AF_PACKET && ifa->ifa_data != NULL) {
          struct rtnl_link_stats *stats = ifa->ifa_data;

          struct rtnl_link *link;
          struct nl_sock *socket;
          uint64_t kbytes_in, kbytes_out, packets_in, packets_out;

          socket = nl_socket_alloc();
          nl_connect(socket, NETLINK_ROUTE);

          if (rtnl_link_get_kernel(socket, 0, ifa->ifa_name, &link) >= 0) {
            packets_in = rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS);
            packets_out = rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS);
            kbytes_in = rtnl_link_get_stat(link, RTNL_LINK_RX_BYTES);
            kbytes_out = rtnl_link_get_stat(link, RTNL_LINK_TX_BYTES);
            // printf("%s: packets_in=%" PRIu64 ",packets_out=%" PRIu64 ",kbytes_in=%" PRIu64 ",kbytes_out=%" PRIu64 "\n", ifa->ifa_name, packets_in, packets_out, kbytes_in, kbytes_out);
            rtnl_link_put(link);
          }

          nl_socket_free(socket);

          if (real_iface_count == 0) {
            // first interface

            sprintf(interface_json_string, "[{\"if\": \"%s\", \"recBytes\": %llu, \"recPackets\": %llu, \"recErrors\": %lu, \"recDrops\": %lu, \"sentBytes\": %llu, \"sentPackets\": %llu, \"sentErrors\": %lu, \"sentDrops\": %lu}", ifa->ifa_name, kbytes_in, packets_in, stats->rx_errors, stats->rx_dropped, kbytes_out, packets_out, stats->tx_errors, stats->tx_dropped);

          } else {
            // after first interface

            // increase the size
            interface_json_string = realloc(interface_json_string, strlen(interface_json_string) + 800);
            // create temporary string
            char *temp = calloc(800, sizeof(char));

            sprintf(temp, ", {\"if\": \"%s\", \"recBytes\": %llu, \"recPackets\": %llu, \"recErrors\": %lu, \"recDrops\": %lu, \"sentBytes\": %llu, \"sentPackets\": %llu, \"sentErrors\": %lu, \"sentDrops\": %lu}", ifa->ifa_name, kbytes_in, packets_in, stats->rx_errors, stats->rx_dropped, kbytes_out, packets_out, stats->tx_errors, stats->tx_dropped);

            strcat(interface_json_string, temp);
            free(temp);
          }

          real_iface_count++;
        }
      }

      strcat(interface_json_string, "]");
    }

    freeifaddrs(ifaddr);

    //printf("got interface_json_string: %s\n", interface_json_string);

    // get collector system
    struct sysinfo system_info;
    sysinfo(&system_info);

    unsigned long long uptime = system_info.uptime;
    unsigned long long procs = system_info.procs;
    unsigned long long totalram = system_info.totalram * system_info.mem_unit;
    unsigned long long freeram = system_info.freeram * system_info.mem_unit;
    unsigned long long bufferram = system_info.bufferram * system_info.mem_unit;
    unsigned long long sharedram = system_info.sharedram * system_info.mem_unit;

    char *disks_json_string = calloc(200, sizeof(char));
    struct mntent *m;
    FILE *f;
    f = setmntent(MOUNTED, "r");
    int c = 0;
    while ((m = getmntent(f))) {
      struct statvfs stat;
      statvfs(m->mnt_dir, &stat);

      // printf ("Drive: %s\t\t\t\t\tName: %s\tType: %s\tBlock Size:
      // %lu\tFragment Size: %lu\tSize of FS: %llu\tFree Blocks:
      // %llu\n", m->mnt_dir, m->mnt_fsname, m->mnt_type, stat.f_bsize,
      // stat.f_frsize, stat.f_blocks, stat.f_bfree);

      unsigned long long total = stat.f_blocks * stat.f_bsize;
      unsigned long long used = stat.f_bfree * stat.f_bsize;

      if (total == 0) {
        // disk has no space
        // skip to the next
        continue;
      }

      unsigned long long avail = total - used;

      if (strlen(disks_json_string) == 0) {
        // first
        sprintf(disks_json_string, "[{\"mount\": \"%s\", \"used\": %llu, \"avail\": %llu}", m->mnt_dir, used, avail);

      } else {
        // increase the size of the string
        disks_json_string = realloc(disks_json_string, strlen(disks_json_string) + 200);
        // create temporary string
        char *temp = calloc(200, sizeof(char));

        sprintf(temp, ", {\"mount\": \"%s\", \"used\": %llu, \"avail\": %llu}", m->mnt_dir, stat.f_blocks * stat.f_bsize, stat.f_bfree * stat.f_bsize);

        strcat(disks_json_string, temp);
        free(temp);
      }

      c++;
    }
    endmntent(f);

    if (c == 0) {
      // there were no filesystems, set empty array
      sprintf(disks_json_string, "[]");
    } else {
      // place the last character to close the array
      strcat(disks_json_string, "]");
    }

    //printf("got disks_json_string (len %u): %s\n", strlen(disks_json_string), disks_json_string);

    // write to system_json_string
    char *system_json_string = calloc(strlen(disks_json_string) + 400, sizeof(char));

    sprintf(system_json_string, "{\"load\": {\"one\": %ld, \"five\": %ld, \"fifteen\": %ld, \"processCount\": %llu}, \"memory\": {\"total\": %llu, \"free\": %llu, \"buffers\": %llu, \"cache\": %llu}, \"disks\": %s}", system_info.loads[0], system_info.loads[1], system_info.loads[2], procs, totalram, freeram, bufferram, sharedram, disks_json_string);

    //printf("got system json string: %s\n", system_json_string);

    // get collector wap
    wap_json = json_object_new_array();
    Netlink nl;
    wifi_index_count = 0;

    nl.id = initNl80211(&nl);
    if (nl.id < 0) {
      fprintf(stderr, "Error initializing netlink 802.11\n");
    } else {
      getWifiStatus(&nl);
      // printf("getWifiStatus() finished\n\n");

      nl_cb_put(nl.cb1);
      nl_cb_put(nl.cb2);
      nl_close(nl.socket);
      nl_socket_free(nl.socket);
    }

    const char *wap_json_string = json_object_to_json_string(wap_json);
    //printf("wap_json: %s\n\n", wap_json_string);

    int ret = 1, len;

    char *updateString;

    if (send_col_data == 1) {

      updateString = calloc(600 + strlen(wan_ip) + strlen(wap_json_string) + strlen(ping_json_string) + strlen(system_json_string) + strlen(interface_json_string), sizeof(char));
      sprintf(updateString, "{\"type\": \"update\", \"uptime\": %llu, \"wanIp\": \"%s\", \"collectors\": {\"wap\": %s, \"ping\": %s, \"system\": %s, \"interface\": %s}}", uptime, wan_ip, wap_json_string, ping_json_string, system_json_string, interface_json_string);

    } else {

      updateString = calloc(179, sizeof(char));
      sprintf(updateString, "{\"type\": \"update\", \"uptime\": %llu, \"wanIp\": \"%s\"}", uptime, wan_ip);

    }

    //printf("updateString: %s\n\n", updateString);

    // after the first response, write a update request json string
    char *sbuf = calloc(strlen(updateString) + 14, sizeof(char));
    long long unsigned int sbuf_len = wss_frame_encode_message(sbuf, 1, updateString);

    if (sbuf_len >= 0) {
      while ((ret = mbedtls_ssl_write(&ssl, sbuf, sbuf_len)) <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
          mbedtls_printf(" failed\n  ! mbedtls_ssl_write returned %d\n\n", ret);
          break;
        }
      }

      len = ret;
      //mbedtls_printf("sent update: %lld bytes written\n\n", sbuf_len);

    } else {
      printf("error creating websocket frame\n");
    }

    free(updateString);
    free(sbuf);

    free(wan_ip);
    free(interface_json_string);
    free(disks_json_string);
    free(system_json_string);

    while (json_object_put(wap_json) != 1) {
      // keep decrementing the object until the memory it is using is free
    }
  }

  printf("sendLoop() end\n");
}

static void my_debug(void *ctx, int level, const char *file, int line, const char *str) {
  ((void)level);

  mbedtls_fprintf((FILE *)ctx, "%s:%04d: %s", file, line, str);
  fflush((FILE *)ctx);
}

int main(int argc, char **argv) {
  if (system("which timeout > /dev/null 2>&1")) {
    // Command doesn't exist...
    printf("timeout command does not exist, install timeout\n");
    exit(1);
  } else {
    // Command does exist
    printf("timeout command exists\n");
  }

  if (argc != 14) {
    printf("Missing %i arguments.\n", 15 - argc);
    printf("Usage: ./collect-client ADDRESS PORT WLAN_IF KEY HARDWARE_MAKE HARDWARE_MODEL HARDWARE_MODEL_NUMBER HARDWARE_CPU_INFO HARDWARE_SERIAL OS_BUILD_DATE FIRMWARE ROOT_CERT_PATH CONFIG_OUTPUT_FILE\n");
    printf("\n\tExamples...\n");
    printf("\tADDRESS:\tdev.ispapp.co\t\t(the address of the websocket server)\n");
    printf("\tPORT:\t\t8550\t\t\t(the port of the websocket server)\n");
    printf("\tWLAN_IF:\teth0\t\t\t(login field is set to the mac address of WLAN_IF)\n");
    printf("\tKEY:\t\tauthkey\t\t\t(the collect key)\n\n");
    exit(0);
  } else {
    root_address = escape_string_for_json((char *)argv[1]);
    root_port = escape_string_for_json((char *)argv[2]);
    root_wlan_if = escape_string_for_json((char *)argv[3]);
    root_collect_key = escape_string_for_json((char *)argv[4]);
    root_hardware_make = escape_string_for_json((char *)argv[5]);
    root_hardware_model = escape_string_for_json((char *)argv[6]);
    root_hardware_model_number = escape_string_for_json((char *)argv[7]);
    root_hardware_cpu_info = escape_string_for_json((char *)argv[8]);
    root_hardware_serial = escape_string_for_json((char *)argv[9]);
    root_os_build_date = escape_string_for_json((char *)argv[10]);
    root_fw = escape_string_for_json((char *)argv[11]);
    root_cert_path = escape_string_for_json((char *)argv[12]);
    root_config_file = escape_string_for_json((char *)argv[13]);

    if (get_mac(root_wlan_if, root_mac)) {
      printf("LOGIN MAC ADDRESS: %s\n", root_mac);
    }
  }

  while (1) {
    printf("connecting\n");

    int ret = 1, len;
    int exit_code = MBEDTLS_EXIT_FAILURE;
    uint32_t flags;
    const char *pers = "ssl_client1";

#if defined(MBEDTLS_DEBUG_C)
    mbedtls_debug_set_threshold(DEBUG_LEVEL);
#endif

    // set first update_wait to 2 seconds
    update_wait = 2;
    authed_flag = 0;
    last_response = time(NULL) + update_wait;

    // init the rng and session data
    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    // seed the rng
    mbedtls_entropy_init(&entropy);
    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers))) != 0) {
      mbedtls_printf(" failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret);
      goto reconnect;
    }

    // load root CA certificate from root_cert_path provided as a ARGV parameter
    // 0 if all certificates parsed successfully, a positive number if partly successful or a specific X509 or PEM error code
    if ((ret = mbedtls_x509_crt_parse_path(&cacert, root_cert_path)) < 0) {
      mbedtls_printf(" failed\n  !  mbedtls_x509_crt_parse returned %d\n\n", ret);
      goto reconnect;
    }

    // generate a random baseb64 encoded string to send as Sec-WebSocket-Key
    char *randomBytes = calloc(16, sizeof(char));
    int rngfd = open("/dev/urandom", O_RDONLY);
    if (rngfd < 0) {
      // error opening /dev/urandom
      printf("error opening /dev/urandom!\n");
    } else {
      ssize_t result = read(rngfd, randomBytes, 16);
      if (result < 0) {
        // error getting data from /dev/urandom
        printf("error getting data from /dev/urandom.\n");
      }
    }

    char randomB64String[90];
    size_t b64len;
    int b64_encode_status = mbedtls_base64_encode(randomB64String, sizeof randomB64String, &b64len, randomBytes, 16);

    close(rngfd);
    free(randomBytes);

    // printf("\nrandom b64 string: (%i, %i) %s %i\n", strlen(randomB64String), b64len, randomB64String);

    // The hashing function appends the fixed string 258EAFA5-E914-47DA-95CA-C5AB0DC85B11 (a UUID) to the value from Sec-WebSocket-Key header (not decoded from base64), applies the SHA-1 hashing function, and encodes the result using base64.
    char hashCheckString[150];
    hashCheckString[0] = '\0';
    strcat(hashCheckString, randomB64String);
    strcat(hashCheckString, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    // printf("\nhashCheckString: (%i, %i) %s\n", strlen(hashCheckString), sizeof hashCheckString, hashCheckString);

    char sha1HashCheckString[20];
    int sha1_hash_status = mbedtls_sha1_ret(hashCheckString, strlen(hashCheckString), sha1HashCheckString);

    // printf("\nsha1 hash of hashCheckString: (%i) %s\n", strlen(sha1HashCheckString), sha1HashCheckString);

    char b64HashCheckString[60];
    size_t b64HashCheckStringLen;
    int b64_hash_check_encode_status = mbedtls_base64_encode(b64HashCheckString, sizeof b64HashCheckString, &b64HashCheckStringLen, sha1HashCheckString, sizeof sha1HashCheckString);

    // printf("\nb64 hash check string: (%i, %i) %s\n", strlen(b64HashCheckString), b64HashCheckStringLen, b64HashCheckString);

    // setup the GET request string
    char reqString[400];
    sprintf(reqString, "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Protocol: json\r\nSec-WebSocket-Version: 13\r\n\r\n", root_address, randomB64String);

    // printf("Initial GET request (%i):\n\n%s\n\n", strlen(reqString), reqString);

    // start the connection
    if ((ret = mbedtls_net_connect(&server_fd, root_address, root_port, MBEDTLS_NET_PROTO_TCP)) != 0) {
      mbedtls_printf(" failed\n  ! mbedtls_net_connect returned %d\n\n", ret);
      goto reconnect;
    }

    // ssl config
    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
      mbedtls_printf(" failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret);
      goto reconnect;
    }

    // ssl setup
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_dbg(&conf, my_debug, stdout);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
      mbedtls_printf(" failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret);
      goto reconnect;
    }

    if ((ret = mbedtls_ssl_set_hostname(&ssl, root_address)) != 0) {
      mbedtls_printf(" failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret);
      goto reconnect;
    }

    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // tls/ssl handshake
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        mbedtls_printf(" failed\n  ! mbedtls_ssl_handshake returned -0x%x\n\n", (unsigned int)-ret);
        goto reconnect;
      }
    }

    // verify the server certificate
    if ((flags = mbedtls_ssl_get_verify_result(&ssl)) != 0) {
      char vrfy_buf[512];
      mbedtls_printf(" failed\n");
      mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);
      mbedtls_printf("%s\n", vrfy_buf);
      goto reconnect;
    } else {
      // server cert verified
      mbedtls_printf("server cert verified\n");
    }

    // write the get request
    while ((ret = mbedtls_ssl_write(&ssl, reqString, strlen(reqString))) <= 0) {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        mbedtls_printf(" failed\n  ! mbedtls_ssl_write returned %d\n\n", ret);
        goto reconnect;
      }
    }

    int first_response = 0;
    do {
      printf("\n\nREAD LOOP\n");

      len = 8192;
      unsigned char *buf = calloc(len, sizeof(char));
      // unsigned char buf[len];

      // read up to a maximum size of buf, if the server sends more you won't get the whole json object
      ret = mbedtls_ssl_read(&ssl, buf, len);

      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        char err[5000];
        mbedtls_strerror(ret, err, 5000);
        printf("mbedtls_ssl_read() returned MBEDTLS_ERR_SSL_WANT_READ or MBEDTLS_ERR_SSL_WANT_WRITE, error: %s\n", err);
        free(buf);
        // try again
        continue;
      }

      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        char err[5000];
        mbedtls_strerror(ret, err, 5000);
        printf("mbedtls_ssl_read() returned MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY, error: %s\n", err);
        free(buf);
        goto reconnect;
      }

      if (ret < 0) {
        char err[5000];
        mbedtls_strerror(ret, err, 5000);
        printf("mbedtls_ssl_read() returned < 0, error: %s\n", err);
        free(buf);
        goto reconnect;
      }

      if (ret == 0) {
        mbedtls_printf("\n\nEOF\n\n");
        free(buf);
        goto reconnect;
      }

      //printf("buf size: %u, read %u bytes\n", len, ret);
      //printf("%s\n\n", buf);

      // set the length to the number of bytes read
      len = ret;

      // store the time this response was receieved
      last_response = time(NULL);

      if (first_response == 0) {
        // printf("first response\n\n%s\n\n", buf);

        // parse and check the required headers
        char headerHttp[40];
        char headerUpgrade[40];
        char headerConnection[40];
        char headerSecWebsocketAccept[40];  // ensure this matches b64HashCheckString calculated earlier from the random bytes we sent in Sec-WebSocket-Key

        int hbuf_realloc_size = 200;
        char *hbuf = calloc(hbuf_realloc_size, sizeof(char));
        int validHeaderCount = 0;
        int c = 0;
        int d = 0;
        while (c < len) {
          if (buf[c] == '\r' && buf[c + 1] == '\n') {
            c++;
          }

          if (c + 1 == len) {
            // finished, this skips the last \r\n for parsing
            break;
          }

          if (d % hbuf_realloc_size == 0 && d != 0) {
            int multiple = (d / hbuf_realloc_size) + 2;

            // printf("realloc with multiple: %i, c:%i, d:%i\n", multiple, c, d);

            // increase the size of hbuf
            hbuf = (char *)realloc(hbuf, sizeof(char) * multiple * hbuf_realloc_size);
          }

          if (buf[c] == '\n' && buf[c - 1] == '\r') {
            // terminate the string for hbuf
            hbuf[d] = '\0';

            // copy the header to the char array for this header
            // strcpy(headerHttp, hbuf);
            // printf("header: %s\n", hbuf);

            if (strstr(hbuf, "HTTP/1.1 ") != 0) {
              l_strcpy(headerHttp, hbuf, 8, -1);
              // printf("headerHttp: %s\n", headerHttp);

              if (headerHttp[0] == '1' && headerHttp[1] == '0' && headerHttp[2] == '1') {
                // 101 response code
                validHeaderCount++;
                // printf("valid 101\n");
              }

            } else if (strstr(hbuf, "Upgrade: ") != 0) {
              l_strcpy(headerUpgrade, hbuf, 8, -1);
              // printf("headerUpgrade: %s\n", headerUpgrade);

              if (strcmp(headerUpgrade, "websocket") == 0) {
                // http upgraded to websocket
                validHeaderCount++;
                // printf("valid Upgrade: websocket\n");
              }

            } else if (strstr(hbuf, "Connection: ") != 0) {
              l_strcpy(headerConnection, hbuf, 11, -1);
              // printf("headerConnection: %s\n", headerConnection);

              if (strcmp(headerConnection, "Upgrade") == 0) {
                // denotes this is an upgrade
                validHeaderCount++;
                // printf("valid Connection: Upgrade\n");
              }

            } else if (strstr(hbuf, "Sec-WebSocket-Accept: ") != 0) {
              l_strcpy(headerSecWebsocketAccept, hbuf, 21, -1);
              // printf("headerSecWebsocketAccept: %s, %s\n", headerSecWebsocketAccept, b64HashCheckString);

              if (strcmp(headerSecWebsocketAccept, b64HashCheckString) == 0) {
                // the Sec-WebSocket-Key we provided and hashed matches the Sec-WebSocket-Accept response
                validHeaderCount++;
                // printf("valid hash\n");
              }
            }

            d = 0;
          } else {
            // copy this character to the header buffer
            hbuf[d] = buf[c];
            d++;
          }

          c++;
        }

        free(hbuf);

        printf("validHeaderCount: %i\n", validHeaderCount);

        if (validHeaderCount != 4) {
          printf("Missing the 4 required http response headers to make a valid WebSocket session\n");
          free(buf);
          goto reconnect;
        }

        first_response = 1;

        printf("\nWriting json config request to wss:\n");

        // get osVersion
        struct utsname uts;
        char *os_version = calloc(400, sizeof(char));
        int uname_err = uname(&uts);
        if (uname_err != 0) {
          printf("uname error=%d\n", uname_err);
        } else {
          sprintf(os_version, "%s %s %s %s %s", uts.sysname, uts.nodename, uts.release, uts.version, uts.machine);
        }

        char *config_req = calloc(strlen(root_mac) + strlen(root_collect_key) + strlen(root_client_info) + strlen(os_version) + strlen(os_version) + strlen(root_hardware_make) + strlen(root_hardware_model) + strlen(root_hardware_model_number) + strlen(root_hardware_cpu_info) + strlen(root_hardware_serial) + strlen(root_os_build_date) + strlen(root_fw) + 500, sizeof(char));
        sprintf(config_req, "{\"type\": \"config\", \"login\": \"%s\", \"key\": \"%s\", \"clientInfo\": \"%s\", \"os\": \"%s\", \"osVersion\": \"%s\", \"hardwareMake\": \"%s\", \"hardwareModel\": \"%s\", \"hardwareModelNumber\": \"%s\", \"hardwareCpuInfo\": \"%s\", \"hardwareSerialNumber\": \"%s\", \"osBuildDate\": %u, \"fw\": \"%s\"}", root_mac, root_collect_key, root_client_info, os_version, os_version, root_hardware_make, root_hardware_model, root_hardware_model_number, root_hardware_cpu_info, root_hardware_serial, atoi(root_os_build_date), root_fw);

        // printf("config req: %s\n", config_req);

        // after the first response, write a config request json string
        char *sbuf = calloc(strlen(config_req) + 14, sizeof(char));
        long long unsigned int sbuf_len = wss_frame_encode_message(sbuf, 1, config_req);

        if (sbuf_len >= 0) {
          while ((ret = mbedtls_ssl_write(&ssl, sbuf, sbuf_len)) <= 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
              mbedtls_printf(" failed\n  ! mbedtls_ssl_write returned %d\n\n", ret);

              free(config_req);
              free(os_version);
              free(sbuf);
              free(buf);

              goto reconnect;
            }
          }

          len = ret;
          mbedtls_printf("\t%lld bytes written\n\n", sbuf_len);

        } else {
          printf("error creating websocket frame\n");
        }

        free(config_req);
        free(os_version);
        free(sbuf);

      } else {
        // this is a response after the ws upgrade
        // printf("\n\nResponse Frame:\n");
        // printf("1st byte: %u: %c\n", (unsigned char) buf[0], buf[0]);
        // printf("2nd byte: %u: %c\n", (unsigned char) buf[1], buf[1]);
        // printf("\n\n");

        // make sure the first byte is 129, indicating a FIN and opcode=text
        if (buf[0] != 129) {
          printf("skipping message from server because it is not type: text\n");
          free(buf);
          continue;
        }

        // get the payload length and start position, by getting the len from the byte array
        int payload_start = 2;
        int payload_len;
        if (buf[1] <= 125) {
          // printf("payload_len is from 7 bits\n");
          payload_len = buf[1];
        } else if (buf[1] == 126) {
          // printf("payload_len is from 2 bytes\n");
          payload_len = (buf[2] << 8) + buf[3];
          payload_start = 4;

        } else if (buf[1] == 127) {
          // printf("payload_len is from 8 bytes\n");
          payload_len = (buf[2] << 8) + buf[3] + buf[4] + buf[5] + buf[6] + buf[7] + buf[8] + buf[9];
          payload_start = 10;
        }

        if (buf[1] < 128) {
          // since there is no masking key, get the payload from buf[payload_start]
          // printf("no masking key, payload len: %u\n", payload_len);

          // shift buf to only include the payload
          memmove(buf, buf + payload_start, payload_len);
          // null terminate the payload string
          buf[payload_len] = '\0';

        } else {
          // printf("applying masking key, payload len: %u\n", payload_len);

          char masking_key[4];
          masking_key[0] = buf[payload_start];
          masking_key[1] = buf[payload_start + 1];
          masking_key[2] = buf[payload_start + 2];
          masking_key[3] = buf[payload_start + 3];

          // shift buf to only include the payload
          memmove(buf, buf + payload_start + 4, payload_len);
          // null terminate the payload string
          buf[payload_len] = '\0';

          int c = 0;
          while (c < strlen(buf)) {
            buf[c] = masking_key[c % 4] ^ buf[c];
            c++;
          }
        }

        // print the payload data
        //printf("payload (%u, %u): %s\n", payload_len, strlen(buf), buf);

        struct json_object *json;

        json = json_tokener_parse(buf);
        if (!json) {
          printf("error parsing json response from:\n\n%s\n", buf);
          free(buf);
          goto reconnect;
        } else {
          const char *json_dump = json_object_to_json_string_ext(json, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY);
          //printf("\nserver responded with:\n---\n%s\n---\n", json_dump);

          // response must have a type field
          struct json_object *type;
          if (!json_object_object_get_ex(json, "type", &type)) {
            // there was no type field, reconnect
            free(buf);
            goto reconnect;
          }

          const char *type_string = json_object_get_string(type);

          if (strcmp(type_string, "error") == 0) {
            struct json_object *e_msg;
            if (json_object_object_get_ex(json, "error", &e_msg)) {
              const char *e_msg_string = json_object_get_string(e_msg);

              printf("Error message from Listener: %s\n", e_msg_string);

            } else {
              printf("Error from Listener: no error_message.\n");
            }

            while (json_object_put(json) != 1) {
              // keep decrementing the object until the memory it is using is free
            }

            free(buf);
            goto reconnect;

          } else if (authed_flag == 0 && strcmp(type_string, "config") == 0) {
            // this is a config response

            // set wss_recv to 1, indicating that we have recieved a response
            // allowing the sendLoop function to send another update
            wss_recv = 1;

            printf("checking client.authed for msg.type config\n");
            struct json_object *client;
            if (json_object_object_get_ex(json, "client", &client)) {
              // if type == config and client.authed == true then we
              // can set authed_flag to 1
              struct json_object *authed;
              if (json_object_object_get_ex(client, "authed", &authed)) {
                bool authed_bool = json_object_get_boolean(authed);

                if (authed_bool) {
                  printf("client.authed is true, this host is authenticated and it is time to set startup configuration values\n");
                  authed_flag = 1;

                  // start a detached thread for sending updates
                  pthread_create(&thread_id, NULL, sendLoop, NULL);
                  pthread_detach(thread_id);

                  // start a ping thread
                  pthread_create(&ping_thread_id, NULL, pingLoop, NULL);
                  pthread_detach(ping_thread_id);

                  // write the configuration parameters to file
                  if (root_config_file[0] != '\0') {
                    FILE *fp = fopen(root_config_file, "w");
                    if (!fp) {
                      printf("There was an error opening '%s', could not write the configuration to file.\n", root_config_file);
                      exit(0);
                    } else {
                      struct json_object *host;
                      if (json_object_object_get_ex(client, "host", &host)) {
                        // check if the host should be rebooted
                        // for example, a wss type=cmd request for reboot could have been issued by the server
                        // but this client could have been disconnected at the time the cmd was sent due to a
                        // network disconnect.
                        //
                        // this would cause the script to reconnect to the wss listener when it has an internet connection
                        // and it would first issue this config request, getting a response of reboot=1 meaning reboot this device

                        struct json_object *reboot_js;
                        if (json_object_object_get_ex(host, "reboot", &reboot_js)) {
                          int reboot_int = json_object_get_int(reboot_js);

                          if (reboot_int == 1) {
                            // reboot this host
                            sync();
                            reboot(LINUX_REBOOT_CMD_RESTART);
                          }
                        }

                        // set the outage interval
                        struct json_object *outage_interval_seconds_js;
                        if (json_object_object_get_ex(host, "outageIntervalSeconds", &outage_interval_seconds_js)) {
                          int outage_interval_seconds = json_object_get_int(outage_interval_seconds_js);

                          // use this value for the normal update interval
                          listener_outage_interval_seconds = outage_interval_seconds;
                        }

                        // set the update interval
                        struct json_object *update_interval_seconds_js;
                        if (json_object_object_get_ex(host, "updateIntervalSeconds", &update_interval_seconds_js)) {
                          int update_interval_seconds = json_object_get_int(update_interval_seconds_js);

                          // use this value for the normal update interval
                          listener_update_interval_seconds = update_interval_seconds;
                        }

                        const char *host_json_dump = json_object_to_json_string_ext(host, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY);
                        fprintf(fp, "%s", host_json_dump);
                      }
                      fclose(fp);
                    }
                  }
                }
              }
            }

          } else if (authed_flag == 1 && strcmp(type_string, "update") == 0) {
            // this is an update response

            // set wss_recv to 1, indicating that we have recieved a response
            // allowing the sendLoop function to send another update
            wss_recv = 1;

            // check if updateFast is true
            struct json_object *uf;
            if (json_object_object_get_ex(json, "updateFast", &uf)) {
              bool uf_bool = json_object_get_boolean(uf);

              // printf("updateFast is %d\n", uf_bool);

              if (uf_bool == true) {
                // server has instructed the client to updateFast
                collector_wait = 0;
                update_wait = 0;
                send_col_data = 1;
              } else {
                // server has instructed the client to not updateFast

                // get the offsets
                struct json_object *lastUpdateOffsetSec;
                json_object_object_get_ex(json, "lastUpdateOffsetSec", &lastUpdateOffsetSec);
                int lastUpdateOffsetSec_int = json_object_get_int(lastUpdateOffsetSec);

                struct json_object *lastColUpdateOffsetSec;
                json_object_object_get_ex(json, "lastColUpdateOffsetSec", &lastColUpdateOffsetSec);
                int lastColUpdateOffsetSec_int = json_object_get_int(lastColUpdateOffsetSec);

                // set to listener_outage_interval_seconds
                update_wait = listener_outage_interval_seconds - lastUpdateOffsetSec_int;

                if (listener_update_interval_seconds - lastColUpdateOffsetSec_int <= update_wait) {
                  collector_wait = 0;
                  send_col_data = 1;
                  update_wait = listener_update_interval_seconds - lastColUpdateOffsetSec_int;
                } else {
                  send_col_data = 0;
                }

                if (update_wait < 0) {
                  update_wait = 0;
                }
              }

              //printf("outage: %i, update:%i\n", listener_outage_interval_seconds, listener_update_interval_seconds);
              //printf("set update_wait to %i seconds\n", update_wait);

            }

          } else if (authed_flag == 1 && strcmp(type_string, "cmd") == 0) {
            // this is a command that should be run on the host

            struct json_object *cmd;
            json_object_object_get_ex(json, "cmd", &cmd);
            const char *cmd_string = json_object_get_string(cmd);

            char *out_stdout;
            char *out_stderr;
            unsigned long out_size;
            unsigned long err_size;

            execCommand(cmd_string, &out_stdout, &out_size, &out_stderr, &err_size);

            printf("\nSTDOUT:\n%s\n\n", out_stdout);
            printf("\nSTDERR:\n%s\n\n", out_stderr);

            if (strlen(out_stdout) > 4000) {
              sprintf(out_stderr, "command output is too long: %u bytes\ntry sending the output to a file with >", strlen(out_stdout));
            }

            // allocate enough space for the b64 encoded string by using twice the strlen, +1 incase the strlen is 0
            char *e_out_stdout = calloc((strlen(out_stdout) * 2) + 1, sizeof(char));
            size_t e_out_stdout_len;
            int e_out_stdout_encode_status = mbedtls_base64_encode(e_out_stdout, strlen(out_stdout) * 2, &e_out_stdout_len, out_stdout, strlen(out_stdout));
            e_out_stdout[e_out_stdout_len] = '\0';
            // printf("e_out_stdout strlen(): %u, %u\n", strlen(e_out_stdout), e_out_stdout_len);

            // allocate enough space for the b64 encoded string by using twice the strlen, +1 incase the strlen is 0
            char *e_out_stderr = calloc((strlen(out_stderr) * 2) + 1, sizeof(char));
            size_t e_out_stderr_len;
            int e_out_stderr_encode_status = mbedtls_base64_encode(e_out_stderr, strlen(out_stderr) * 2, &e_out_stderr_len, out_stderr, strlen(out_stderr));
            e_out_stderr[e_out_stderr_len] = '\0';
            // printf("e_out_stderr strlen(): %u, %u\n", strlen(e_out_stderr), e_out_stderr_len);

            struct json_object *uuidv4;
            json_object_object_get_ex(json, "uuidv4", &uuidv4);
            const char *uuidv4_string = json_object_get_string(uuidv4);

            struct json_object *ws_id;
            json_object_object_get_ex(json, "ws_id", &ws_id);
            const char *ws_id_string = json_object_get_string(ws_id);

            char *update = calloc(strlen(uuidv4_string) + strlen(e_out_stdout) + strlen(e_out_stderr) + strlen(ws_id_string) + 200, sizeof(char));
            sprintf(update, "{\"type\": \"cmd\", \"uuidv4\": \"%s\", \"stdout\": \"%s\", \"stderr\": \"%s\", \"ws_id\": \"%s\"}", uuidv4_string, e_out_stdout, e_out_stderr, ws_id_string);

            printf("responding with command output: %s\n", update);

            // cl->send(cl, update, strlen(update), UWSC_OP_TEXT);
            int ret = 1, len;

            char *sbuf = calloc(strlen(update) + 14, sizeof(char));
            long long unsigned int sbuf_len = wss_frame_encode_message(sbuf, 1, update);

            if (sbuf_len >= 0) {
              while ((ret = mbedtls_ssl_write(&ssl, sbuf, sbuf_len)) <= 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                  mbedtls_printf(" failed\n  ! mbedtls_ssl_write returned %d\n\n", ret);

                  free(buf);

                  free(update);
                  free(sbuf);

                  free(out_stdout);
                  free(out_stderr);
                  free(e_out_stdout);
                  free(e_out_stderr);

                  while (json_object_put(json) != 1) {
                    // keep decrementing the object until the memory it is using is free
                  }

                  goto reconnect;
                }
              }

              len = ret;

            } else {
              printf("error creating websocket frame.\n");
            }

            free(update);
            free(sbuf);

            free(out_stdout);
            free(out_stderr);
            free(e_out_stdout);
            free(e_out_stderr);
          }
        }

        while (json_object_put(json) != 1) {
          // keep decrementing the object until the memory it is using is free
        }
      }

      free(buf);

    }

    while (1);

  reconnect:

    wsocket_kill();

    // reconnect
    printf("reconnecting...\n");
    sleep(2);
  }

  printf("ending cleanly\n");

  free(root_address);
  free(root_port);
  free(root_wlan_if);
  free(root_collect_key);
  free(root_hardware_make);
  free(root_hardware_model);
  free(root_hardware_model_number);
  free(root_hardware_cpu_info);
  free(root_hardware_serial);
  free(root_os_build_date);
  free(root_fw);
  free(root_cert_path);
  free(root_config_file);
}
