/* =====================================================================
   etape9.c  —  CHRONOGUARD
   Interference memoire mesuree avec EPINGLAGE REEL des coeurs (Linux)

   Tache critique  : coeur 0, priorite temps reel SCHED_FIFO
   Voisins bruyants: coeurs 1, 2, 3, priorite normale

   Compilation :  gcc -O2 -Wall -pthread -o etape9 etape9.c
   Lancement   :  sudo ./etape9        (SCHED_FIFO exige les droits root)
   ===================================================================== */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#define TAILLE_KO       256    /* empreinte de la tache critique      */
#define PERIODE_US    20000    /* 50 Hz au lieu de 500 Hz */
#define ITERATIONS    10000    /* 20 s par campagne                   */
#define COEUR_CRITIQUE    0
#define BRUIT_THREADS     3    /* coeurs 1, 2, 3                      */
#define BRUIT_MO         64    /* agresseur MEMOIRE : depasse le L3   */
#define BRUIT_KO_CPU      4    /* agresseur CPU PUR : tient en L1     */

static volatile int bruit_actif = 0;
static int nb_coeurs = 4;

/* ------------------------------------------------------------------ */
uint64_t maintenant_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void dormir_jusqua(uint64_t cible)
{
    struct timespec t;
    t.tv_sec  = (time_t)(cible / 1000000000ULL);
    t.tv_nsec = (long)(cible % 1000000000ULL);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
}

/* Epingle le FIL APPELANT sur un coeur. Renvoie 1 si reussi. */
static int epingler(int coeur)
{
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(coeur % nb_coeurs, &m);
    return sched_setaffinity(0, sizeof(m), &m) == 0;
}

static uint64_t travail(uint8_t *tampon, size_t taille)
{
    uint64_t somme = 0;
    for (size_t i = 0; i < taille; i += 64) {
        tampon[i] = (uint8_t)(tampon[i] + 1);
        somme += tampon[i];
    }
    return somme;
}

/* ---- LE VOISIN : meme boucle, seule l'EMPREINTE change ---- */
struct consigne { size_t taille; int coeur; };

static void *voisin(void *arg)
{
    struct consigne *c = (struct consigne *)arg;
    epingler(c->coeur);

    uint8_t *zone = malloc(c->taille);
    if (!zone) return NULL;
    memset(zone, 1, c->taille);

    uint64_t somme = 0;
    while (bruit_actif) {
        for (size_t i = 0; i < c->taille && bruit_actif; i += 64) {
            zone[i]++;
            somme += zone[i];
        }
    }
    if (somme == 0xFFFFFFFFFFFFFFFFULL) printf(" ");
    free(zone);
    return NULL;
}

static void demarrer_bruit(pthread_t *fils, struct consigne *cons, size_t taille)
{
    bruit_actif = 1;
    for (int i = 0; i < BRUIT_THREADS; i++) {
        cons[i].taille = taille;
        cons[i].coeur  = 1 + i;              /* coeurs 1, 2, 3 */
        pthread_create(&fils[i], NULL, voisin, &cons[i]);
    }
    sleep(2);
}

static void arreter_bruit(pthread_t *fils)
{
    bruit_actif = 0;
    for (int i = 0; i < BRUIT_THREADS; i++) pthread_join(fils[i], NULL);
    sleep(2);
}

