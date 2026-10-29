/**
 * ethernetif.c - Pilote reseau lwIP pour STM32N6 (HAL ETH multi-canaux) sous FreeRTOS (CMSIS-RTOS2)
 *
 * - PHY detecte automatiquement (scan MDIO) et pilote avec les registres IEEE 802.3 standard
 * - Reception en interruption (zero-copy, buffers dans un pool statique)
 * - Emission en polling (simple et robuste)
 * - DHCP lance automatiquement a chaque (re)connexion du cable
 */
#include "ethernetif.h"
#include "main.h"
#include "cmsis_os2.h"

#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "lwip/sys.h"
#include "netif/ethernet.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* Parametres                                                                */
/* ------------------------------------------------------------------------- */
#define ETH_RX_BUFFER_SIZE     1536U       /* >= RxBuffLen (1524) de MX_ETH1_Init, multiple de 32 */
#define ETH_RX_BUFFER_CNT      24U         /* buffers de reception disponibles */
#define ETH_TX_TIMEOUT_MS      50U

/* Registres PHY standard (IEEE 802.3 clause 22) */
#define PHY_BMCR               0U
#define PHY_BMSR               1U
#define PHY_ID1                2U
#define PHY_ANAR               4U
#define PHY_ANLPAR             5U
#define PHY_BMCR_RESET         (1U << 15)
#define PHY_BMCR_ANEN          (1U << 12)
#define PHY_BMCR_ANRESTART     (1U << 9)
#define PHY_BMSR_LINK          (1U << 2)
#define PHY_BMSR_ANDONE        (1U << 5)
#define PHY_CAP_100FD          (1U << 8)
#define PHY_CAP_100HD          (1U << 7)
#define PHY_CAP_10FD           (1U << 6)

/* ------------------------------------------------------------------------- */
/* Variables (heth1 est cree par CubeMX dans main.c)                         */
/* ------------------------------------------------------------------------- */
extern ETH_HandleTypeDef heth1;

static struct netif gnetif;
static osSemaphoreId_t rx_sem;
static ETH_TxPacketConfigTypeDef tx_cfg;
static int32_t phy_addr = -1;

/* Pool de buffers de reception : pbuf_custom doit etre le 1er champ */
typedef struct
{
  struct pbuf_custom pbuf_custom;
  uint8_t buff[ETH_RX_BUFFER_SIZE] __attribute__((aligned(32)));
  volatile uint8_t used;
} RxBuff_t;

static RxBuff_t rx_pool[ETH_RX_BUFFER_CNT] __attribute__((aligned(32)));

/* ------------------------------------------------------------------------- */
/* printf thread-safe : evite que les messages des taches se melangent        */
/* ------------------------------------------------------------------------- */
extern UART_HandleTypeDef huart1;
static osMutexId_t print_mutex;

