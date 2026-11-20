/**
 * web_serveur.c - ETRS923, projet AMR & EJUB : serveur web de la carte (port 80)
 *
 *   GET /          -> la page (Web/page.html, compilee dans web_page.h)
 *   GET /data.json -> les dernieres valeurs, lues par la page toutes les 3 s
 *
 * Tache FreeRTOS separee (priorite basse) : la page ne ralentit ni les mesures
 * ni l'IA. Les valeurs sont partagees avec defaultTask via une section critique.
 */
#include "web_serveur.h"
#include "main.h"
#include "web_page.h"
#include "meteo_features.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Valeurs partagees                                                      */
/* ---------------------------------------------------------------------- */
#define HIST_N 90                      /* ~12 min de courbes (1 point par mesure) */
#define VERIF_N 20                     /* journal des 20 dernieres verifications */

typedef struct {
  float temp, rhum, p_station, acc[3];
  int   classe;
  float proba[METEO_NB_CLASSES], duree_us, p_mer, dpres3h;
  int   hist_ok;
  char  heure[24];
  char  cloud[20];
  char  reelle[20];
  int   code_wmo;
  float t_ext, rh_ext, p_ext;
  unsigned nb_correct, nb_verif, nb_mesures;
  float h_t[HIST_N], h_rh[HIST_N], h_p[HIST_N];
  int   h_nb, h_tete;
  struct { char heure[8]; char edge[12]; char reel[12]; float proba; int ok; } v[VERIF_N];
  int   v_nb, v_tete;
} web_etat_t;

static web_etat_t etat = { .classe = -1 };

void WEB_Capteurs(float temp, float rhum, float p_station, const float acc_mg[3])
{
  taskENTER_CRITICAL();
  etat.temp = temp; etat.rhum = rhum; etat.p_station = p_station;
  etat.acc[0] = acc_mg[0]; etat.acc[1] = acc_mg[1]; etat.acc[2] = acc_mg[2];
  taskEXIT_CRITICAL();
}

void WEB_Edge(int classe, const float *proba, float duree_us, float p_mer, float dpres3h,
              int historique_ok, const char *heure_utc)
{
  taskENTER_CRITICAL();
  etat.classe = classe;
  for (int k = 0; k < METEO_NB_CLASSES; k++) etat.proba[k] = proba[k];
  etat.duree_us = duree_us; etat.p_mer = p_mer; etat.dpres3h = dpres3h;
  etat.hist_ok = historique_ok;
  strncpy(etat.heure, heure_utc ? heure_utc : "", sizeof(etat.heure) - 1);
  etat.h_t[etat.h_tete] = etat.temp;
  etat.h_rh[etat.h_tete] = etat.rhum;
  etat.h_p[etat.h_tete] = p_mer;
  etat.h_tete = (etat.h_tete + 1) % HIST_N;
  if (etat.h_nb < HIST_N) etat.h_nb++;
  etat.nb_mesures++;
  taskEXIT_CRITICAL();
}

void WEB_Cloud(const char *classe, int longueur)
{
  if (longueur < 0) longueur = 0;
  if (longueur > (int)sizeof(etat.cloud) - 1) longueur = (int)sizeof(etat.cloud) - 1;
  taskENTER_CRITICAL();
  memcpy(etat.cloud, classe, (size_t)longueur);
  etat.cloud[longueur] = '\0';
  taskEXIT_CRITICAL();
}

void WEB_Reelle(const char *classe, int code_wmo, float t, float rh, float p_mer,
                unsigned nb_correct, unsigned nb_verif)
{
  taskENTER_CRITICAL();
  strncpy(etat.reelle, classe, sizeof(etat.reelle) - 1);
  etat.code_wmo = code_wmo;
  etat.t_ext  = (t == t) ? t : 0.0f;          /* NaN (valeur absente) -> 0 : JSON valide */
  etat.rh_ext = (rh == rh) ? rh : 0.0f;
  etat.p_ext  = (p_mer == p_mer) ? p_mer : 0.0f;
  etat.nb_correct = nb_correct; etat.nb_verif = nb_verif;
  taskEXIT_CRITICAL();
}

