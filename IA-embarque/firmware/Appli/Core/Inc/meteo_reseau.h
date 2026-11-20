/**
 * meteo_reseau.h - TP3 ETRS923, partie 3 (Edge AI)
 * Inference du reseau meteo directement sur la carte (Cortex-M55, float32).
 * Le modele (tailles, poids, classes) est dans meteo_modele.h, genere par le notebook Colab.
 */
#ifndef METEO_RESEAU_H
#define METEO_RESEAU_H

#include "meteo_features.h"

/* Calcule les probabilites des classes (ordre METEO_CLASSES).
   Retourne l'indice de la classe la plus probable. */
int meteo_reseau_inferer(const float entrees[METEO_NB_ENTREES], float proba[METEO_NB_CLASSES]);

#endif /* METEO_RESEAU_H */
