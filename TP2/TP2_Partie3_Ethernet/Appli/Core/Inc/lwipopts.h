#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/* --- Systeme (FreeRTOS via CMSIS-RTOS2) --- */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define TCPIP_STACK_REQUIREMENTS        1
#define TCPIP_THREAD_NAME               "TCP/IP"
#define TCPIP_THREAD_STACKSIZE          4096      /* octets (1024 etait trop court) */
#define TCPIP_THREAD_PRIO               osPriorityHigh

/* Boites aux lettres : valeur 0 par defaut => osMessageQueueNew(0) echoue => tcpip_init plante */
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8

/* --- Memoire --- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_TCP_PCB                5
#define MEMP_NUM_SYS_TIMEOUT            10
#define MEMP_NUM_NETCONN                8
#define PBUF_POOL_SIZE                  16

/* --- Protocoles --- */
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1         /* sockets "bruts" : necessaires pour envoyer un vrai ping ICMP */
#define MEMP_NUM_RAW_PCB                4

/* --- API --- */
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             1
#define LWIP_POSIX_SOCKETS_IO_NAMES     1
#define LWIP_SO_RCVTIMEO                1         /* permet setsockopt(SO_RCVTIMEO) : evite un recv() bloque */
#define SO_REUSE                        1

/* --- Interface reseau --- */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1

/* --- Checksums : calcules/verifies par le materiel ETH --- */
#define CHECKSUM_BY_HARDWARE            1
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_GEN_TCP                0
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0

/* --- Nombres aleatoires (DNS, ports TCP) : requis par lwIP --- */
#include <stdlib.h>
#define LWIP_RAND()                     ((u32_t)rand())

/* --- Codes d'erreur (ENOMEM, EWOULDBLOCK...) : fournis par <errno.h> de newlib --- */
#define LWIP_ERRNO_STDINCLUDE           1

/* --- Pas d'AutoIP ni de detection de conflit d'adresse (acd.c / autoip.c exclus du build) --- */
#define LWIP_AUTOIP                     0
#define LWIP_DHCP_DOES_ACD_CHECK        0

/* --- Logs (decommenter pour debugger, voir le sujet du TP2) --- */
/* #define LWIP_DEBUG */

#endif /* __LWIPOPTS_H__ */
