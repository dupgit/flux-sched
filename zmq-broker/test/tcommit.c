/* tcommit - performance test for KVS commits */

#define _GNU_SOURCE
#include <json/json.h>
#include <assert.h>
#include <libgen.h>
#include <pthread.h>
#include <getopt.h>
#include <czmq.h>

#include "cmb.h"
#include "util.h"
#include "log.h"
#include "tstat.h"

typedef struct {
    pthread_t t;
    pthread_attr_t attr;
    int n;
    flux_t h;
    zlist_t *perf;
} thd_t;

static int count = -1;
static int nthreads = -1;
static char *prefix = NULL;
static bool fopt = false;
static bool sopt = false;

#define OPTIONS "fs"
static const struct option longopts[] = {
   {"fence",   no_argument,         0, 'f'},
   {"stats",   no_argument,         0, 's'},
   {0, 0, 0, 0},
};

static void usage (void)
{
    fprintf (stderr, "Usage: tcommit [--fence] [--stats] nthreads count prefix\n");
    exit (1);
}

double *ddup (double d)
{
    double *dcpy = xzmalloc (sizeof (double));
    *dcpy = d;
    return dcpy; 
}

void *thread (void *arg)
{
    thd_t *t = arg;
    char *key, *fence = NULL;
    int i;
    struct timespec t0;

    if (!(t->h = cmb_init ())) {
        err ("%d: cmb_init", t->n);
        goto done;
    }
    for (i = 0; i < count; i++) {
        if (asprintf (&key, "%s.%d.%d", prefix, t->n, i) < 0)
            oom ();
        if (fopt && asprintf (&fence, "%s-%d", prefix, i) < 0)
            oom ();
        if (sopt)
            monotime (&t0);
        if (kvs_put_int (t->h, key, 42) < 0)
            err_exit ("%s", key);
        if (fopt) {
            if (kvs_fence (t->h, fence, nthreads) < 0)
                err_exit ("kvs_commit");
        } else {
            if (kvs_commit (t->h) < 0)
                err_exit ("kvs_commit");
        }
        if (sopt && zlist_append (t->perf, ddup (monotime_since (t0))) < 0)
            oom ();
        free (key);
        if (fence)
            free (fence);
    }
done:
    if (t->h)
        flux_handle_destroy (&t->h);
  return NULL;
}

int main (int argc, char *argv[])
{
    thd_t *thd;
    int i, rc;
    int ch;
    tstat_t ts;
    struct timespec t0;

    log_init (basename (argv[0]));

    while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (ch) {
            case 'f':
                fopt = true;
                break;
            case 's':
                sopt = true;
                break;
            default:
                usage ();
        }
    }
    if (argc - optind != 3)
        usage ();

    nthreads = strtoul (argv[optind++], NULL, 10);
    count = strtoul (argv[optind++], NULL, 10);
    prefix = argv[optind++];

    memset (&ts, 0, sizeof (ts));

    thd = xzmalloc (sizeof (*thd) * nthreads);

    if (sopt)
        monotime (&t0);

    for (i = 0; i < nthreads; i++) {
        thd[i].n = i;
        if (!(thd[i].perf = zlist_new ()))
            oom ();
        if ((rc = pthread_attr_init (&thd[i].attr)))
            errn (rc, "pthread_attr_init");
        if ((rc = pthread_create (&thd[i].t, &thd[i].attr, thread, &thd[i])))
            errn (rc, "pthread_create");
    }

    for (i = 0; i < nthreads; i++) {
        if ((rc = pthread_join (thd[i].t, NULL)))
            errn (rc, "pthread_join");
        if (sopt) {
            double *e;
            while ((e = zlist_pop (thd[i].perf))) {
                tstat_push (&ts, *e);
                free (e);
            }
        }
        zlist_destroy (&thd[i].perf);
    }

    if (sopt) {
        json_object *o = util_json_object_new_object ();
        util_json_object_add_tstat (o, "put+commit times (sec)", &ts, 1E-3);
        util_json_object_add_double (o, "put+commmit throughput (#/sec)",
                          (double)(count*nthreads)/(monotime_since (t0)*1E-3));
        printf ("%s\n", json_object_to_json_string_ext (o,JSON_C_TO_STRING_PRETTY));
        json_object_put (o);
    }

    free (thd);

    log_fini ();

    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */