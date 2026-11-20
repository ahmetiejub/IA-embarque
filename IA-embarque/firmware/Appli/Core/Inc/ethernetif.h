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
/* ---- Configuration de l'adresse IP de la carte ----
   NET_IP_FIXE = 1 : adresse fixe ci-dessous (a utiliser si un appareil du reseau distribue
                     de mauvaises adresses, ex. passerelle 192.168.1.142 sans Internet)
   NET_IP_FIXE = 0 : adresse donnee automatiquement par le DHCP (par defaut, marche sur
                     n'importe quel reseau). Si le serveur DHCP qui a repondu ne donne pas
                     acces a Internet, la carte l'ignore et redemande une adresse a un autre. */
#define NET_IP_FIXE        0
#define NET_IP_CARTE       "192.168.1.230"   /* libre sur le reseau, hors des adresses du PC */
#define NET_MASQUE         "255.255.255.0"
#define NET_PASSERELLE     "192.168.1.254"   /* la box (ipconfig : Passerelle par defaut)  */
#define NET_DNS            "192.168.1.254"   /* la box (ipconfig : Serveurs DNS)            */

void NET_Init(void);

/* 1 si la carte a une adresse IP (DHCP termine), 0 sinon */
int  NET_HasIp(void);
/* 1 si le cable est branche et le lien Ethernet actif */
int  NET_LinkUp(void);
/* A appeler apres un test Internet (DNS / ping) : si Internet ne repond pas 2 fois de suite,
   le serveur DHCP actuel est ignore et la carte redemande une adresse */
void NET_InternetOk(void);
void NET_InternetKo(void);

/* Attend une adresse IP pendant timeout_ms ms. Retourne 1 si obtenue, 0 sinon */
int  NET_WaitIp(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNETIF_H */
