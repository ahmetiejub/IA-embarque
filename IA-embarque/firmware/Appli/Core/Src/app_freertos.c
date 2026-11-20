/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : app_freertos.c
  * Description        : ETRS923 - Projet AMR & EJUB : station meteo IA embarquee
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
#include <stdlib.h>
#include <math.h>
#include "lps22hh_reg.h"
#include "meteo_features.h"     /* TP3 partie 3 : calcul des 12 entrees */
#include "meteo_reseau.h"       /* TP3 partie 3 : reseau meteo.onnx en C */
#include "web_serveur.h"        /* page web de la carte */
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
#include "thingspeak_keys.h"    /* cles API ThingSpeak (fichier non publie sur GitHub) */
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
        printf("  [NET] Echec DNS pour google.com -> test sans DNS : ping de 8.8.8.8\r\n");
        struct sockaddr_in a8;
        memset(&a8, 0, sizeof(a8));
        a8.sin_family = AF_INET;
        a8.sin_addr.s_addr = inet_addr("8.8.8.8");
        uint8_t ttl8 = 0;
        int32_t rtt8 = icmp_ping(&a8, ++seq, &ttl8);
        if (rtt8 >= 0) {
            printf("  [PING] 8.8.8.8 repond (%ld ms) : Internet OK, seul le DNS bloque\r\n", (long)rtt8);
        } else {
            printf("  [PING] 8.8.8.8 ne repond pas : pas d'acces Internet par cette passerelle\r\n");
            NET_InternetKo();
        }
        return;
    }

    /* 2) Vrai ping ICMP vers google.com */
    struct sockaddr_in ping_addr;
    memset(&ping_addr, 0, sizeof(ping_addr));
    ping_addr.sin_family = AF_INET;
    memcpy(&ping_addr.sin_addr, host_entry->h_addr, host_entry->h_length);

    char ip_txt[16];
    inet_ntoa_r(ping_addr.sin_addr, ip_txt, sizeof(ip_txt));

    NET_InternetOk();                     /* le DNS a repondu : Internet accessible */
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

/* ==================================================================
 * TP3 : humidite (SHT40 de la carte capteurs X-NUCLEO-IKS4A1)
 * Le SHT40 n'a pas de registres : on lui envoie une commande,
 * on attend la mesure, puis on lit 6 octets (T + CRC, RH + CRC).
 * ================================================================== */
extern I2C_HandleTypeDef hi2c1;
#define SHT40_ADDR       (0x44U << 1)   /* adresse 7 bits 0x44, decalee pour la HAL */
#define SHT40_CMD_MESURE 0xFDU          /* mesure T + RH, haute precision (~9 ms)  */