void WEB_Verification(const char *classe_edge, float proba_edge, const char *classe_reelle, int correct)
{
  meteo_utc_t u;
  char h[8] = "--:--";
  if (meteo_heure_utc(sys_now(), &u)) snprintf(h, sizeof(h), "%02d:%02d", u.heure, u.minute);
  taskENTER_CRITICAL();
  int i = etat.v_tete;
  strncpy(etat.v[i].heure, h, sizeof(etat.v[i].heure) - 1);
  strncpy(etat.v[i].edge, classe_edge, sizeof(etat.v[i].edge) - 1);
  strncpy(etat.v[i].reel, classe_reelle, sizeof(etat.v[i].reel) - 1);
  etat.v[i].proba = proba_edge;
  etat.v[i].ok = correct;
  etat.v_tete = (etat.v_tete + 1) % VERIF_N;
  if (etat.v_nb < VERIF_N) etat.v_nb++;
  taskEXIT_CRITICAL();
}

/* ---------------------------------------------------------------------- */
/* JSON                                                                   */
/* ---------------------------------------------------------------------- */
static char json[6144];
static web_etat_t copie;

#define AJOUT(...) do { if (n < (int)sizeof(json)) n += snprintf(json + n, sizeof(json) - (size_t)n, __VA_ARGS__); } while (0)

static void ajout_courbe(int *pn, const char *nom, const float *v)
{
  int n = *pn;
  AJOUT("\"%s\":[", nom);
  for (int i = 0; i < copie.h_nb; i++) {
    int idx = (copie.h_tete - copie.h_nb + i + HIST_N) % HIST_N;
    AJOUT("%s%.1f", i ? "," : "", (double)v[idx]);
  }
  AJOUT("]");
  *pn = n;
}

static int construire_json(void)
{
  taskENTER_CRITICAL();
  copie = etat;
  taskEXIT_CRITICAL();

  char ip[16] = "0.0.0.0";
  if (netif_default != NULL) ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip, sizeof(ip));

  int n = 0;
  AJOUT("{\"modele\":\"%s\",\"classes\":[", METEO_MODELE_DESCRIPTION);
  for (int k = 0; k < METEO_NB_CLASSES; k++) AJOUT("%s\"%s\"", k ? "," : "", METEO_CLASSES[k]);
  AJOUT("],\"ip\":\"%s\",\"uptime\":%lu,\"cpu_mhz\":%lu,\"n\":%u,", ip,
        (unsigned long)(sys_now() / 1000U), (unsigned long)(SystemCoreClock / 1000000U), copie.nb_mesures);
  AJOUT("\"capteurs\":{\"temp\":%.2f,\"rhum\":%.1f,\"p_station\":%.2f,\"p_mer\":%.2f,\"dpres3h\":%.2f,"
        "\"acc\":[%.0f,%.0f,%.0f]},",
        (double)copie.temp, (double)copie.rhum, (double)copie.p_station, (double)copie.p_mer,
        (double)copie.dpres3h, (double)copie.acc[0], (double)copie.acc[1], (double)copie.acc[2]);
  AJOUT("\"edge\":{\"classe\":\"%s\",\"proba\":[",
        copie.classe >= 0 ? METEO_CLASSES[copie.classe] : "");
  for (int k = 0; k < METEO_NB_CLASSES; k++) AJOUT("%s%.4f", k ? "," : "", (double)copie.proba[k]);
  AJOUT("],\"us\":%.1f,\"hist_ok\":%d,\"heure\":\"%s\"},", (double)copie.duree_us, copie.hist_ok, copie.heure);
  AJOUT("\"cloud\":\"%s\",\"reelle\":{\"classe\":\"%s\",\"code\":%d,\"t\":%.1f,\"rh\":%.0f,\"p\":%.1f},"
        "\"score\":[%u,%u],\"hist\":{",
        copie.cloud, copie.reelle, copie.code_wmo, (double)copie.t_ext, (double)copie.rh_ext,
        (double)copie.p_ext, copie.nb_correct, copie.nb_verif);
  ajout_courbe(&n, "t", copie.h_t);  AJOUT(",");
  ajout_courbe(&n, "rh", copie.h_rh); AJOUT(",");
  ajout_courbe(&n, "p", copie.h_p);
  AJOUT("},\"verifs\":[");             /* plus recente en premier */
  for (int i = 0; i < copie.v_nb; i++) {
    int idx = (copie.v_tete - 1 - i + VERIF_N) % VERIF_N;
    AJOUT("%s{\"h\":\"%s\",\"e\":\"%s\",\"pe\":%.3f,\"r\":\"%s\",\"ok\":%d}", i ? "," : "",
          copie.v[idx].heure, copie.v[idx].edge, (double)copie.v[idx].proba, copie.v[idx].reel, copie.v[idx].ok);
  }
  AJOUT("]}");
  if (n >= (int)sizeof(json)) n = (int)sizeof(json) - 1;
  return n;
}