int _write(int file, char *ptr, int len)
{
  (void)file;
  uint8_t locked = 0U;
  if (print_mutex != NULL && osKernelGetState() == osKernelRunning &&
      osMutexAcquire(print_mutex, osWaitForever) == osOK)
  {
    locked = 1U;
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
  if (locked != 0U)
  {
    osMutexRelease(print_mutex);
  }
  return len;
}

/* ------------------------------------------------------------------------- */
/* Adresse MAC unique                                                        */
/* ------------------------------------------------------------------------- */
void NET_GetMac(uint8_t mac[6])
{
  uint32_t a = HAL_GetUIDw0();
  uint32_t b = HAL_GetUIDw1();
  uint32_t c = HAL_GetUIDw2();
  uint32_t x = (a * 2654435761u) ^ (b * 40503u) ^ (c * 2246822519u);
  uint32_t y = (a ^ (b * 31u) ^ (c * 131u)) * 3266489917u;

  mac[0] = 0x02;                 /* bit 1 = "localement administree", bit 0 = 0 (unicast) */
  mac[1] = (uint8_t)(x >> 24);
  mac[2] = (uint8_t)(x >> 16);
  mac[3] = (uint8_t)(x >> 8);
  mac[4] = (uint8_t)(x);
  mac[5] = (uint8_t)(y >> 24);
}

/* ------------------------------------------------------------------------- */
/* Pool de reception (callbacks du HAL ETH)                                  */
/* ------------------------------------------------------------------------- */
static void rx_pbuf_free(struct pbuf *p)
{
  RxBuff_t *rx = (RxBuff_t *)p;       /* pbuf_custom est le 1er champ */
  rx->used = 0U;
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  *buff = NULL;
  sys_prot_t lev = sys_arch_protect();
  for (uint32_t i = 0; i < ETH_RX_BUFFER_CNT; i++)
  {
    if (rx_pool[i].used == 0U)
    {
      rx_pool[i].used = 1U;
      *buff = rx_pool[i].buff;
      break;
    }
  }
  sys_arch_unprotect(lev);
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd   = (struct pbuf **)pEnd;
  RxBuff_t *rx = (RxBuff_t *)(buff - offsetof(RxBuff_t, buff));

  rx->pbuf_custom.custom_free_function = rx_pbuf_free;
  struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, Length, PBUF_REF, &rx->pbuf_custom,
                                       rx->buff, ETH_RX_BUFFER_SIZE);
  if (p == NULL)
  {
    rx->used = 0U;
    return;
  }

  if (*ppStart == NULL)
  {
    *ppStart = p;
  }
  else
  {
    /* paquet sur plusieurs buffers : on met a jour tot_len des buffers precedents */
    for (struct pbuf *q = *ppStart; q != NULL; q = q->next)
    {
      q->tot_len = (u16_t)(q->tot_len + Length);
    }
    (*ppEnd)->next = p;
  }
  *ppEnd = p;
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
  (void)heth;
  if (rx_sem != NULL)
  {
    osSemaphoreRelease(rx_sem);
  }
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
  (void)heth;
  /* ex. buffer de reception indisponible (RBU) : on relance la lecture, ce qui reconstruit les descripteurs */
  if (rx_sem != NULL)
  {
    osSemaphoreRelease(rx_sem);
  }
}

/* Interruption ETH (vecteur defini dans le fichier de demarrage) */
void ETH1_IRQHandler(void)
{
  HAL_ETH_IRQHandler(&heth1);
}

/* ------------------------------------------------------------------------- */
/* Emission                                                                  */
/* ------------------------------------------------------------------------- */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  (void)netif;
  ETH_BufferTypeDef segs[ETH_TX_DESC_CNT];
  struct pbuf *tmp = NULL;
  uint32_t n = 0;

  for (struct pbuf *q = p; q != NULL; q = q->next)
  {
    n++;
  }
  if (n > ETH_TX_DESC_CNT)
  {
    /* trop de morceaux : on copie le paquet dans un seul buffer contigu */
    tmp = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    if (tmp == NULL)
    {
      return ERR_MEM;
    }
    p = tmp;
  }

  n = 0;
  for (struct pbuf *q = p; q != NULL; q = q->next)
  {
    segs[n].buffer = (uint8_t *)q->payload;
    segs[n].len    = q->len;
    segs[n].next   = (q->next != NULL) ? &segs[n + 1U] : NULL;
    n++;
  }

  tx_cfg.Length   = p->tot_len;
  tx_cfg.TxBuffer = segs;
  tx_cfg.TxDMACh  = 0U;
  tx_cfg.pData    = p;

  err_t res = ERR_OK;
  if (HAL_ETH_Transmit(&heth1, &tx_cfg, ETH_TX_TIMEOUT_MS) != HAL_OK)
  {
    res = ERR_IF;
  }

  if (tmp != NULL)
  {
    pbuf_free(tmp);
  }
  return res;
}