/* CRC-8 Sensirion : polynome 0x31, valeur initiale 0xFF */
static uint8_t sht40_crc(const uint8_t *d)
{
    uint8_t crc = 0xFFU;
    for (int i = 0; i < 2; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Retourne 1 si la lecture est valide, 0 sinon */
static int sht40_lire(float *temp_c, float *rhum)
{
    uint8_t cmd = SHT40_CMD_MESURE;
    uint8_t r[6];
    if (HAL_I2C_Master_Transmit(&hi2c1, SHT40_ADDR, &cmd, 1, 100) != HAL_OK) {
        return 0;
    }
    osDelay(10);                                    /* temps de mesure du capteur */
    if (HAL_I2C_Master_Receive(&hi2c1, SHT40_ADDR, r, 6, 100) != HAL_OK) {
        return 0;
    }
    if (sht40_crc(&r[0]) != r[2] || sht40_crc(&r[3]) != r[5]) {
        return 0;                                   /* transmission abimee */
    }
    uint16_t t_raw  = (uint16_t)((r[0] << 8) | r[1]);
    uint16_t rh_raw = (uint16_t)((r[3] << 8) | r[4]);
    *temp_c = -45.0f + 175.0f * (float)t_raw  / 65535.0f;   /* formules de la datasheet */
    *rhum   =  -6.0f + 125.0f * (float)rh_raw / 65535.0f;
    if (*rhum < 0.0f)   *rhum = 0.0f;
    if (*rhum > 100.0f) *rhum = 100.0f;
    return 1;
}

#if HAS_LWIP_SOCKETS
/* ==================================================================
 * TP3 : client HTTP minimal (socket BSD TCP) pour ThingSpeak
 * ================================================================== */
#define TS_HOST       "api.thingspeak.com"
#define HTTP_TIMEOUT  5000U                         /* ms */
static char http_req[512];                          /* buffers statiques : pas sur la pile */
static char http_rep[3072];                        /* les en-tetes de ThingSpeak font ~1 Ko */

/* Envoie http_req au serveur "hote" (port 80) et met la reponse dans http_rep.
   Retourne un pointeur sur le corps de la reponse, ou NULL en cas d'echec. */
static const char *http_envoyer_hote(const char *hote)
{
    struct hostent *h = gethostbyname(hote);
    if (h == NULL) {
        printf("  [HTTP] Echec DNS %s\r\n", hote);
        return NULL;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("  [HTTP] Erreur creation socket\r\n");
        return NULL;
    }
    struct timeval tv = { .tv_sec = HTTP_TIMEOUT / 1000U, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(80);
    memcpy(&a.sin_addr, h->h_addr, h->h_length);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("  [HTTP] Echec connexion %s\r\n", hote);
        close(s);
        return NULL;
    }
    send(s, http_req, strlen(http_req), 0);

    /* "Connection: close" : le serveur ferme la connexion a la fin de la reponse */
    size_t n = 0;
    int r;
    while (n < sizeof(http_rep) - 1 &&
           (r = recv(s, http_rep + n, sizeof(http_rep) - 1 - n, 0)) > 0) {
        n += (size_t)r;
    }
    http_rep[n] = '\0';
    meteo_heure_depuis_http(http_rep, sys_now());   /* TP3 : heure UTC lue dans l'en-tete Date */
    if (n == sizeof(http_rep) - 1) {               /* reponse trop longue : on vide le reste */
        char poubelle[128];
        while (recv(s, poubelle, sizeof(poubelle), 0) > 0) { }
    }
    close(s);

    char *corps = strstr(http_rep, "\r\n\r\n");
    if (corps != NULL) {
        *corps = '\0';                 /* coupe : http_rep = en-tetes, corps = contenu */
        corps += 4;
    }

    /* Corps envoye "par morceaux" (HTTP/1.1 chunked) : on recolle les morceaux */
    if (corps != NULL && strstr(http_rep, "chunked") != NULL) {
        char *lec = corps, *ecr = corps;
        for (;;) {
            char *fin_taille;
            long taille = strtol(lec, &fin_taille, 16);
            if (taille <= 0 || fin_taille == lec) break;
            lec = strstr(fin_taille, "\r\n");
            if (lec == NULL) break;
            lec += 2;
            memmove(ecr, lec, (size_t)taille);
            ecr += taille;
            lec += taille + 2;           /* saute les donnees + "\r\n" */
            if (lec >= http_rep + n) break;
        }
        *ecr = '\0';
    }

    if (strncmp(http_rep, "HTTP/1.", 7) != 0 || strstr(http_rep, " 200 ") == NULL) {
        char *fin_ligne = strstr(http_rep, "\r\n");
        int lg = (fin_ligne != NULL) ? (int)(fin_ligne - http_rep) : 40;
        printf("  [HTTP] Erreur : %.*s\r\n", lg, http_rep);          /* ex : HTTP/1.1 401 ... */
        if (corps != NULL && corps[0] != '\0') {
            printf("  [HTTP] Detail : %.150s\r\n", corps);           /* message de ThingSpeak */
        }
        return NULL;
    }
    if (corps == NULL) {
        printf("  [HTTP] Reponse 200 recue mais sans contenu lisible (%u octets)\r\n", (unsigned)n);
    }
    return corps;
}

/* Requete vers ThingSpeak */
static const char *http_envoyer(void)
{
    return http_envoyer_hote(TS_HOST);
}

/* 2.c : envoie T, RH et pression STATION au canal des mesures (field1..3) */
/* Valeurs definies plus bas (resultats Edge et verification), publiees aussi sur ThingSpeak */
static int derniere_classe_edge;
static float derniere_proba_edge;
static int derniere_classe_reelle = -1;
static unsigned nb_verif, nb_correct;

/* Envoi a ThingSpeak (canal 3546450) :
   field1..3 = T, RH, P station (mesures)        -> classification Cloud (MATLAB)
   field4 = classe Edge (0 Neige, 1 Nuageux, 2 Pluie, 3 Soleil), field5 = confiance Edge (%)
   field6 = vraie meteo (meme codage), field7 = score Edge / vraie meteo (%)  -> page publique */
static void thingspeak_envoyer_mesures(float t, float rh, float p_station)
{
    char extra[96] = "";
    int n = 0;
    if (derniere_classe_edge >= 0) {
        n += snprintf(extra + n, sizeof(extra) - (size_t)n, "&field4=%d&field5=%.1f",
                      derniere_classe_edge, 100.0f * derniere_proba_edge);
    }
    if (derniere_classe_reelle >= 0) {
        n += snprintf(extra + n, sizeof(extra) - (size_t)n, "&field6=%d", derniere_classe_reelle);
    }
    if (nb_verif > 0) {
        snprintf(extra + n, sizeof(extra) - (size_t)n, "&field7=%.0f",
                 100.0f * (float)nb_correct / (float)nb_verif);
    }
    snprintf(http_req, sizeof(http_req),
             "GET /update?api_key=%s&field1=%.2f&field2=%.2f&field3=%.2f%s HTTP/1.1\r\n"
             "Host: " TS_HOST "\r\n"
             "User-Agent: STM32N6-ETRS923\r\n"
             "Connection: close\r\n\r\n",
             TS_WRITE_KEY, t, rh, p_station, extra);
    printf("  [ThingSpeak] Envoi des mesures au canal 3546450...\r\n");
    const char *corps = http_envoyer();
    if (corps == NULL) {
        printf("  [ThingSpeak] Echec : verifier TS_WRITE_KEY (Write API Key du canal 3546450)\r\n");
        return;
    }
    long entree = strtol(corps, NULL, 10);
    if (entree > 0) {
        printf("  [ThingSpeak] Mesures envoyees (entree n.%ld) : T=%.2f C  RH=%.1f %%  P=%.2f hPa\r\n",
               entree, t, rh, p_station);
    } else {
        printf("  [ThingSpeak] Envoi refuse (cle API ? moins de 15 s depuis le dernier envoi ?)\r\n");
    }
}

/* 2.d : lit (et consomme) la prochaine commande TalkBack ecrite par MATLAB */
static int derniere_classe_edge = -1;   /* derniere classe calculee sur la carte (3.c) */

static void talkback_lire(void)
{
    const char *body = "api_key=" TS_TALKBACK_KEY;
    snprintf(http_req, sizeof(http_req),
             "POST /talkbacks/" TS_TALKBACK_ID "/commands/execute.json?api_key=" TS_TALKBACK_KEY " HTTP/1.1\r\n"
             "Host: " TS_HOST "\r\n"
             "User-Agent: STM32N6-ETRS923\r\n"
             "Connection: close\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %u\r\n\r\n%s",
             (unsigned)strlen(body), body);
    printf("  [TalkBack] Lecture de la commande en attente (TalkBack " TS_TALKBACK_ID ")...\r\n");
    const char *corps = http_envoyer();
    if (corps == NULL) {
        return;
    }
    /* Reponse JSON : {"id":..., "command_string":"Pluie 84%", ...}  ou vide s'il n'y a rien */
    const char *cle = "\"command_string\":\"";
    const char *deb = strstr(corps, cle);
    if (deb == NULL) {
        printf("  [TalkBack] Aucune nouvelle classification en attente\r\n");
        return;
    }
    deb += strlen(cle);
    const char *fin = strchr(deb, '"');
    int len = (fin != NULL) ? (int)(fin - deb) : (int)strlen(deb);
    printf("<Resultat Cloud> Classification meteo : %.*s\r\n", len, deb);
    WEB_Cloud(deb, len);

    /* 3.c : comparaison avec la classification faite sur la carte */
    if (derniere_classe_edge >= 0) {
        const char *edge = METEO_CLASSES[derniere_classe_edge];
        int identique = ((int)strlen(edge) == len) && (strncmp(edge, deb, (size_t)len) == 0);
        printf("<Comparaison> Cloud : %.*s | Edge : %s -> %s\r\n", len, deb, edge,
               identique ? "IDENTIQUES" : "differents");
    }
}

/* ==================================================================
 * Verification avec la VRAIE meteo en ligne (Open-Meteo, gratuit, sans cle)
 * Meteo actuelle au Bourget-du-Lac -> convertie dans nos 4 classes, puis
 * comparee a la classe trouvee par la carte. Le score s'accumule.
 * ================================================================== */
#define OM_HOST  "api.open-meteo.com"
static float derniere_proba_edge = 0.0f;
static unsigned nb_verif = 0, nb_correct = 0;

/* Code meteo WMO (Open-Meteo) -> nom de nos classes */
static const char *wmo_vers_classe(int code)
{
    if (code <= 1)                                    return "Soleil";   /* 0 clair, 1 peu nuageux  */
    if (code <= 3 || code == 45 || code == 48)        return "Nuageux";  /* 2-3 nuages, 45-48 brouillard */
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Neige";
    return "Pluie";                                   /* bruine, pluie, averses, orage */
}

static int classe_index(const char *nom)
{
    for (int k = 0; k < METEO_NB_CLASSES; k++) {
        if (strcmp(METEO_CLASSES[k], nom) == 0) return k;
    }
    return -1;
}

/* Lit le nombre qui suit "cle" dans le texte (NAN si absent) */
static float json_nombre(const char *txt, const char *cle)
{
    const char *p = strstr(txt, cle);
    if (p == NULL) return NAN;
    return strtof(p + strlen(cle), NULL);
}

static void meteo_reelle_verifier(void)
{
    snprintf(http_req, sizeof(http_req),
             "GET /v1/forecast?latitude=%.2f&longitude=%.2f"
             "&current=weather_code,temperature_2m,relative_humidity_2m,pressure_msl HTTP/1.1\r\n"
             "Host: " OM_HOST "\r\n"
             "User-Agent: STM32N6-ETRS923\r\n"
             "Connection: close\r\n\r\n",
             (double)METEO_LAT, (double)METEO_LON);
    printf("  [Open-Meteo] Lecture de la meteo reelle au Bourget-du-Lac...\r\n");
    const char *corps = http_envoyer_hote(OM_HOST);
    if (corps == NULL) return;

    /* {"...","current_units":{...},"current":{"time":"...","weather_code":3,"temperature_2m":8.1,...}} */
    const char *cur = strstr(corps, "\"current\":{");
    if (cur == NULL) {
        printf("  [Open-Meteo] Reponse inattendue : %.120s\r\n", corps);
        return;
    }
    float code = json_nombre(cur, "\"weather_code\":");
    float t    = json_nombre(cur, "\"temperature_2m\":");
    float rh   = json_nombre(cur, "\"relative_humidity_2m\":");
    float pmer = json_nombre(cur, "\"pressure_msl\":");
    if (isnan(code)) {
        printf("  [Open-Meteo] Code meteo absent de la reponse\r\n");
        return;
    }
    const char *reelle = wmo_vers_classe((int)code);
    printf("<Meteo reelle> Open-Meteo : %s (code WMO %d) | dehors : T=%.1f C  RH=%.0f %%  P mer=%.1f hPa\r\n",
           reelle, (int)code, t, rh, pmer);

    derniere_classe_reelle = classe_index(reelle);
    WEB_Reelle(reelle, (int)code, t, rh, pmer, nb_correct, nb_verif);
    if (derniere_classe_edge < 0 || classe_index(reelle) < 0) {
        return;                         /* pas encore d'inference sur la carte */
    }
    const char *edge = METEO_CLASSES[derniere_classe_edge];
    int correct = (strcmp(edge, reelle) == 0);
    nb_verif++;
    if (correct) nb_correct++;
    printf("<Verification> Edge : %s (%.1f %%) | Reelle : %s -> %s   [score : %u/%u = %.0f %%]\r\n",
           edge, 100.0f * derniere_proba_edge, reelle, correct ? "CORRECT" : "FAUX",
           nb_correct, nb_verif, 100.0f * (float)nb_correct / (float)nb_verif);
    WEB_Reelle(reelle, (int)code, t, rh, pmer, nb_correct, nb_verif);
    WEB_Verification(edge, derniere_proba_edge, reelle, correct);
}
#endif

/* ==================================================================
 * TP3 partie 3 : inference EDGE (le reseau tourne sur le Cortex-M55)
 * ================================================================== */

/* Compteur de cycles du processeur (DWT) : mesure precise du temps */
static void chrono_init(void)
{
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Inference chronometree : moyenne sur N executions. Retourne la classe. */
static int inference_chronometree(const float x[METEO_NB_ENTREES], float proba[METEO_NB_CLASSES],
                                  float *duree_us)
{
    const uint32_t N = 10;                /* moyenne sur 10 inferences (~55 ms de calcul) */
    int classe = 0;
    uint32_t t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) {
        classe = meteo_reseau_inferer(x, proba);
    }
    uint32_t cycles = (DWT->CYCCNT - t0) / N;
    *duree_us = (float)cycles * 1e6f / (float)SystemCoreClock;
    return classe;
}

/* 3.b : test sur deux echantillons connus de echantillons_test.csv */
static void test_echantillons_connus(void)
{
    static const float test1[METEO_NB_ENTREES] = METEO_TEST1_ENTREES;
    static const float test2[METEO_NB_ENTREES] = METEO_TEST2_ENTREES;
    float proba[METEO_NB_CLASSES], us;
    int k;

    printf("\r\n<Test 3.b> Reseau meteo embarque (CPU a %lu MHz)\r\n",
           (unsigned long)(SystemCoreClock / 1000000U));
    printf("  Modele : %s\r\n", METEO_MODELE_DESCRIPTION);
    k = inference_chronometree(test1, proba, &us);
    printf("  test 1 -> %s %.1f %%  (attendu : %s)  en %.1f us\r\n",
           METEO_CLASSES[k], 100.0f * proba[k], METEO_TEST1_ATTENDU, us);
    k = inference_chronometree(test2, proba, &us);
    printf("  test 2 -> %s %.1f %%  (attendu : %s)  en %.1f us\r\n",
           METEO_CLASSES[k], 100.0f * proba[k], METEO_TEST2_ATTENDU, us);
}

/* 3.c : classification des mesures reelles des capteurs */
static void inference_edge(float temp, float rhum, float p_station)
{
    uint32_t maintenant = sys_now();
    float p_mer = meteo_niveau_mer(p_station, temp, METEO_ALT);
    meteo_historique_ajouter(maintenant, p_mer, temp, rhum);

    /* Heure UTC (seulement si le modele s'en sert) : Internet, sinon heure de compilation */
    meteo_utc_t utc;
    int source_heure = meteo_heure_utc(maintenant, &utc);
    int heure_ok = (source_heure != 0);

    float x[METEO_NB_ENTREES], proba[METEO_NB_CLASSES], us;
    int hist_ok;
    meteo_calculer_entrees(temp, rhum, p_station, maintenant, heure_ok ? &utc : NULL, x, &hist_ok);
    int k = inference_chronometree(x, proba, &us);
#if HAS_LWIP_SOCKETS
    derniere_classe_edge = k;
    derniere_proba_edge  = proba[k];
#endif
    int ok3h;
    float dp3 = meteo_tendance(maintenant, METEO_VAR_PRES, p_mer, 3, &ok3h);

    char heure_txt[24] = "";
    if (heure_ok) {
        snprintf(heure_txt, sizeof(heure_txt), "%02d/%02d %02d:%02d%s", utc.jour, utc.mois,
                 utc.heure, utc.minute, source_heure == 1 ? "" : " (approx.)");
    }
    WEB_Edge(k, proba, us, p_mer, dp3, hist_ok, heure_txt);

    printf("<Inference Edge> %s (%.1f %%) en %.1f us | T=%.1f C  RH=%.0f %%  P mer=%.1f hPa  dpres3h=%.1f\r\n",
           METEO_CLASSES[k], 100.0f * proba[k], us, temp, rhum, p_mer, dp3);
    if (!hist_ok) {
        printf("                 (historique incomplet : tendances a 0 pendant les %d premieres heures)\r\n",
               METEO_HISTORIQUE_H);
    }
    if (METEO_BESOIN_HEURE) {
        printf("                 heure utilisee : %02d/%02d %02d:%02d UTC (%s)\r\n",
               utc.jour, utc.mois, utc.heure, utc.minute,
               source_heure == 1 ? "lue sur Internet" : "approx. : heure de compilation, carte hors ligne");
    }
}
/* USER CODE END Variables */

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = 1024 * 8 // 6 Ko : DNS + sockets + printf de flottants (tas FreeRTOS = 32 Ko)
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
  printf("  ETRS923 - Projet AMR & EJUB : station meteo IA embarquee\r\n");
  printf("=========================================\r\n");

  // Initialisation de lwIP + Ethernet (PHY, DHCP) sous FreeRTOS
  NET_Init();
  WEB_Demarrer();                       /* page web : http://<IP de la carte>/ */

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

  chrono_init();
  test_echantillons_connus();          /* TP3 question 3.b */

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

      // Humidite + temperature (SHT40)
      float temp_sht = 0.0f, rhum = 0.0f;
      int sht_ok = sht40_lire(&temp_sht, &rhum);
      /* Cable branche mais DHCP en cours : on laisse le reseau obtenir son adresse
         (30 s max) avant de lancer l'IA. Sans cable, l'IA tourne tout de suite. */
      static uint32_t debut_lien = 0;
      if (!NET_LinkUp()) debut_lien = 0;
      else if (debut_lien == 0) debut_lien = sys_now() | 1U;
      int attente_dhcp = NET_LinkUp() && !NET_HasIp() && (sys_now() - debut_lien) < 30000U;

      if (sht_ok) {
          printf("  -> Humidite: %.1f %%RH | Temp SHT40: %.2f C\r\n", rhum, temp_sht);
          {
              const float acc_mg[3] = { acc_x, acc_y, acc_z };
              WEB_Capteurs(temp_sht, rhum, pressure_hPa, acc_mg);
          }
          if (attente_dhcp) {
              printf("  [Edge] Cable branche : attente de l'adresse IP avant l'inference (30 s max)\r\n");
          } else {
              inference_edge(temp_sht, rhum, pressure_hPa);   /* TP3 question 3.c */
          }
      } else {
          printf("  -> [ERREUR] Lecture SHT40 (humidite) impossible\r\n");
      }

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
#if HAS_LWIP_SOCKETS
          // TP3 : envoi a ThingSpeak toutes les 20 s (plan gratuit : 15 s minimum)
          static uint32_t dernier_envoi = 0;
          if (sht_ok && (dernier_envoi == 0 || (sys_now() - dernier_envoi) >= 20000U)) {
              dernier_envoi = sys_now();
              thingspeak_envoyer_mesures(temp_sht, rhum, pressure_hPa);
              talkback_lire();
          }
          // Verification avec la vraie meteo en ligne, toutes les 60 s
          static uint32_t derniere_verif = 0;
          if (sht_ok && (derniere_verif == 0 || (sys_now() - derniere_verif) >= 60000U)) {
              derniere_verif = sys_now();
              meteo_reelle_verifier();
          }
#endif
      } else {
          printf("  [NET] Pas d'adresse IP (cable / DHCP ?) : la carte continue en mode Edge seul\r\n");
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
/* Diagnostic : FreeRTOS appelle ces fonctions si une tache deborde de sa pile
   ou si la memoire FreeRTOS est pleine (message envoye directement sur l'UART) */
extern UART_HandleTypeDef huart1;
static void message_urgence(const char *m)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)m, (uint16_t)strlen(m), HAL_MAX_DELAY);
}
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    message_urgence("\r\n[ERREUR] Debordement de pile de la tache : ");
    message_urgence(pcTaskName);
    message_urgence("\r\n");
    for (;;) { }
}
void vApplicationMallocFailedHook(void)
{
    message_urgence("\r\n[ERREUR] Memoire FreeRTOS pleine (configTOTAL_HEAP_SIZE)\r\n");
    for (;;) { }
}
/* USER CODE END Application */
