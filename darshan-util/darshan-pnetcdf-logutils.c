/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#define _GNU_SOURCE
#include "darshan-util-config.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "darshan-logutils.h"

/* counter name strings for the PNETCDF module */
#define X(a) #a,
char *pnetcdf_counter_names[] = {
    PNETCDF_COUNTERS
};

char *pnetcdf_f_counter_names[] = {
    PNETCDF_F_COUNTERS
};
#undef X

static int darshan_log_get_pnetcdf_file(darshan_fd fd, void* pnetcdf_buf,
    darshan_record_id* rec_id);
static int darshan_log_put_pnetcdf_file(darshan_fd fd, void* pnetcdf_buf);
static void darshan_log_print_pnetcdf_file(void *file_rec,
    char *file_name, char *mnt_pt, char *fs_type);

struct darshan_mod_logutil_funcs pnetcdf_logutils =
{
    .log_get_record = &darshan_log_get_pnetcdf_file,
    .log_put_record = &darshan_log_put_pnetcdf_file,
    .log_print_record = &darshan_log_print_pnetcdf_file,
};

static int darshan_log_get_pnetcdf_file(darshan_fd fd, void* pnetcdf_buf,
    darshan_record_id* rec_id)
{
    struct darshan_pnetcdf_file *file;
    int i;
    int ret;

    ret = darshan_log_getmod(fd, DARSHAN_PNETCDF_MOD, pnetcdf_buf,
        sizeof(struct darshan_pnetcdf_file));
    if(ret < 0)
        return(-1);
    else if(ret < sizeof(struct darshan_pnetcdf_file))
        return(0);
    else
    {
        file = (struct darshan_pnetcdf_file *)pnetcdf_buf;
        if(fd->swap_flag)
        {
            /* swap bytes if necessary */
            DARSHAN_BSWAP64(&file->f_id);
            DARSHAN_BSWAP64(&file->rank);
            for(i=0; i<PNETCDF_NUM_INDICES; i++)
                DARSHAN_BSWAP64(&file->counters[i]);
            for(i=0; i<PNETCDF_F_NUM_INDICES; i++)
                DARSHAN_BSWAP64(&file->fcounters[i]);
        }

        *rec_id = file->f_id;
        return(1);
    }
}

static int darshan_log_put_pnetcdf_file(darshan_fd fd, void* pnetcdf_buf)
{
    struct darshan_pnetcdf_file *file = (struct darshan_pnetcdf_file *)pnetcdf_buf;
    int ret;

    ret = darshan_log_putmod(fd, DARSHAN_PNETCDF_MOD, file,
        sizeof(struct darshan_pnetcdf_file));
    if(ret < 0)
        return(-1);

    return(0);
}

static void darshan_log_print_pnetcdf_file(void *file_rec, char *file_name,
    char *mnt_pt, char *fs_type)
{
    int i;
    struct darshan_pnetcdf_file *pnetcdf_file_rec =
        (struct darshan_pnetcdf_file *)file_rec;

    for(i=0; i<PNETCDF_NUM_INDICES; i++)
    {
        DARSHAN_COUNTER_PRINT(darshan_module_names[DARSHAN_PNETCDF_MOD],
            pnetcdf_file_rec->rank, pnetcdf_file_rec->f_id, pnetcdf_counter_names[i],
            pnetcdf_file_rec->counters[i], file_name, mnt_pt, fs_type);
    }

    for(i=0; i<PNETCDF_F_NUM_INDICES; i++)
    {
        DARSHAN_F_COUNTER_PRINT(darshan_module_names[DARSHAN_PNETCDF_MOD],
            pnetcdf_file_rec->rank, pnetcdf_file_rec->f_id, pnetcdf_f_counter_names[i],
            pnetcdf_file_rec->fcounters[i], file_name, mnt_pt, fs_type);
    }

    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
