/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : app_freertos.c
  * Description        : FreeRTOS applicative file - TP2 ETRS923
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_freertos.h"
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "lps22hh_reg.h"
#include "lsm6dso_reg.h"

#include "ethernetif.h"
#include "lwip/tcpip.h"
#if __has_include("lwip/sockets.h")
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/inet_chksum.h"   /* calcul du checksum ICMP */
#include "lwip/prot/icmp.h"     /* structure d'un message ICMP echo */
#include "lwip/prot/ip4.h"      /* lecture de l'en-tete IP de la reponse */
#include "lwip/sys.h"           /* sys_now() : temps en ms */
#define HAS_LWIP_SOCKETS 1
#else
#define HAS_LWIP_SOCKETS 0
#endif
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
extern stmdev_ctx_t dev_ctx_lps;
extern stmdev_ctx_t dev_ctx_lsm;
extern int32_t platform_write(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len);
extern int32_t platform_read(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len);

#if HAS_LWIP_SOCKETS
/* ------------------------------------------------------------------
 * Vrai ping ICMP (question c du TP2 partie 3)
 * Envoie un "echo request" ICMP via un socket BSD brut (SOCK_RAW)
 * et attend l'"echo reply". Retourne le temps aller-retour en ms,
 * ou -1 en cas d'echec / timeout.
 * ------------------------------------------------------------------ */
#define PING_ID          0xAFAFU     /* identifiant de nos pings */
#define PING_DATA_SIZE   32U         /* octets de donnees dans le ping */
#define PING_TIMEOUT_MS  2000U

static int32_t icmp_ping(const struct sockaddr_in *dst, uint16_t seq, uint8_t *ttl_out)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) {
        printf("  [PING] Erreur creation socket RAW\r\n");
        return -1;
    }

    /* Timeout de reception : recvfrom() ne bloque pas plus de PING_TIMEOUT_MS */
    struct timeval tv;
    tv.tv_sec  = PING_TIMEOUT_MS / 1000U;
    tv.tv_usec = (PING_TIMEOUT_MS % 1000U) * 1000U;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* --- Construction du message ICMP "echo request" --- */
    uint8_t pkt[sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE];
    struct icmp_echo_hdr *req = (struct icmp_echo_hdr *)pkt;
    ICMPH_TYPE_SET(req, ICMP_ECHO);      /* type 8 = echo request */
    ICMPH_CODE_SET(req, 0);
    req->id     = lwip_htons(PING_ID);
    req->seqno  = lwip_htons(seq);
    for (uint32_t i = 0; i < PING_DATA_SIZE; i++) {
        pkt[sizeof(struct icmp_echo_hdr) + i] = (uint8_t)('a' + (i % 26U));
    }
    req->chksum = 0;
    req->chksum = inet_chksum(pkt, (u16_t)sizeof(pkt));

    uint32_t t0 = sys_now();
    if (sendto(s, pkt, sizeof(pkt), 0, (const struct sockaddr *)dst, sizeof(*dst)) < 0) {
        printf("  [PING] Echec envoi\r\n");
        close(s);
        return -1;
    }

    /* --- Attente de la reponse (on ignore les autres paquets ICMP) --- */
    int32_t rtt = -1;
    uint8_t buf[128];
    while ((sys_now() - t0) < PING_TIMEOUT_MS) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (len <= 0) {
            break;                                    /* timeout */
        }
        /* Un socket RAW renvoie le paquet IP complet : on saute l'en-tete IP */
        struct ip_hdr *iph = (struct ip_hdr *)buf;
        uint32_t ihl = IPH_HL_BYTES(iph);
        if ((uint32_t)len < ihl + sizeof(struct icmp_echo_hdr)) {
            continue;
        }
        struct icmp_echo_hdr *rep = (struct icmp_echo_hdr *)(buf + ihl);
        if ((ICMPH_TYPE(rep) == ICMP_ER) &&
            (rep->id == lwip_htons(PING_ID)) &&
            (rep->seqno == lwip_htons(seq))) {
            rtt = (int32_t)(sys_now() - t0);
            if (ttl_out != NULL) {
                *ttl_out = IPH_TTL(iph);
            }
            break;
        }
    }

    close(s);
    return rtt;
}
#endif

static void ping_or_query_google(void)
{
#if HAS_LWIP_SOCKETS
    static uint16_t seq = 0;

    /* 1) Resolution DNS de google.com */
    struct hostent *host_entry = gethostbyname("google.com");
    if (host_entry == NULL) {
        printf("  [NET] Echec DNS pour google.com\r\n");
        return;
    }

    /* 2) Vrai ping ICMP vers google.com */
    struct sockaddr_in ping_addr;
    memset(&ping_addr, 0, sizeof(ping_addr));
    ping_addr.sin_family = AF_INET;
    memcpy(&ping_addr.sin_addr, host_entry->h_addr, host_entry->h_length);

    char ip_txt[16];
    inet_ntoa_r(ping_addr.sin_addr, ip_txt, sizeof(ip_txt));

    uint8_t ttl = 0;
    seq++;
    int32_t rtt = icmp_ping(&ping_addr, seq, &ttl);
    if (rtt >= 0) {
        printf("  [PING] Reponse de google.com (%s) : seq=%u temps=%ld ms TTL=%u\r\n",
               ip_txt, seq, (long)rtt, ttl);
    } else {
        printf("  [PING] google.com (%s) : pas de reponse (seq=%u)\r\n", ip_txt, seq);
    }

    /* 3) Test complementaire : connexion TCP (socket BSD) sur le port 80 */
    struct sockaddr_in server_addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("  [NET] Erreur creation socket\r\n");
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(80);
    memcpy(&server_addr.sin_addr, host_entry->h_addr, host_entry->h_length);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
        printf("  [NET] Connexion reussie avec google.com:80 !\r\n");
    } else {
        printf("  [NET] Echec connexion google.com\r\n");
    }
    close(sock);
