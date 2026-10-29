#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adresse MAC unique derivee de l'identifiant du STM32 (a appeler AVANT HAL_ETH_Init) */
void NET_GetMac(uint8_t mac[6]);

/* Initialise lwIP (tcpip_init), l'interface reseau, le PHY et le DHCP.
 * A appeler UNE fois depuis une tache FreeRTOS, a la place de tcpip_init(). */
void NET_Init(void);

/* 1 si la carte a une adresse IP (DHCP termine), 0 sinon */
int  NET_HasIp(void);

/* Attend une adresse IP pendant timeout_ms ms. Retourne 1 si obtenue, 0 sinon */
int  NET_WaitIp(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNETIF_H */