/* ---------------------------------------------------------------------- */
/* Serveur HTTP                                                           */
/* ---------------------------------------------------------------------- */
static void envoyer_tout(int s, const char *d, int len)
{
  while (len > 0) {
    int r = lwip_send(s, d, (size_t)len, 0);
    if (r <= 0) return;
    d += r; len -= r;
  }
}

static void repondre(int s, const char *type, const char *corps, int len, const char *statut)
{
  char ent[200];
  int n = snprintf(ent, sizeof(ent),
                   "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n", statut, type, len);
  envoyer_tout(s, ent, n);
  envoyer_tout(s, corps, len);
}

static void tache_web(void *arg)
{
  (void)arg;
  static char req[512];
  int ecoute = -1;

  for (;;) {
    if (ecoute < 0) {
      ecoute = lwip_socket(AF_INET, SOCK_STREAM, 0);
      struct sockaddr_in a;
      memset(&a, 0, sizeof(a));
      a.sin_family = AF_INET;
      a.sin_port = lwip_htons(80);
      a.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
      if (ecoute < 0 || lwip_bind(ecoute, (struct sockaddr *)&a, sizeof(a)) < 0 ||
          lwip_listen(ecoute, 4) < 0) {
        if (ecoute >= 0) lwip_close(ecoute);
        ecoute = -1;
        osDelay(2000);
        continue;
      }
    }

    int c = lwip_accept(ecoute, NULL, NULL);
    if (c < 0) { osDelay(100); continue; }

    /* Les navigateurs ouvrent souvent une 2e connexion "en avance" sans rien envoyer :
       sans delai maximal, le serveur l'attendrait pour toujours et la page ne chargerait jamais.
       lwIP attend une struct timeval (pas un entier). */
    struct timeval delai = { .tv_sec = 1, .tv_usec = 0 };
    lwip_setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &delai, sizeof(delai));
    int n = lwip_recv(c, req, sizeof(req) - 1, 0);
    if (n > 0) {
      req[n] = '\0';
      if (strncmp(req, "GET /data.json", 14) == 0) {
        int len = construire_json();
        repondre(c, "application/json", json, len, "200 OK");
      } else if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /index.html", 15) == 0) {
        printf("[WEB] Page envoyee a un navigateur\r\n");
        repondre(c, "text/html; charset=utf-8", WEB_PAGE_HTML, (int)sizeof(WEB_PAGE_HTML) - 1, "200 OK");
      } else {
        static const char introuvable[] = "Page introuvable";
        repondre(c, "text/plain", introuvable, (int)sizeof(introuvable) - 1, "404 Not Found");
      }
    }
    lwip_close(c);
  }
}

void WEB_Demarrer(void)
{
  const osThreadAttr_t attr = { .name = "web", .stack_size = 4096,
                                .priority = (osPriority_t)osPriorityBelowNormal };
  osThreadNew(tache_web, NULL, &attr);
}