#else
    printf("  [NET] Simulation requete ICMP/Socket vers google.com (8.8.8.8)...\r\n");
#endif
}
/* USER CODE END Variables */

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = 1024 * 4 // 4 Ko pour eviter tout stack overflow
};

/* Private function prototypes -----------------------------------------------*/
void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */
}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Boucle principale de la tâche applicative
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN defaultTask */
  printf("\r\n=========================================\r\n");
  printf("  TP2 - ETRS923: FreeRTOS & Reseau ETH   \r\n");
  printf("=========================================\r\n");

  // Initialisation de lwIP + Ethernet (PHY, DHCP) sous FreeRTOS
  NET_Init();

  uint8_t whoami = 0;

  // 1. Détection Pression & Température (ID 0xB4)
  lps22hh_device_id_get(&dev_ctx_lps, &whoami);
  if (whoami == LPS22HH_ID || whoami == 0xB4) {
      printf("[OK] Capteur Pression detecte (ID: 0x%02X)\r\n", whoami);
      lps22hh_data_rate_set(&dev_ctx_lps, LPS22HH_10_Hz_LOW_NOISE);
  } else {
      printf("[ERREUR] Pression non detectee (0x%02X)\r\n", whoami);
  }

  // 2. Détection Accéléromètre (ID 0x70)
  whoami = 0;
  lsm6dso_device_id_get(&dev_ctx_lsm, &whoami);
  if (whoami == LSM6DSO_ID || whoami == 0x70) {
      printf("[OK] Accelerometre detecte (ID: 0x%02X)\r\n", whoami);
      uint8_t ctrl1_val = 0x03; // Réveil à 30 Hz
      platform_write(dev_ctx_lsm.handle, 0x10, &ctrl1_val, 1);
  } else {
      printf("[ERREUR] Accelerometre non detecte (0x%02X)\r\n", whoami);
  }

  printf("\r\n<debut d'application>\r\n");

  for(;;)
  {
      // -------------------------------------------------------------
      // 1. PHASE CAPTEURS
      // LED Rouge = ON, autres = OFF
      // -------------------------------------------------------------
      printf("<Lecture Interface Capteurs>\r\n");
      HAL_GPIO_WritePin(GPIOG, LED_RED_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(GPIOG, LED_GREEN_Pin | LED_BLUE_Pin, GPIO_PIN_RESET);

      // Pression
      uint32_t raw_pressure = 0;
      float pressure_hPa = 0.0f;
      lps22hh_pressure_raw_get(&dev_ctx_lps, &raw_pressure);
      pressure_hPa = lps22hh_from_lsb_to_hpa(raw_pressure);

      // Température
      int16_t raw_temp = 0;
      float temp_c = 0.0f;
      lps22hh_temperature_raw_get(&dev_ctx_lps, &raw_temp);
      temp_c = lps22hh_from_lsb_to_celsius(raw_temp);

      // Accélération (axes X, Y, Z)
      uint8_t raw_acc_data[6] = {0};
      int16_t accel_raw[3] = {0};
      if (platform_read(dev_ctx_lsm.handle, 0x28, raw_acc_data, 6) == 0) {
          accel_raw[0] = (int16_t)((raw_acc_data[1] << 8) | raw_acc_data[0]);
          accel_raw[1] = (int16_t)((raw_acc_data[3] << 8) | raw_acc_data[2]);
          accel_raw[2] = (int16_t)((raw_acc_data[5] << 8) | raw_acc_data[4]);
      }
      float acc_x = (float)accel_raw[0] * 0.061f;
      float acc_y = (float)accel_raw[1] * 0.061f;
      float acc_z = (float)accel_raw[2] * 0.061f;

      printf("  -> Pression: %.2f hPa | Temp: %.2f C | Accel (mg): X=%.2f Y=%.2f Z=%.2f\r\n",
             pressure_hPa, temp_c, acc_x, acc_y, acc_z);

      osDelay(1000); // Maintien 1s

      // -------------------------------------------------------------
      // 2. PHASE RÉSEAU
      // LED Bleue = ON, autres = OFF
      // -------------------------------------------------------------
      printf("<Communication Reseau>\r\n");
      HAL_GPIO_WritePin(GPIOG, LED_BLUE_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(GPIOG, LED_RED_Pin | LED_GREEN_Pin, GPIO_PIN_RESET);

      if (NET_WaitIp(5000)) {
          ping_or_query_google();
      } else {
          printf("  [NET] Pas d'adresse IP (cable / DHCP ?)\r\n");
      }

      osDelay(1000); // Maintien 1s

      // -------------------------------------------------------------
      // 3. PHASE ATTENTE / DISPONIBLE
      // LED Verte = ON, autres = OFF
      // -------------------------------------------------------------
      HAL_GPIO_WritePin(GPIOG, LED_GREEN_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(GPIOG, LED_RED_Pin | LED_BLUE_Pin, GPIO_PIN_RESET);

      osDelay(2000); // Pause 2s avant le cycle suivant
  }
  /* USER CODE END defaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
/* USER CODE END Application */
