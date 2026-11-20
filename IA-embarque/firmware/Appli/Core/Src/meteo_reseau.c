/**
 * meteo_reseau.c - ETRS923 TP3, projet AMR & EJUB (Edge AI) - inference sur le Cortex-M55
 *
 * Reseau dense generique : les tailles, les poids et les noms des classes sont dans
 * meteo_modele.h, genere automatiquement par le notebook Colab (section 7).
 * Pour changer de modele : remplacer meteo_modele.h, puis Clean + Build. Ce fichier ne change pas.
 *
 * Chaque couche : y = x.W + b, puis ReLU (couches cachees) ou Softmax (derniere couche).
 * La normalisation des entrees est deja fusionnee dans la 1re couche (cf. notebook, section 6).
 */
#define METEO_MODELE_POIDS
#include "meteo_reseau.h"
#include <math.h>

static void dense(const float *x, int n_in, int n_out, const float *w, const float *b,
                  float *y, int relu)
{
  for (int j = 0; j < n_out; j++) {
    float s = b[j];
    for (int i = 0; i < n_in; i++) {
      s += x[i] * w[i * n_out + j];
    }
    y[j] = (relu && s < 0.0f) ? 0.0f : s;
  }
}

int meteo_reseau_inferer(const float entrees[METEO_NB_ENTREES], float proba[METEO_NB_CLASSES])
{
  float a[METEO_LARGEUR_MAX], b[METEO_LARGEUR_MAX];
  float *x = a, *y = b, *t;

  for (int i = 0; i < METEO_NB_ENTREES; i++) x[i] = entrees[i];
  for (int c = 0; c < METEO_NB_COUCHES; c++) {
    dense(x, METEO_TAILLES[c], METEO_TAILLES[c + 1], METEO_W[c], METEO_B[c], y,
          c < METEO_NB_COUCHES - 1);
    t = x; x = y; y = t;                 /* la sortie devient l'entree de la couche suivante */
  }

  /* Softmax sur les sorties du reseau (on soustrait le max pour eviter les debordements de expf) */
  static const int regroupement[METEO_NB_SORTIES_RESEAU] = METEO_REGROUPEMENT;
  float zmax = x[0], somme = 0.0f;
  for (int k = 1; k < METEO_NB_SORTIES_RESEAU; k++) if (x[k] > zmax) zmax = x[k];
  for (int k = 0; k < METEO_NB_SORTIES_RESEAU; k++) { x[k] = expf(x[k] - zmax); somme += x[k]; }

  /* Probabilite de chaque classe affichee (somme des sorties regroupees dans cette classe) */
  for (int c = 0; c < METEO_NB_CLASSES; c++) proba[c] = 0.0f;
  for (int k = 0; k < METEO_NB_SORTIES_RESEAU; k++) proba[regroupement[k]] += x[k] / somme;

  int meilleure = 0;
  for (int c = 1; c < METEO_NB_CLASSES; c++) if (proba[c] > proba[meilleure]) meilleure = c;
  return meilleure;
}
