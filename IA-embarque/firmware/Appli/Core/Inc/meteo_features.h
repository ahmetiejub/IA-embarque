/**
 * meteo_features.h - ETRS923 TP3, projet AMR & EJUB (Edge AI)
 *
 * Calcul, sur la carte, des entrees du reseau meteo a partir des mesures des
 * capteurs. Les formules sont IDENTIQUES a celles du notebook Colab et du
 * script MATLAB : sinon le reseau recevrait des entrees differentes de celles
 * de l'entrainement.
 *
 * Le modele (meteo_modele.h, genere par Colab) dit QUELLES entrees il utilise
 * (METEO_LISTE_ENTREES) : le modele final n'utilise que les capteurs
 *   temp, rhum, pres (niveau de la mer), dpres3h, spread.
 */
#ifndef METEO_FEATURES_H
#define METEO_FEATURES_H

#include <stdint.h>

/* Toutes les entrees que la carte sait calculer (le modele en choisit une partie) */
enum {
  METEO_E_TEMP = 0,   /* temperature (C)                                  */
  METEO_E_RHUM,       /* humidite relative (%)                            */
  METEO_E_PRES,       /* pression ramenee au niveau de la mer (hPa)       */
  METEO_E_DPRES3H,    /* pression(t) - pression(t - 3 h) (hPa)            */
  METEO_E_SPREAD,     /* temperature - point de rosee (C)                 */
  METEO_E_LAT, METEO_E_LON, METEO_E_ALT,
  METEO_E_DOY_SIN, METEO_E_DOY_COS, METEO_E_HOUR_SIN, METEO_E_HOUR_COS,
  METEO_E_DPRES1H,    /* tendances : valeur(t) - valeur(t - 1 h / 3 h / 6 h) */
  METEO_E_DPRES6H,
  METEO_E_DTEMP1H, METEO_E_DTEMP3H,
  METEO_E_DRHUM1H, METEO_E_DRHUM3H,
  METEO_NB_CANDIDATS
};

/* Grandeurs gardees dans l'historique de la carte */
enum { METEO_VAR_PRES = 0, METEO_VAR_TEMP, METEO_VAR_RHUM, METEO_NB_VAR
};

/* Nombre d'entrees, classes, poids : dans meteo_modele.h (genere par le notebook Colab) */
#include "meteo_modele.h"
#ifndef METEO_HISTORIQUE_H
#define METEO_HISTORIQUE_H  3        /* anciens fichiers meteo_modele.h */
#endif

/* Noms des classes affichees : Neige, Nuageux, Pluie, Soleil */
extern const char *const METEO_CLASSES[METEO_NB_CLASSES];

/* Position de la carte : Le Bourget-du-Lac (campus USMB).
   Seule l'altitude sert au modele final (conversion de la pression au niveau de la mer). */
#define METEO_LAT   45.65f
#define METEO_LON    5.86f
#define METEO_ALT  240.0f     /* m */

/* Decalage heure du PC -> UTC (France : 1 en hiver, 2 en ete), pour l'heure de compilation */
#define METEO_DECALAGE_UTC_H  1

/* Date/heure UTC (utilisee seulement si le modele en a besoin : METEO_BESOIN_HEURE) */
typedef struct {
  int annee, mois, jour;      /* mois 1..12, jour 1..31 */
  int heure, minute, seconde;
} meteo_utc_t;

/* --- Formules de base --- */
float meteo_niveau_mer(float p_station_hpa, float temp_c, float alt_m);
float meteo_point_de_rosee(float temp_c, float rhum);
int   meteo_jour_de_l_annee(int annee, int mois, int jour);

/* --- Les entrees du reseau, dans l'ordre de METEO_LISTE_ENTREES ---
   Les tendances sont calculees avec l'historique (appeler meteo_historique_ajouter avant).
   utc peut etre NULL si le modele n'utilise pas la date / l'heure.
   *historique_ok = 0 si une tendance utilisee n'a pas encore assez d'historique (mise a 0). */
void meteo_calculer_entrees(float temp_c, float rhum, float p_station_hpa,
                            uint32_t maintenant_ms, const meteo_utc_t *utc,
                            float entrees[METEO_NB_ENTREES], int *historique_ok);

/* --- Heure UTC sans RTC : on la lit dans l'en-tete "Date:" des reponses HTTP --- */
int  meteo_heure_depuis_http(const char *reponse_http, uint32_t maintenant_ms);
/* Retourne 1 si l'heure vient d'Internet, 2 si c'est l'heure de compilation (hors ligne) */
int  meteo_heure_utc(uint32_t maintenant_ms, meteo_utc_t *utc);

/* --- Historique des mesures (un point toutes les 10 min, 6 h 40 gardees) --- */
void  meteo_historique_ajouter(uint32_t maintenant_ms, float p_mer_hpa, float temp_c, float rhum);
/* actuelle - valeur il y a "heures" heures (var = METEO_VAR_xxx).
   *ok = 0 s'il n'y a pas encore assez d'historique (la fonction renvoie alors 0). */
float meteo_tendance(uint32_t maintenant_ms, int var, float actuelle, int heures, int *ok);

#endif /* METEO_FEATURES_H */
