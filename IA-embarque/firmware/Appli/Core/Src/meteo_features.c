/**
 * meteo_features.c - ETRS923 TP3, projet AMR & EJUB (Edge AI)
 * Voir meteo_features.h. Formules identiques au notebook Colab.
 */
#include "meteo_features.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define PI_F 3.14159265358979f

const char *const METEO_CLASSES[METEO_NB_CLASSES] = METEO_NOMS_CLASSES;

/* Colab : p * (1 - 0.0065*h / (T + 0.0065*h + 273.15)) ** -5.257 */
float meteo_niveau_mer(float p, float t, float alt)
{
  return p * powf(1.0f - 0.0065f * alt / (t + 0.0065f * alt + 273.15f), -5.257f);
}

/* Colab : Magnus, a = 17.62, b = 243.12, rh borne a [1, 100] */
float meteo_point_de_rosee(float t, float rh)
{
  const float a = 17.62f, b = 243.12f;
  if (rh < 1.0f)   rh = 1.0f;
  if (rh > 100.0f) rh = 100.0f;
  float g = logf(rh / 100.0f) + a * t / (b + t);
  return b * g / (a - g);
}

static int bissextile(int a)
{
  return ((a % 4 == 0) && (a % 100 != 0)) || (a % 400 == 0);
}

