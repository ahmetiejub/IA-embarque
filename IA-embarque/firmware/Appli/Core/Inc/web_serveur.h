/**
 * web_serveur.h - ETRS923, projet AMR & EJUB : page web servie par la carte
 *
 * Une fois la carte connectee, ouvrir dans un navigateur (meme reseau) :
 *     http://<adresse IP de la carte>/          -> tableau de bord
 *     http://<adresse IP de la carte>/data.json -> donnees brutes (JSON)
 * L'adresse est affichee dans la console : "[WEB] Page web : http://..."
 */
#ifndef WEB_SERVEUR_H
#define WEB_SERVEUR_H

#include <stdint.h>

/* Demarre la tache du serveur web (port 80). A appeler apres NET_Init(). */
void WEB_Demarrer(void);

/* Mises a jour des valeurs affichees (appelees par app_freertos.c) */
void WEB_Capteurs(float temp, float rhum, float p_station, const float acc_mg[3]);
void WEB_Edge(int classe, const float *proba, float duree_us, float p_mer, float dpres3h,
              int historique_ok, const char *heure_utc);
void WEB_Cloud(const char *classe, int longueur);
void WEB_Reelle(const char *classe, int code_wmo, float t, float rh, float p_mer,
                unsigned nb_correct, unsigned nb_verif);
/* Une comparaison carte / vraie meteo, gardee dans le journal de la page (20 dernieres) */
void WEB_Verification(const char *classe_edge, float proba_edge, const char *classe_reelle, int correct);

#endif /* WEB_SERVEUR_H */