/* ------------------------------------------------------------------ */
static int comparer(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t centile(const uint64_t *tri, int n, double p)
{
    long i = (long)(p / 100.0 * (n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    return tri[i];
}

static void campagne(const char *titre, const char *fichier,
                     uint8_t *tampon, size_t taille, uint64_t *garde)
{
    uint64_t *ech = malloc((size_t)ITERATIONS * sizeof(uint64_t));
    if (!ech) { perror("malloc"); exit(1); }

    const uint64_t periode = (uint64_t)PERIODE_US * 1000ULL;
    uint64_t echeance = maintenant_ns();
    int rates = 0;

    for (int n = 0; n < ITERATIONS; n++) {
        *garde += travail(tampon, taille);
        echeance += periode;
        dormir_jusqua(echeance);
        uint64_t r = maintenant_ns();
        ech[n] = (r > echeance) ? (r - echeance) : (echeance - r);
        if (ech[n] > periode) rates++;
    }

    FILE *f = fopen(fichier, "w");
    if (f) {
        fprintf(f, "iteration,gigue_ns\n");
        for (int n = 0; n < ITERATIONS; n++)
            fprintf(f, "%d,%llu\n", n, (unsigned long long)ech[n]);
        fclose(f);
    }

    uint64_t *tri = malloc((size_t)ITERATIONS * sizeof(uint64_t));
    if (!tri) { perror("malloc"); exit(1); }
    for (int n = 0; n < ITERATIONS; n++) tri[n] = ech[n];
    qsort(tri, ITERATIONS, sizeof(uint64_t), comparer);

    double s = 0;
    for (int n = 0; n < ITERATIONS; n++) s += (double)tri[n];

    printf("%-28s %8.1f %8.1f %8.1f %8.1f %8.1f   %d/%d\n",
           titre, s / ITERATIONS / 1000.0,
           centile(tri, ITERATIONS, 50.0) / 1000.0,
           centile(tri, ITERATIONS, 99.0) / 1000.0,
           centile(tri, ITERATIONS, 99.9) / 1000.0,
           tri[ITERATIONS - 1] / 1000.0, rates, ITERATIONS);

    free(tri); free(ech);
}

/* ------------------------------------------------------------------ */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    nb_coeurs = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nb_coeurs < 1) nb_coeurs = 1;

    /* --- 1. epingler la tache critique sur le coeur 0 --- */
    if (!epingler(COEUR_CRITIQUE)) {
        perror("sched_setaffinity");
        return 1;
    }

    /* --- 2. priorite temps reel --- */
    struct sched_param p;
    memset(&p, 0, sizeof(p));
    p.sched_priority = 80;
    int tr = (sched_setscheduler(0, SCHED_FIFO, &p) == 0);
    if (!tr) perror("sched_setscheduler (relance avec sudo)");

    size_t taille = (size_t)TAILLE_KO * 1024;
    uint8_t *tampon = malloc(taille);
    if (!tampon) { perror("malloc"); return 1; }
    memset(tampon, 0, taille);
    uint64_t garde = 0;
    for (int n = 0; n < 200; n++) garde += travail(tampon, taille);

    printf("Coeurs disponibles : %d\n", nb_coeurs);
    printf("Tache critique : coeur %d (confirme : %d), temps reel : %s\n",
           COEUR_CRITIQUE, sched_getcpu(), tr ? "OUI" : "NON");
    printf("Voisins : %d threads sur les coeurs 1..%d, priorite normale\n",
           BRUIT_THREADS, BRUIT_THREADS);
    printf("Periode %d us, %d iterations par campagne\n\n", PERIODE_US, ITERATIONS);

    printf("%-28s %8s %8s %8s %8s %8s   %s\n",
           "campagne", "moy", "p50", "p99", "p99.9", "MAX", "rates");
    printf("%-28s %8s %8s %8s %8s %8s\n", "", "us", "us", "us", "us", "us");
    printf("----------------------------------------------------------------------------------\n");

    pthread_t fils[BRUIT_THREADS];
    struct consigne cons[BRUIT_THREADS];
    size_t gros  = (size_t)BRUIT_MO     * 1024 * 1024;
    size_t petit = (size_t)BRUIT_KO_CPU * 1024;

    campagne("A  silence", "a_silence.csv", tampon, taille, &garde);

    demarrer_bruit(fils, cons, petit);
    campagne("D  bruit CPU PUR (4 Ko)", "d_cpu.csv", tampon, taille, &garde);
    arreter_bruit(fils);

    demarrer_bruit(fils, cons, gros);
    campagne("B  bruit MEMOIRE (64 Mo)", "b_memoire.csv", tampon, taille, &garde);
    arreter_bruit(fils);

    campagne("C  silence (temoin)", "c_silence.csv", tampon, taille, &garde);

    printf("----------------------------------------------------------------------------------\n");
    printf("\nD contre A : effet d'occuper les autres coeurs, SANS trafic memoire.\n");
    printf("B contre D : INTERFERENCE MEMOIRE PURE (coeurs occupes des deux cotes).\n");
    printf("A contre C : derive de la machine pendant l'experience.\n");
    printf("(garde = %llu)\n", (unsigned long long)garde);
    free(tampon);
    return 0;
}