int meteo_jour_de_l_annee(int annee, int mois, int jour)
{
  static const int cumul[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  int j = cumul[mois - 1] + jour;
  if (mois > 2 && bissextile(annee)) j++;
  return j;                               /* 1..366, comme pandas dayofyear */
}

void meteo_calculer_entrees(float temp, float rhum, float p_station, uint32_t maintenant_ms,
                            const meteo_utc_t *utc, float x[METEO_NB_ENTREES], int *historique_ok)
{
  static const int liste[METEO_NB_ENTREES] = METEO_LISTE_ENTREES;
  float tout[METEO_NB_CANDIDATS];
  int   doy  = utc ? meteo_jour_de_l_annee(utc->annee, utc->mois, utc->jour) : 1;
  float hour = utc ? (float)utc->heure : 0.0f;   /* heure entiere, comme dans Colab */
  float p_mer = meteo_niveau_mer(p_station, temp, METEO_ALT);
  int ok[METEO_NB_CANDIDATS];

  for (int i = 0; i < METEO_NB_CANDIDATS; i++) ok[i] = 1;
  tout[METEO_E_TEMP]     = temp;
  tout[METEO_E_RHUM]     = rhum;
  tout[METEO_E_PRES]     = p_mer;
  tout[METEO_E_SPREAD]   = temp - meteo_point_de_rosee(temp, rhum);
  tout[METEO_E_LAT]      = METEO_LAT;
  tout[METEO_E_LON]      = METEO_LON;
  tout[METEO_E_ALT]      = METEO_ALT;
  tout[METEO_E_DOY_SIN]  = sinf(2.0f * PI_F * (float)doy / 365.25f);
  tout[METEO_E_DOY_COS]  = cosf(2.0f * PI_F * (float)doy / 365.25f);
  tout[METEO_E_HOUR_SIN] = sinf(2.0f * PI_F * hour / 24.0f);
  tout[METEO_E_HOUR_COS] = cosf(2.0f * PI_F * hour / 24.0f);
  /* Tendances = valeur actuelle - valeur il y a 1, 3 ou 6 h (historique de la carte) */
  tout[METEO_E_DPRES1H]  = meteo_tendance(maintenant_ms, METEO_VAR_PRES, p_mer, 1, &ok[METEO_E_DPRES1H]);
  tout[METEO_E_DPRES3H]  = meteo_tendance(maintenant_ms, METEO_VAR_PRES, p_mer, 3, &ok[METEO_E_DPRES3H]);
  tout[METEO_E_DPRES6H]  = meteo_tendance(maintenant_ms, METEO_VAR_PRES, p_mer, 6, &ok[METEO_E_DPRES6H]);
  tout[METEO_E_DTEMP1H]  = meteo_tendance(maintenant_ms, METEO_VAR_TEMP, temp,  1, &ok[METEO_E_DTEMP1H]);
  tout[METEO_E_DTEMP3H]  = meteo_tendance(maintenant_ms, METEO_VAR_TEMP, temp,  3, &ok[METEO_E_DTEMP3H]);
  tout[METEO_E_DRHUM1H]  = meteo_tendance(maintenant_ms, METEO_VAR_RHUM, rhum,  1, &ok[METEO_E_DRHUM1H]);
  tout[METEO_E_DRHUM3H]  = meteo_tendance(maintenant_ms, METEO_VAR_RHUM, rhum,  3, &ok[METEO_E_DRHUM3H]);

  int tout_ok = 1;
  for (int i = 0; i < METEO_NB_ENTREES; i++) {
    x[i] = tout[liste[i]];
    if (!ok[liste[i]]) tout_ok = 0;
  }
  if (historique_ok) *historique_ok = tout_ok;
}

/* ------------------------------------------------------------------------ */
/* Heure UTC : lue dans l'en-tete "Date:" des reponses de ThingSpeak         */
/* ------------------------------------------------------------------------ */
static int      heure_source = 0;         /* 0 inconnue, 1 Internet, 2 compilation */
static int64_t  ref_epoch = -1;           /* secondes depuis 1970 (UTC)      */
static uint32_t ref_ms    = 0;            /* sys_now() au moment de la lecture */

static int64_t jours_depuis_1970(int a, int m, int j)
{
  /* algorithme "days from civil" (H. Hinnant) */
  a -= (m <= 2);
  int64_t era = (a >= 0 ? a : a - 399) / 400;
  int64_t yoe = a - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + j - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void depuis_epoch(int64_t s, meteo_utc_t *u)
{
  int64_t z = s / 86400, r = s % 86400;
  if (r < 0) { r += 86400; z--; }
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp  = (5 * doy + 2) / 153;
  u->jour  = (int)(doy - (153 * mp + 2) / 5 + 1);
  u->mois  = (int)(mp < 10 ? mp + 3 : mp - 9);
  u->annee = (int)(yoe + era * 400 + (u->mois <= 2));
  u->heure   = (int)(r / 3600);
  u->minute  = (int)((r % 3600) / 60);
  u->seconde = (int)(r % 60);
}

int meteo_heure_depuis_http(const char *rep, uint32_t maintenant_ms)
{
  static const char *MOIS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = strstr(rep, "\nDate: ");
  if (d == NULL) d = strstr(rep, "\ndate: ");
  if (d == NULL) return 0;
  d += 7;                                 /* "Thu, 29 Oct 2026 22:00:25 GMT" */
  const char *v = strchr(d, ',');
  if (v == NULL) return 0;
  v += 2;
  char *fin;
  long jour = strtol(v, &fin, 10);
  if (fin == v || fin[0] != ' ') return 0;
  const char *m = strstr(MOIS, (char[4]){ fin[1], fin[2], fin[3], 0 });
  if (m == NULL) return 0;
  int mois = (int)((m - MOIS) / 3) + 1;
  long annee = strtol(fin + 5, &fin, 10);
  long h  = strtol(fin + 1, &fin, 10);
  long mi = strtol(fin + 1, &fin, 10);
  long s  = strtol(fin + 1, &fin, 10);
  if (annee < 2000 || h > 23 || mi > 59 || s > 60) return 0;

  ref_epoch = jours_depuis_1970((int)annee, mois, (int)jour) * 86400 + h * 3600 + mi * 60 + s;
  ref_ms    = maintenant_ms;
  heure_source = 1;
  return 1;
}

/* Carte hors ligne : on part de l'heure a laquelle le programme a ete compile
   (__DATE__ / __TIME__, heure du PC), convertie en UTC. Comme on flashe juste apres
   la compilation, l'erreur n'est que de quelques minutes (tant qu'on ne fait pas de reset). */
static void heure_depuis_compilation(void)
{
  static const char *MOIS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = __DATE__;               /* "Nov 19 2026" */
  const char *t = __TIME__;               /* "15:17:42"    */
  char m[4] = { d[0], d[1], d[2], 0 };
  const char *pm = strstr(MOIS, m);
  int mois  = pm ? (int)((pm - MOIS) / 3) + 1 : 1;
  int jour  = atoi(d + 4);
  int annee = atoi(d + 7);
  ref_epoch = jours_depuis_1970(annee, mois, jour) * 86400
            + atoi(t) * 3600 + atoi(t + 3) * 60 + atoi(t + 6)
            - (int64_t)METEO_DECALAGE_UTC_H * 3600;
  ref_ms = 0;                             /* = demarrage de la carte */
  heure_source = 2;
}

int meteo_heure_utc(uint32_t maintenant_ms, meteo_utc_t *utc)
{
  if (ref_epoch < 0) heure_depuis_compilation();
  depuis_epoch(ref_epoch + (int64_t)((uint32_t)(maintenant_ms - ref_ms) / 1000U), utc);
  return heure_source;
}

/* ------------------------------------------------------------------------ */
/* Historique des mesures : un point toutes les 10 min, sur 6 h 40           */
/* ------------------------------------------------------------------------ */
#define HIST_PAS_MS   (10U * 60U * 1000U)
#define HIST_N        40U
static float    hist_v[HIST_N][METEO_NB_VAR];
static uint32_t hist_t[HIST_N];
static uint32_t hist_nb = 0, hist_tete = 0;

void meteo_historique_ajouter(uint32_t maintenant_ms, float p_mer, float temp, float rhum)
{
  if (hist_nb > 0) {
    uint32_t dernier = hist_t[(hist_tete + HIST_N - 1U) % HIST_N];
    if ((uint32_t)(maintenant_ms - dernier) < HIST_PAS_MS) return;
  }
  hist_v[hist_tete][METEO_VAR_PRES] = p_mer;
  hist_v[hist_tete][METEO_VAR_TEMP] = temp;
  hist_v[hist_tete][METEO_VAR_RHUM] = rhum;
  hist_t[hist_tete] = maintenant_ms;
  hist_tete = (hist_tete + 1U) % HIST_N;
  if (hist_nb < HIST_N) hist_nb++;
}

float meteo_tendance(uint32_t maintenant_ms, int var, float actuelle, int heures, int *ok)
{
  const uint32_t cible = (uint32_t)heures * 3600U * 1000U;
  const uint32_t tol   = (heures <= 1 ? 15U : 30U) * 60U * 1000U;
  uint32_t meilleur_ecart = 0xFFFFFFFFU;
  float ancienne = 0.0f;
  for (uint32_t i = 0; i < hist_nb; i++) {
    uint32_t age = (uint32_t)(maintenant_ms - hist_t[i]);
    uint32_t ecart = (age > cible) ? age - cible : cible - age;
    if (ecart < meilleur_ecart) { meilleur_ecart = ecart; ancienne = hist_v[i][var]; }
  }
  if (meilleur_ecart < tol) { *ok = 1; return actuelle - ancienne; }
  *ok = 0;
  return 0.0f;                            /* pas encore assez d'historique */
}