/* ------------------------------------------------------------------------- */
/* Reception (tache)                                                         */
/* ------------------------------------------------------------------------- */
static void input_thread(void *arg)
{
  (void)arg;
  for (;;)
  {
    if (osSemaphoreAcquire(rx_sem, osWaitForever) != osOK)
    {
      continue;
    }
    for (uint32_t ch = 0; ch < ETH_DMA_CH_CNT; ch++)
    {
      heth1.RxOpCH = ch;
      struct pbuf *p = NULL;
      while (HAL_ETH_ReadData(&heth1, (void **)&p) == HAL_OK)
      {
        if (p != NULL)
        {
          if (gnetif.input(p, &gnetif) != ERR_OK)
          {
            pbuf_free(p);
          }
          p = NULL;
        }
      }
    }
    heth1.RxOpCH = 0U;
  }
}

/* ------------------------------------------------------------------------- */
/* PHY                                                                       */
/* ------------------------------------------------------------------------- */
static uint32_t phy_read(uint32_t reg)
{
  uint32_t v = 0;
  if (HAL_ETH_ReadPHYRegister(&heth1, (uint32_t)phy_addr, reg, &v) != HAL_OK)
  {
    return 0U;
  }
  return v;
}

static void phy_write(uint32_t reg, uint32_t val)
{
  (void)HAL_ETH_WritePHYRegister(&heth1, (uint32_t)phy_addr, reg, val);
}

static int phy_find(void)
{
  uint32_t n_err = 0, n_ffff = 0, n_zero = 0;

  for (uint32_t a = 0; a < 32U; a++)
  {
    uint32_t id = 0;
    if (HAL_ETH_ReadPHYRegister(&heth1, a, PHY_ID1, &id) != HAL_OK)
    {
      n_err++;
    }
    else if (id == 0xFFFFU)
    {
      n_ffff++;
    }
    else if (id == 0x0000U)
    {
      n_zero++;
    }
    else
    {
      phy_addr = (int32_t)a;
      printf("[NET] PHY trouve a l'adresse %lu (ID1=0x%04lX)\r\n", (unsigned long)a, (unsigned long)id);
      return 1;
    }
  }
  /* Diagnostic : que repond le bus MDIO ? */
  printf("[NET] Scan MDIO : %lu timeout, %lu x 0xFFFF, %lu x 0x0000 (sur 32 adresses)\r\n",
         (unsigned long)n_err, (unsigned long)n_ffff, (unsigned long)n_zero);
  return 0;
}

static void phy_start(void)
{
  phy_write(PHY_BMCR, PHY_BMCR_RESET);
  for (int i = 0; i < 50; i++)
  {
    osDelay(10);
    if ((phy_read(PHY_BMCR) & PHY_BMCR_RESET) == 0U)
    {
      break;
    }
  }
  phy_write(PHY_BMCR, PHY_BMCR_ANEN | PHY_BMCR_ANRESTART);
}

