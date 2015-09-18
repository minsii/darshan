/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ftw.h>
#include <zlib.h>

#include "darshan-logutils.h"

#define BUCKET1 0.20
#define BUCKET2 0.40
#define BUCKET3 0.60
#define BUCKET4 0.80

int total_shared = 0;
int total_fpp    = 0;
int total_mpio   = 0;
int total_pnet   = 0;
int total_hdf5   = 0;
int total_count  = 0;

int bucket1 = 0;
int bucket2 = 0;
int bucket3 = 0;
int bucket4 = 0;
int bucket5 = 0;

int process_log(const char *fname, double *io_ratio, int *used_mpio, int *used_pnet, int *used_hdf5, int *used_shared, int *used_fpp)
{
    int ret;
    darshan_fd file;
    struct darshan_job job;
    struct darshan_mod_logutil_funcs *psx_mod = mod_logutils[DARSHAN_POSIX_MOD];
    struct darshan_posix_file psx_rec;
    darshan_record_id rec_id;
    int f_count;
    double total_io_time;
    double total_job_time;

    assert(psx_mod);
    memset(&psx_rec, 0, sizeof(struct darshan_posix_file));

    file = darshan_log_open(fname);
    if (file == NULL)
    {
        fprintf(stderr, "darshan_log_open() failed to open %s.\n", fname);
        return -1;
    }

    ret = darshan_log_getjob(file, &job);
    if (ret < 0)
    {
        fprintf(stderr, "darshan_log_getjob() failed on file %s.\n", fname);
        darshan_log_close(file);
        return -1;
    }

    f_count = 0;
    total_io_time = 0.0;

    while((ret = psx_mod->log_get_record(file, &psx_rec, &rec_id)) == 1)
    {
        f_count   += 1;

        if (psx_rec.rank == -1)
            *used_shared = 1;
        else
            *used_fpp = 1;

        total_io_time += (psx_rec.fcounters[POSIX_F_READ_TIME] +
                         psx_rec.fcounters[POSIX_F_WRITE_TIME] +
                         psx_rec.fcounters[POSIX_F_META_TIME]);

        memset(&psx_rec, 0, sizeof(struct darshan_posix_file));
    }
    if (ret < 0)
    {
        fprintf(stderr, "Error: unable to read posix file record in log file %s.\n", fname);
        darshan_log_close(file);
        return -1;
    }

    if (file->mod_map[DARSHAN_MPIIO_MOD].len > 0)
        *used_mpio += 1;
    if (file->mod_map[DARSHAN_HDF5_MOD].len > 0)
        *used_hdf5 += 1;
    if (file->mod_map[DARSHAN_PNETCDF_MOD].len > 0)
        *used_pnet += 1;

    total_job_time = (double)job.end_time - (double)job.start_time;
    if (total_job_time < 1.0)
    {
        total_job_time = 1.0;
    }

    if (f_count > 0)
    {
        *io_ratio = total_io_time/total_job_time;
    }
    else
    {
        *io_ratio = 0.0;
    }

    darshan_log_close(file);

    return 0;
}

int tree_walk (const char *fpath, const struct stat *sb, int typeflag)
{
    double io_ratio = 0.0;
    int used_mpio = 0;
    int used_pnet = 0;
    int used_hdf5 = 0;
    int used_shared = 0;
    int used_fpp = 0;

    if (typeflag != FTW_F) return 0;

    process_log(fpath,&io_ratio,&used_mpio,&used_pnet,&used_hdf5,&used_shared,&used_fpp);

    total_count++;

    if (used_mpio > 0) total_mpio++;
    if (used_pnet > 0) total_pnet++;
    if (used_hdf5 > 0) total_hdf5++;
    if (used_shared > 0) total_shared++;
    if (used_fpp > 0) total_fpp++;

    if (io_ratio <= BUCKET1)
        bucket1++;
    else if ((io_ratio > BUCKET1) && (io_ratio <= BUCKET2))
        bucket2++;
    else if ((io_ratio > BUCKET2) && (io_ratio <= BUCKET3))
        bucket3++;
    else if ((io_ratio > BUCKET3) && (io_ratio <= BUCKET4))
        bucket4++;
    else if (io_ratio > BUCKET4)
        bucket5++;

    return 0;
}

int main(int argc, char **argv)
{
    char * base = NULL;
    int ret = 0;

    if(argc != 2)
    {
        fprintf(stderr, "Error: directory of Darshan logs required as argument.\n");
        return(-1);
    }

    base = argv[1];

    ret = ftw(base, tree_walk, 512);
    if(ret != 0)
    {
        fprintf(stderr, "Error: failed to walk path: %s\n", base);
        return(-1);
    }

    printf ("log dir: %s\n", base);
    printf ("  total: %d\n", total_count);
    printf (" shared: %lf [%d]\n", (double)total_shared/(double)total_count, total_shared);
    printf ("    fpp: %lf [%d]\n", (double)total_fpp/(double)total_count, total_fpp);
    printf ("   mpio: %lf [%d]\n", (double)total_mpio/(double)total_count, total_mpio);
    printf ("   pnet: %lf [%d]\n", (double)total_pnet/(double)total_count, total_pnet);
    printf ("   hdf5: %lf [%d]\n", (double)total_hdf5/(double)total_count, total_hdf5);
    printf ("%.2lf-%.2lf: %d\n", (double)0.0,     (double)BUCKET1, bucket1);
    printf ("%.2lf-%.2lf: %d\n", (double)BUCKET1, (double)BUCKET2, bucket2);
    printf ("%.2lf-%.2lf: %d\n", (double)BUCKET2, (double)BUCKET3, bucket3);
    printf ("%.2lf-%.2lf: %d\n", (double)BUCKET3, (double)BUCKET4, bucket4);
    printf ("%.2lf-%.2lf: %d\n", (double)BUCKET4, (double)100.0,   bucket5);
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