/* ------------------------------------------------------------------------- */
/* Interface lwIP                                                            */
/* ------------------------------------------------------------------------- */
static err_t ethernetif_init(struct netif *netif)
{
#if LWIP_NETIF_HOSTNAME
  netif->hostname = "stm32n6-meteo";
#endif
  netif->name[0] = 's';
  netif->name[1] = 't';
  netif->output     = etharp_output;
  netif->linkoutput = low_level_output;
  netif->hwaddr_len = ETH_HWADDR_LEN;
  memcpy(netif->hwaddr, heth1.Init.MACAddr, ETH_HWADDR_LEN);
  netif->mtu   = 1500;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

static void status_cb(struct netif *n)
{
  if (!ip4_addr_isany_val(*netif_ip4_addr(n)))
  {
    printf("[NET] IP obtenue : %s\r\n", ip4addr_ntoa(netif_ip4_addr(n)));
  }
}

int NET_HasIp(void)
{
  return !ip4_addr_isany_val(*netif_ip4_addr(&gnetif));
}

int NET_WaitIp(uint32_t timeout_ms)
{
  uint32_t t = 0;
  while (!NET_HasIp() && t < timeout_ms)
  {
    osDelay(100);
    t += 100U;
  }
  return NET_HasIp();
}

/* Tache de surveillance du lien : demarre/arrete l'ETH et le DHCP selon l'etat du cable */
static void link_thread(void *arg)
{
  (void)arg;
  uint8_t link_up = 0U;

  while (!phy_find())
  {
    printf("[NET] Aucun PHY detecte (MDIO). Nouvel essai...\r\n");
    osDelay(2000);
  }
  phy_start();

  for (;;)
  {
    (void)phy_read(PHY_BMSR);                 /* la 1re lecture efface le bit "link down" memorise */
    uint32_t bmsr = phy_read(PHY_BMSR);
    uint8_t up = ((bmsr & PHY_BMSR_LINK) != 0U) && ((bmsr & PHY_BMSR_ANDONE) != 0U);

    if (up && !link_up)
    {
      uint32_t common = phy_read(PHY_ANAR) & phy_read(PHY_ANLPAR);
      ETH_MACConfigTypeDef mac;
      uint32_t speed100 = ((common & (PHY_CAP_100FD | PHY_CAP_100HD)) != 0U);
      uint32_t full     = ((common & (PHY_CAP_100FD | PHY_CAP_10FD)) != 0U);

      HAL_ETH_GetMACConfig(&heth1, &mac);
      mac.Speed      = speed100 ? ETH_SPEED_100M : ETH_SPEED_10M;
      mac.DuplexMode = full ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;
      HAL_ETH_SetMACConfig(&heth1, &mac);

      if (HAL_ETH_Start_IT(&heth1) == HAL_OK)
      {
        printf("[NET] Lien actif : %s Mb/s, %s duplex\r\n", speed100 ? "100" : "10", full ? "full" : "half");
        LOCK_TCPIP_CORE();
        netif_set_link_up(&gnetif);
        dhcp_start(&gnetif);
        UNLOCK_TCPIP_CORE();
        link_up = 1U;
      }
      else
      {
        printf("[NET] Echec HAL_ETH_Start_IT\r\n");
      }
    }
    else if (!up && link_up)
    {
      printf("[NET] Lien perdu\r\n");
      HAL_ETH_Stop_IT(&heth1);
      LOCK_TCPIP_CORE();
      dhcp_stop(&gnetif);
      netif_set_link_down(&gnetif);
      UNLOCK_TCPIP_CORE();
      link_up = 0U;
    }
    osDelay(500);
  }
}

void NET_Init(void)
{
  ip4_addr_t ip, mask, gw;
  ip4_addr_set_zero(&ip);
  ip4_addr_set_zero(&mask);
  ip4_addr_set_zero(&gw);

  memset(&tx_cfg, 0, sizeof(tx_cfg));
  tx_cfg.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  tx_cfg.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  tx_cfg.CRCPadCtrl   = ETH_CRC_PAD_INSERT;

  /* Horloges MAC / TX / RX de l'ETH1 (le code CubeMX n'active que ETH1) */
  __HAL_RCC_ETH1MAC_CLK_ENABLE();
  __HAL_RCC_ETH1TX_CLK_ENABLE();
  __HAL_RCC_ETH1RX_CLK_ENABLE();

  print_mutex = osMutexNew(NULL);
  rx_sem = osSemaphoreNew(1U, 0U, NULL);

  tcpip_init(NULL, NULL);
  printf("[NET] Pile TCP/IP lwIP initialisee\r\n");

  LOCK_TCPIP_CORE();
  netif_add(&gnetif, &ip, &mask, &gw, NULL, ethernetif_init, tcpip_input);
  netif_set_default(&gnetif);
  netif_set_status_callback(&gnetif, status_cb);
  netif_set_up(&gnetif);
  UNLOCK_TCPIP_CORE();

  HAL_NVIC_SetPriority(ETH1_IRQn, 5U, 0U);   /* >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5) */
  HAL_NVIC_EnableIRQ(ETH1_IRQn);

  const osThreadAttr_t in_attr   = { .name = "eth_in",   .stack_size = 2048, .priority = (osPriority_t)osPriorityAboveNormal };
  const osThreadAttr_t link_attr = { .name = "eth_link", .stack_size = 2048, .priority = (osPriority_t)osPriorityNormal };
  osThreadNew(input_thread, NULL, &in_attr);
  osThreadNew(link_thread,  NULL, &link_attr);
}
