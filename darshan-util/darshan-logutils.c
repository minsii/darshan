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

/* default input buffer size for decompression algorithm */
#define DARSHAN_DEF_COMP_BUF_SZ (1024*1024) /* 1 MiB */

/* special identifers for referring to header, job, and
 * record map regions of the darshan log file
 */
#define DARSHAN_HEADER_REGION_ID    (-3)
#define DARSHAN_JOB_REGION_ID       (-2)
#define DARSHAN_REC_MAP_REGION_ID   (-1)

struct darshan_dz_state
{
    /* (libz/bzip2) stream data structure for managing
     * compression and decompression state */
    void *strm;
    /* buffer for staging compressed data to/from log file */
    unsigned char *buf;
    /* size of staging buffer */
    int size;
    /* for reading logs, flag indicating end of log file region */
    int eor;
    /* the region we last tried reading/writing */
    int prev_reg_id;
};

/* internal fd data structure */
struct darshan_fd_int_state
{
    /* posix file descriptor for the log file */
    int fildes;
    /* file pointer position */
    int64_t pos;
    /* flag indicating whether log file was created (and written) */
    int creat_flag;
    /* compression type used on log file (libz or bzip2) */
    enum darshan_comp_type comp_type;
    /* log file path name */
    char logfile_path[PATH_MAX];
    /* pointer to exe & mount data in darshan job data structure */
    char *exe_mnt_data;
    /* whether previous file operations have failed */
    int err;

    /* compression/decompression state */
    struct darshan_dz_state dz;
};

static int darshan_log_getheader(darshan_fd fd);
static int darshan_log_putheader(darshan_fd fd);
static int darshan_log_seek(darshan_fd fd, off_t offset);
static int darshan_log_read(darshan_fd fd, void *buf, int len);
static int darshan_log_write(darshan_fd fd, void *buf, int len);
static int darshan_log_dzinit(struct darshan_fd_int_state *state);
static void darshan_log_dzdestroy(struct darshan_fd_int_state *state);
static int darshan_log_dzread(darshan_fd fd, int region_id, void *buf, int len);
static int darshan_log_dzwrite(darshan_fd fd, int region_id, void *buf, int len);
static int darshan_log_libz_read(darshan_fd fd, int region_id, void *buf, int len);
static int darshan_log_libz_write(darshan_fd fd, int region_id, void *buf, int len);
static int darshan_log_libz_flush(darshan_fd fd, int region_id);
#ifdef HAVE_LIBBZ2
static int darshan_log_bzip2_read(darshan_fd fd, int region_id, void *buf, int len);
static int darshan_log_bzip2_write(darshan_fd fd, int region_id, void *buf, int len);
static int darshan_log_bzip2_flush(darshan_fd fd, int region_id);
#endif
static int darshan_log_dzload(darshan_fd fd, struct darshan_log_map map);
static int darshan_log_dzunload(darshan_fd fd, struct darshan_log_map *map_p);

/* each module's implementation of the darshan logutil functions */
#define X(a, b, c, d) d,
struct darshan_mod_logutil_funcs *mod_logutils[DARSHAN_MAX_MODS] =
{
    DARSHAN_MODULE_IDS
};
#undef X

/* darshan_log_open()
 *
 * open an existing darshan log file for reading only
 *
 * returns file descriptor on success, NULL on failure
 */
darshan_fd darshan_log_open(const char *name)
{
    darshan_fd tmp_fd;
    int ret;

    /* allocate a darshan file descriptor */
    tmp_fd = malloc(sizeof(*tmp_fd));
    if(!tmp_fd)
        return(NULL);
    memset(tmp_fd, 0, sizeof(*tmp_fd));
    tmp_fd->state = malloc(sizeof(struct darshan_fd_int_state));
    if(!tmp_fd->state)
    {
        free(tmp_fd->state);
        return(NULL);
    }
    memset(tmp_fd->state, 0, sizeof(struct darshan_fd_int_state));

    /* open the log file in read mode */
    tmp_fd->state->fildes = open(name, O_RDONLY);
    if(tmp_fd->state->fildes < 0)
    {
        fprintf(stderr, "Error: failed to open darshan log file %s.\n", name);
        free(tmp_fd->state);
        free(tmp_fd);
        return(NULL);
    }
    strncpy(tmp_fd->state->logfile_path, name, PATH_MAX);

    /* read the header from the log file to init fd data structures */
    ret = darshan_log_getheader(tmp_fd);
    if(ret < 0)
    {
        close(tmp_fd->state->fildes);
        free(tmp_fd->state);
        free(tmp_fd);
        return(NULL);
    }

    /* initialize compression data structures */
    ret = darshan_log_dzinit(tmp_fd->state);
    if(ret < 0)
    {
        fprintf(stderr, "Error: failed to initialize decompression data structures.\n");
        close(tmp_fd->state->fildes);
        free(tmp_fd->state);
        free(tmp_fd);
        return(NULL);
    }

    return(tmp_fd);
}

/* darshan_log_create()
 *
 * create a darshan log file for writing with the given compression method
 *
 * returns file descriptor on success, NULL on failure
 */
darshan_fd darshan_log_create(const char *name, enum darshan_comp_type comp_type,
    int partial_flag)
{
    darshan_fd tmp_fd;
    int ret;

    /* allocate a darshan file descriptor */
    tmp_fd = malloc(sizeof(*tmp_fd));
    if(!tmp_fd)
        return(NULL);
    memset(tmp_fd, 0, sizeof(*tmp_fd));
    tmp_fd->state = malloc(sizeof(struct darshan_fd_int_state));
    if(!tmp_fd->state)
    {
        free(tmp_fd);
        return(NULL);
    }
    memset(tmp_fd->state, 0, sizeof(struct darshan_fd_int_state));

    /* create the log for writing, making sure to not overwrite existing log */
    tmp_fd->state->fildes = creat(name, 0400);
    if(tmp_fd->state->fildes < 0)
    {
        fprintf(stderr, "Error: failed to open darshan log file %s.\n", name);
        free(tmp_fd->state);
        free(tmp_fd);
        return(NULL);
    }
    tmp_fd->state->creat_flag = 1;
    tmp_fd->state->comp_type = comp_type;
    tmp_fd->partial_flag = partial_flag;
    strncpy(tmp_fd->state->logfile_path, name, PATH_MAX);

    /* position file pointer to prealloc space for the log file header
     * NOTE: the header is written at close time, after all internal data
     * structures have been properly set
     */
    ret = darshan_log_seek(tmp_fd, sizeof(struct darshan_header));
    if(ret < 0)
    {
        fprintf(stderr, "Error: unable to seek in darshan log file.\n");
        close(tmp_fd->state->fildes);
        free(tmp_fd->state);
        free(tmp_fd);
        unlink(name);
        return(NULL);
    }

    /* initialize compression data structures */
    ret = darshan_log_dzinit(tmp_fd->state);
    if(ret < 0)
    {
        fprintf(stderr, "Error: failed to initialize compression data structures.\n");
        close(tmp_fd->state->fildes);
        free(tmp_fd->state);
        free(tmp_fd);
        unlink(name);
        return(NULL);
    }

    return(tmp_fd);
}

/* darshan_log_getjob()
 *
 * read job level metadata from the darshan log file
 *
 * returns 0 on success, -1 on failure
 */
int darshan_log_getjob(darshan_fd fd, struct darshan_job *job)
{
    struct darshan_fd_int_state *state = fd->state;
    char job_buf[DARSHAN_JOB_RECORD_SIZE] = {0};
    int job_buf_sz = DARSHAN_JOB_RECORD_SIZE;
    int ret;

    assert(state);
    assert(fd->job_map.len > 0 && fd->job_map.off > 0);

    /* read the compressed job data from the log file */
    ret = darshan_log_dzread(fd, DARSHAN_JOB_REGION_ID, job_buf, job_buf_sz);
    if(ret <= sizeof(*job))
    {
        fprintf(stderr, "Error: failed to read darshan log file job data.\n");
        return(-1);
    }

    memcpy(job, job_buf, sizeof(*job));

    if(fd->swap_flag)
    {
        /* swap bytes if necessary */
        DARSHAN_BSWAP64(&job->uid);
        DARSHAN_BSWAP64(&job->start_time);
        DARSHAN_BSWAP64(&job->end_time);
        DARSHAN_BSWAP64(&job->nprocs);
        DARSHAN_BSWAP64(&job->jobid);
    }

    /* save trailing exe & mount information, so it can be retrieved later */
    if(!(state->exe_mnt_data))
        state->exe_mnt_data = malloc(DARSHAN_EXE_LEN+1);
    if(!(state->exe_mnt_data))
        return(-1);
    memcpy(state->exe_mnt_data, &job_buf[sizeof(*job)], DARSHAN_EXE_LEN+1);

    return(0);
}

/* darshan_log_putjob()
 *
 * write job level metadata to darshan log file
 *
 * returns 0 on success, -1 on failure
 */
int darshan_log_putjob(darshan_fd fd, struct darshan_job *job)
{
    struct darshan_fd_int_state *state = fd->state;
    struct darshan_job job_copy;
    int len;
    int ret;

    assert(state);

    memset(&job_copy, 0, sizeof(*job));
    memcpy(&job_copy, job, sizeof(*job));

    /* check for newline in existing metadata, add if needed */
    len = strlen(job_copy.metadata);
    if(len > 0 && len < DARSHAN_JOB_METADATA_LEN)
    {
        if(job_copy.metadata[len-1] != '\n')
        {
            job_copy.metadata[len] = '\n';
            job_copy.metadata[len+1] = '\0';
        }
    }

    /* write the compressed job data to log file */
    ret = darshan_log_dzwrite(fd, DARSHAN_JOB_REGION_ID, &job_copy, sizeof(*job));
    if(ret != sizeof(*job))
    {
        state->err = -1;
        fprintf(stderr, "Error: failed to write darshan log file job data.\n");
        return(-1);
    }

    return(0);
}

/* darshan_log_getexe()
 *
 * reads the application exe name from darshan log file
 * 
 * returns 0 on success, -1 on failure 
 */
int darshan_log_getexe(darshan_fd fd, char *buf)
{
    struct darshan_fd_int_state *state = fd->state;
    char *newline;
    int ret;

    assert(state);

    /* if the exe/mount data has not been saved yet, read in the job info */
    if(!(state->exe_mnt_data))
    {
        struct darshan_job job;
        ret = darshan_log_getjob(fd, &job);

        if(ret < 0 || !(state->exe_mnt_data))
            return(-1);
    }

    /* exe string is located before the first line break */
    newline = strchr(state->exe_mnt_data, '\n');

    /* copy over the exe string */
    if(newline)
        memcpy(buf, state->exe_mnt_data, (newline - state->exe_mnt_data));

    return (0);
}

/* darshan_log_putexe()
 *
 * wrties the application exe name to darshan log file
 * NOTE: this needs to be called immediately following put_job as it
 * expects the file pointer to be positioned immediately following
 * the darshan job information
 *
 * returns 0 on success, -1 on failure 
 */
int darshan_log_putexe(darshan_fd fd, char *buf)
{
    struct darshan_fd_int_state *state = fd->state;
    int len = strlen(buf);
    int ret;

    assert(fd->state);

    ret = darshan_log_dzwrite(fd, DARSHAN_JOB_REGION_ID, buf, len);
    if(ret != len)
    {
        state->err = -1;
        fprintf(stderr, "Error: failed to write exe string to darshan log file.\n");
        return(-1);
    }

    return(0);
}

/* darshan_log_getmounts()
 * 
 * retrieves mount table information from the log. Note that mnt_pts and
 * fs_types are arrays that will be allocated by the function and must be
 * freed by the caller. count will indicate the size of the arrays
 *
 * returns 0 on success, -1 on failure
 */
int darshan_log_getmounts(darshan_fd fd, char*** mnt_pts,
    char*** fs_types, int* count)
{
    struct darshan_fd_int_state *state = fd->state;
    char *pos;
    int array_index = 0;
    int ret;

    assert(state);

    /* if the exe/mount data has not been saved yet, read in the job info */
    if(!(state->exe_mnt_data))
    {
        struct darshan_job job;
        ret = darshan_log_getjob(fd, &job);

        if(ret < 0 || !(state->exe_mnt_data))
            return(-1);
    }

    /* count entries */
    *count = 0;
    pos = state->exe_mnt_data;
    while((pos = strchr(pos, '\n')) != NULL)
    {
        pos++;
        (*count)++;
    }

    if(*count == 0)
    {
        /* no mount entries present */
        return(0);
    }

    /* allocate output arrays */
    *mnt_pts = malloc((*count)*sizeof(char*));
    assert(*mnt_pts);
    *fs_types = malloc((*count)*sizeof(char*));
    assert(*fs_types);

    /* work backwards through the table and parse each line (except for
     * first, which holds command line information)
     */
    while((pos = strrchr(state->exe_mnt_data, '\n')) != NULL)
    {
        /* overestimate string lengths */
        (*mnt_pts)[array_index] = malloc(DARSHAN_EXE_LEN);
        assert((*mnt_pts)[array_index]);
        (*fs_types)[array_index] = malloc(DARSHAN_EXE_LEN);
        assert((*fs_types)[array_index]);

        ret = sscanf(++pos, "%s\t%s", (*fs_types)[array_index],
            (*mnt_pts)[array_index]);
        if(ret != 2)
        {
            fprintf(stderr, "Error: poorly formatted mount table in darshan log file.\n");
            return(-1);
        }
        pos--;
        *pos = '\0';
        array_index++;
    }

    return(0);
}

/* darshan_log_putmounts()
 *
 * writes mount information to the darshan log file
 * NOTE: this function call should follow immediately after the call
 * to darshan_log_putexe(), as it assumes the darshan log file pointer
 * is pointing to the offset immediately following the exe string
 *
 * returns 0 on success, -1 on failure
 */
int darshan_log_putmounts(darshan_fd fd, char** mnt_pts, char** fs_types, int count)
{
    struct darshan_fd_int_state *state = fd->state;
    int i;
    char line[1024];
    char mnt_dat[DARSHAN_EXE_LEN] = {0};
    int mnt_dat_sz = 0;
    char *tmp;
    int ret;

    assert(state);

    /* write each mount entry to file */
    tmp = mnt_dat;
    for(i=count-1; i>=0; i--)
    {
        sprintf(line, "\n%s\t%s", fs_types[i], mnt_pts[i]);

        memcpy(tmp, line, strlen(line));
        tmp += strlen(line);
        mnt_dat_sz += strlen(line);
    }

    ret = darshan_log_dzwrite(fd, DARSHAN_JOB_REGION_ID, mnt_dat, mnt_dat_sz);
    if (ret != mnt_dat_sz)
    {
        state->err = -1;
        fprintf(stderr, "Error: failed to write darshan log mount data.\n");
        return(-1);
    }

    return(0);
}

/* darshan_log_gethash()
 *
 * read the hash of records from the darshan log file
 *
 * returns 0 on success, -1 on failure
 */
int darshan_log_gethash(darshan_fd fd, struct darshan_record_ref **hash)
{
    struct darshan_fd_int_state *state = fd->state;
    char *hash_buf;
    int hash_buf_sz;
    char *buf_ptr;
    darshan_record_id *rec_id_ptr;
    uint32_t *path_len_ptr, tmp_path_len;
    char *path_ptr;
    struct darshan_record_ref *ref;
    int read;
    int read_req_sz;
    int buf_remaining = 0;

    assert(state);

    /* just return if there is no record mapping data */
    if(fd->rec_map.len == 0)
    {
        *hash = NULL;
        return(0);
    }

    /* default to hash buffer twice as big as default compression buf */
    hash_buf = malloc(DARSHAN_DEF_COMP_BUF_SZ * 2);
    if(!hash_buf)
        return(-1);
    memset(hash_buf, 0, DARSHAN_DEF_COMP_BUF_SZ * 2);
    hash_buf_sz = DARSHAN_DEF_COMP_BUF_SZ * 2;

    do
    {
        /* read chunks of the darshan record id -> file name mapping from log file,
         * constructing a hash table in the process
         */
        read_req_sz = hash_buf_sz - buf_remaining;
        read = darshan_log_dzread(fd, DARSHAN_REC_MAP_REGION_ID,
            hash_buf + buf_remaining, read_req_sz);
        if(read < 0)
        {
            fprintf(stderr, "Error: failed to read record hash from darshan log file.\n");
            free(hash_buf);
            return(-1);
        }

        /* work through the hash buffer -- deserialize the mapping data and
         * add to the output hash table
         * NOTE: these mapping pairs are variable in length, so we have to be able
         * to handle incomplete mappings temporarily here
         */
        buf_ptr = hash_buf;
        buf_remaining += read;
        while(buf_remaining > (sizeof(darshan_record_id) + sizeof(uint32_t)))
        {
            /* see if we have enough buf space to read in the next full record */
            tmp_path_len = *(uint32_t *)(buf_ptr + sizeof(darshan_record_id));
            if(fd->swap_flag)
                DARSHAN_BSWAP32(&tmp_path_len);

            /* we need to read more before we continue deserializing */
            if(buf_remaining <
                (sizeof(darshan_record_id) + sizeof(uint32_t) + tmp_path_len))
                break;

            /* get pointers for each field of this darshan record */
            /* NOTE: darshan record hash serialization method: 
             *          ... darshan_record_id | (uint32_t) path_len | path ...
             */
            rec_id_ptr = (darshan_record_id *)buf_ptr;
            buf_ptr += sizeof(darshan_record_id);
            path_len_ptr = (uint32_t *)buf_ptr;
            buf_ptr += sizeof(uint32_t);
            path_ptr = (char *)buf_ptr;

            if(fd->swap_flag)
            {
                /* we need to sort out endianness issues before deserializing */
                DARSHAN_BSWAP64(rec_id_ptr);
                DARSHAN_BSWAP32(path_len_ptr);
            }

            HASH_FIND(hlink, *hash, rec_id_ptr, sizeof(darshan_record_id), ref);
            if(!ref)
            {
                ref = malloc(sizeof(*ref));
                if(!ref)
                {
                    free(hash_buf);
                    return(-1);
                }
                ref->rec.name = malloc(*path_len_ptr + 1);
                if(!ref->rec.name)
                {
                    free(ref);
                    free(hash_buf);
                    return(-1);
                }

                /* set the fields for this record */
                ref->rec.id = *rec_id_ptr;
                memcpy(ref->rec.name, path_ptr, *path_len_ptr);
                ref->rec.name[*path_len_ptr] = '\0';

                /* add this record to the hash */
                HASH_ADD(hlink, *hash, rec.id, sizeof(darshan_record_id), ref);
            }

            buf_ptr += *path_len_ptr;
            buf_remaining -=
                (sizeof(darshan_record_id) + sizeof(uint32_t) + *path_len_ptr);
        }

        /* copy any leftover data to beginning of buffer to parse next */
        memcpy(hash_buf, buf_ptr, buf_remaining);

        /* we keep reading until we get a short read informing us we have
         * read all of the record hash
         */
    } while(read == read_req_sz);
    assert(buf_remaining == 0);

    free(hash_buf);
    return(0);
}

/* darshan_log_puthash()
 *
 * writes the hash table of records to the darshan log file
 * NOTE: this function call should follow immediately after the call
 * to darshan_log_putmounts(), as it assumes the darshan log file pointer
 * is pointing to the offset immediately following the mount information
 *
 * returns 0 on success, -1 on failure
 */
int darshan_log_puthash(darshan_fd fd, struct darshan_record_ref *hash)
{
    struct darshan_fd_int_state *state = fd->state;
    char *hash_buf;
    int hash_buf_sz;
    struct darshan_record_ref *ref, *tmp;
    char *buf_ptr;
    int path_len;
    int wrote;

    assert(state);

    /* allocate memory for largest possible hash record */
    hash_buf_sz = sizeof(darshan_record_id) + sizeof(uint32_t) + PATH_MAX;
    hash_buf = malloc(hash_buf_sz);
    if(!hash_buf)
        return(-1);
    memset(hash_buf, 0, hash_buf_sz);

    /* individually serialize each hash record and write to log file */
    HASH_ITER(hlink, hash, ref, tmp)
    {
        buf_ptr = hash_buf;
        path_len = strlen(ref->rec.name);

        /* the hash buffer has space to serialize this record
         * NOTE: darshan record hash serialization method: 
         *          ... darshan_record_id | (uint32_t) path_len | path ...
         */
        *((darshan_record_id *)buf_ptr) = ref->rec.id;
        buf_ptr += sizeof(darshan_record_id);
        *((uint32_t *)buf_ptr) = path_len;
        buf_ptr += sizeof(uint32_t);
        memcpy(buf_ptr, ref->rec.name, path_len);
        buf_ptr += path_len;

        /* write this hash entry to log file */
        wrote = darshan_log_dzwrite(fd, DARSHAN_REC_MAP_REGION_ID,
            hash_buf, (buf_ptr - hash_buf));
        if(wrote != (buf_ptr - hash_buf))
        {
            state->err = -1;
            fprintf(stderr, "Error: failed to write record hash to darshan log file.\n");
            free(hash_buf);
            return(-1);
        }
    }

    free(hash_buf);
    return(0);
}

/* darshan_log_getmod()
 *
 * get a chunk of module data from the darshan log file
 *
 * returns number of bytes read on success, -1 on failure
 */
int darshan_log_getmod(darshan_fd fd, darshan_module_id mod_id,
    void *mod_buf, int mod_buf_sz)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    assert(state);

    if(mod_id < 0 || mod_id >= DARSHAN_MAX_MODS)
    {
        fprintf(stderr, "Error: invalid Darshan module id.\n");
        return(-1);
    }

    if(fd->mod_map[mod_id].len == 0)
        return(0); /* no data corresponding to this mod_id */

    /* read this module's data from the log file */
    ret = darshan_log_dzread(fd, mod_id, mod_buf, mod_buf_sz);
    if(ret < 0)
    {
        fprintf(stderr,
            "Error: failed to read module %s data from darshan log file.\n",
            darshan_module_names[mod_id]);
        return(-1);
    }

    return(ret);
}

/* darshan_log_putmod()
 *
 * write a chunk of module data to the darshan log file
 * NOTE: this function call should be called directly after the
 * put_hash() function, as it expects the file pointer to be
 * positioned directly past the record hash location. Also,
 * for a set of modules with data to write to file, this function
 * should be called in order of increasing module identifiers,
 * as the darshan log file format expects this ordering.
 *
 * returns number of bytes written on success, -1 on failure
 */
int darshan_log_putmod(darshan_fd fd, darshan_module_id mod_id,
    void *mod_buf, int mod_buf_sz)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    assert(state);

    if(mod_id < 0 || mod_id >= DARSHAN_MAX_MODS)
    {
        state->err = -1;
        fprintf(stderr, "Error: invalid Darshan module id.\n");
        return(-1);
    }

    /* write the module chunk to the log file */
    ret = darshan_log_dzwrite(fd, mod_id, mod_buf, mod_buf_sz);
    if(ret != mod_buf_sz)
    {
        state->err = -1;
        fprintf(stderr,
            "Error: failed to write module %s data to darshan log file.\n",
            darshan_module_names[mod_id]);
        return(-1);
    }

    return(0);
}

/* darshan_log_close()
 *
 * close an open darshan file descriptor, freeing any resources
 *
 */
void darshan_log_close(darshan_fd fd)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    assert(state);

    /* if the file was created for writing */
    if(state->creat_flag)
    {
        /* flush the last region of the log to file */
        switch(state->comp_type)
        {
            case DARSHAN_ZLIB_COMP:
                ret = darshan_log_libz_flush(fd, state->dz.prev_reg_id);
                if(ret == 0)
                    break;
#ifdef HAVE_LIBBZ2
            case DARSHAN_BZIP2_COMP:
                ret = darshan_log_bzip2_flush(fd, state->dz.prev_reg_id);
                if(ret == 0)
                    break;
#endif 
            default:
                /* if flush fails, remove the output log file */
                state->err = -1;
                fprintf(stderr, "Error: final flush to log file failed.\n");
                break;
        }

        /* if no errors flushing, write the log header before closing */
        if(state->err != -1)
        {
            ret = darshan_log_putheader(fd);
            if(ret < 0)
                state->err = -1;
        }
    }

    close(state->fildes);

    /* remove output log file if error writing to it */
    if((state->creat_flag) && (state->err == -1))
    {
        fprintf(stderr, "Unlinking darshan log file %s ...\n",
            state->logfile_path);
        unlink(state->logfile_path);
    }

    darshan_log_dzdestroy(state);
    if(state->exe_mnt_data)
        free(state->exe_mnt_data);
    free(state);
    free(fd);

    return;
}

/* **************************************************** */

/* read the header of the darshan log and set internal fd data structures
 * NOTE: this is the only portion of the darshan log that is uncompressed
 *
 * returns 0 on success, -1 on failure
 */
static int darshan_log_getheader(darshan_fd fd)
{
    struct darshan_fd_int_state *state = fd->state;
    struct darshan_header header;
    int i;
    int ret;

    ret = darshan_log_seek(fd, 0);
    if(ret < 0)
    {
        fprintf(stderr, "Error: unable to seek in darshan log file.\n");
        return(-1);
    }

    /* read uncompressed header from log file */
    ret = darshan_log_read(fd, &header, sizeof(header));
    if(ret != sizeof(header))
    {
        fprintf(stderr, "Error: failed to read darshan log file header.\n");
        return(-1);
    }

    /* save the version string */
    strncpy(fd->version, header.version_string, 8);

    if(header.magic_nr == DARSHAN_MAGIC_NR)
    {
        /* no byte swapping needed, this file is in host format already */
        fd->swap_flag = 0;
    }
    else
    {
        /* try byte swapping */
        DARSHAN_BSWAP64(&(header.magic_nr));
        if(header.magic_nr == DARSHAN_MAGIC_NR)
        {
            fd->swap_flag = 1;

            /* swap the log map variables in the header */
            DARSHAN_BSWAP64(&(header.rec_map.off));
            DARSHAN_BSWAP64(&(header.rec_map.len));
            for(i = 0; i < DARSHAN_MAX_MODS; i++)
            {
                DARSHAN_BSWAP64(&(header.mod_map[i].off));
                DARSHAN_BSWAP64(&(header.mod_map[i].len));
            }
        }
        else
        {
            /* otherwise this file is just broken */
            fprintf(stderr, "Error: bad magic number in darshan log file.\n");
            return(-1);
        }
    }

    state->comp_type = header.comp_type;
    fd->partial_flag = header.partial_flag;

    /* save the mapping of data within log file to this file descriptor */
    fd->job_map.off = sizeof(struct darshan_header);
    fd->job_map.len = header.rec_map.off - fd->job_map.off;
    memcpy(&fd->rec_map, &(header.rec_map), sizeof(struct darshan_log_map));
    memcpy(&fd->mod_map, &(header.mod_map), DARSHAN_MAX_MODS * sizeof(struct darshan_log_map));

    return(0);
}

/* write a darshan header to log file
 *
 * returns 0 on success, -1 on failure
 */
static int darshan_log_putheader(darshan_fd fd)
{
    struct darshan_fd_int_state *state = fd->state;
    struct darshan_header header;
    int ret;

    ret = darshan_log_seek(fd, 0);
    if(ret < 0)
    {
        fprintf(stderr, "Error: unable to seek in darshan log file.\n");
        return(-1);
    }

    memset(&header, 0, sizeof(header));
    strcpy(header.version_string, DARSHAN_LOG_VERSION);
    header.magic_nr = DARSHAN_MAGIC_NR;
    header.comp_type = state->comp_type;
    header.partial_flag = fd->partial_flag;

    /* copy the mapping information to the header */
    memcpy(&header.rec_map, &fd->rec_map, sizeof(struct darshan_log_map));
    memcpy(&header.mod_map, &fd->mod_map, DARSHAN_MAX_MODS * sizeof(struct darshan_log_map));

    /* write header to file */
    ret = darshan_log_write(fd, &header, sizeof(header));
    if(ret != sizeof(header))
    {
        fprintf(stderr, "Error: failed to write Darshan log file header.\n");
        return(-1);
    }

    return(0);
}

/* return 0 on successful seek to offset, -1 on failure.
 */
static int darshan_log_seek(darshan_fd fd, off_t offset)
{
    struct darshan_fd_int_state *state = fd->state;
    off_t ret_off;

    if(state->pos == offset)
        return(0);

    ret_off = lseek(state->fildes, offset, SEEK_SET);
    if(ret_off == offset)
    {
        state->pos = offset;
        return(0);
    }

    return(-1);
}

/* return amount read on success, 0 on EOF, -1 on failure.
 */
static int darshan_log_read(darshan_fd fd, void* buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    /* read data from the log file using the given map */
    ret = read(state->fildes, buf, len);
    if(ret > 0)
        state->pos += ret;

    return(ret);
}

/* return amount written on success, -1 on failure.
 */
static int darshan_log_write(darshan_fd fd, void* buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    ret = write(state->fildes, buf, len);
    if(ret > 0)
        state->pos += ret;

    return(ret);
}

static int darshan_log_dzinit(struct darshan_fd_int_state *state)
{
    int ret;

    /* initialize buffers for staging compressed data to/from log file */
    state->dz.buf = malloc(DARSHAN_DEF_COMP_BUF_SZ);
    if(state->dz.buf == NULL)
        return(-1);

    state->dz.prev_reg_id = DARSHAN_HEADER_REGION_ID;

    switch(state->comp_type)
    {
        case DARSHAN_ZLIB_COMP:
        {
            z_stream *tmp_zstrm = malloc(sizeof(*tmp_zstrm));
            if(!tmp_zstrm)
            {
                free(state->dz.buf);
                return(-1);
            }
            tmp_zstrm->zalloc = Z_NULL;
            tmp_zstrm->zfree = Z_NULL;
            tmp_zstrm->opaque = Z_NULL;
            tmp_zstrm->avail_in = 0;
            tmp_zstrm->next_in = Z_NULL;

            /* TODO: worth using {inflate/deflate}Init2 ?? */
            if(!(state->creat_flag))
            {
                /* read only file, init inflate algorithm */
                ret = inflateInit(tmp_zstrm);
            }
            else
            {
                /* write only file, init deflate algorithm */
                ret = deflateInit(tmp_zstrm, Z_DEFAULT_COMPRESSION);
                tmp_zstrm->avail_out = DARSHAN_DEF_COMP_BUF_SZ;
                tmp_zstrm->next_out = state->dz.buf;
            }
            if(ret != Z_OK)
            {
                free(tmp_zstrm);
                free(state->dz.buf);
                return(-1);
            }
            state->dz.strm = tmp_zstrm;
            break;
        }
#ifdef HAVE_LIBBZ2
        case DARSHAN_BZIP2_COMP:
        {
            bz_stream *tmp_bzstrm = malloc(sizeof(*tmp_bzstrm));
            if(!tmp_bzstrm)
            {
                free(state->dz.buf);
                return(-1);
            }
            tmp_bzstrm->bzalloc = NULL;
            tmp_bzstrm->bzfree = NULL;
            tmp_bzstrm->opaque = NULL;
            tmp_bzstrm->avail_in = 0;
            tmp_bzstrm->next_in = Z_NULL;

            if(state->creat_flag)
            {
                /* read only file, init decompress algorithm */
                ret = BZ2_bzDecompressInit(tmp_bzstrm, 1, 0);
            }
            else
            {
                /* write only file, init compress algorithm */
                ret = BZ2_bzCompressInit(tmp_bzstrm, 9, 1, 30);
                tmp_bzstrm->avail_out = DARSHAN_DEF_COMP_BUF_SZ;
                tmp_bzstrm->next_out = (char *)state->dz.buf;
            }
            if(ret != BZ_OK)
            {
                free(tmp_bzstrm);
                free(state->dz.buf);
                return(-1);
            }
            state->dz.strm = tmp_bzstrm;
            break;
        }
#endif
        default:
            fprintf(stderr, "Error: invalid compression type.\n");
            return(-1);
    }

    return(0);
}

static void darshan_log_dzdestroy(struct darshan_fd_int_state *state)
{
    switch(state->comp_type)
    {
        case DARSHAN_ZLIB_COMP:
            if(!(state->creat_flag))
                inflateEnd(state->dz.strm);
            else
                deflateEnd(state->dz.strm);
            free(state->dz.strm);
            break;
#ifdef HAVE_LIBBZ2
        case DARSHAN_BZIP2_COMP:
            if(!(state->creat_flag))
                BZ2_bzDecompressEnd(state->dz.strm);
            else
                BZ2_bzCompressEnd(state->dz.strm);
            free(state->dz.strm);
            break;
#endif
        default:
            fprintf(stderr, "Error: invalid compression type.\n");
    }

    free(state->dz.buf);
    return;
}

static int darshan_log_dzread(darshan_fd fd, int region_id, void *buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    switch(state->comp_type)
    {
        case DARSHAN_ZLIB_COMP:
            ret = darshan_log_libz_read(fd, region_id, buf, len);
            break;
#ifdef HAVE_LIBBZ2
        case DARSHAN_BZIP2_COMP:
            ret = darshan_log_bzip2_read(fd, region_id, buf, len);
            break;
#endif
        default:
            fprintf(stderr, "Error: invalid compression type.\n");
            return(-1);
    }

    return(ret);
}

static int darshan_log_dzwrite(darshan_fd fd, int region_id, void *buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;

    switch(state->comp_type)
    {
        case DARSHAN_ZLIB_COMP:
            ret = darshan_log_libz_write(fd, region_id, buf, len);
            break;
#ifdef HAVE_LIBBZ2
        case DARSHAN_BZIP2_COMP:
            ret = darshan_log_bzip2_write(fd, region_id, buf, len);
            break;
#endif
        default:
            fprintf(stderr, "Error: invalid compression type.\n");
            return(-1);
    }

    return(ret);
}

static int darshan_log_libz_read(darshan_fd fd, int region_id, void *buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    int total_bytes = 0;
    int tmp_out_bytes;
    struct darshan_log_map map;
    z_stream *z_strmp = (z_stream *)state->dz.strm;

    assert(z_strmp);

    /* if new log region, we reload buffers and clear eor flag */
    if(region_id != state->dz.prev_reg_id)
    {
        z_strmp->avail_in = 0;
        state->dz.eor = 0;
        state->dz.prev_reg_id = region_id;
    }

    if(region_id == DARSHAN_JOB_REGION_ID)
        map = fd->job_map;
    else if(region_id == DARSHAN_REC_MAP_REGION_ID)
        map = fd->rec_map;
    else
        map = fd->mod_map[region_id];

    z_strmp->avail_out = len;
    z_strmp->next_out = buf;

    /* we just decompress until the output buffer is full, assuming there
     * is enough compressed data in file to satisfy the request size.
     */
    while(z_strmp->avail_out)
    {
        /* check if we need more compressed data */
        if(z_strmp->avail_in == 0)
        {
            /* if the eor flag is set, clear it and return -- future
             * reads of this log region will restart at the beginning
             */
            if(state->dz.eor)
            {
                state->dz.eor = 0;
                break;
            }

            /* read more data from input file */
            ret = darshan_log_dzload(fd, map);
            if(ret < 0)
                return(-1);
            assert(state->dz.size > 0);

            z_strmp->avail_in = state->dz.size;
            z_strmp->next_in = state->dz.buf;
        }

        tmp_out_bytes = z_strmp->total_out;
        ret = inflate(z_strmp, Z_NO_FLUSH);
        if(ret != Z_OK && ret != Z_STREAM_END)
        {
            fprintf(stderr, "Error: unable to inflate darshan log data.\n");
            return(-1);
        }
        total_bytes += (z_strmp->total_out - tmp_out_bytes);

        /* reset the decompression if we encountered end of stream */
        if(ret == Z_STREAM_END)
            inflateReset(z_strmp);
    }

    return(total_bytes);
}

static int darshan_log_libz_write(darshan_fd fd, int region_id, void *buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    int total_bytes = 0;
    int tmp_in_bytes;
    int tmp_out_bytes;
    struct darshan_log_map *map_p;
    z_stream *z_strmp = (z_stream *)state->dz.strm;

    assert(z_strmp);

    /* if new log region, finish prev region's zstream and flush to log file */
    if(region_id != state->dz.prev_reg_id)
    {
        /* error out if the region we are writing to precedes the previous
         * region we wrote -- we shouldn't be moving backwards in the log
         */
        if(region_id < state->dz.prev_reg_id)
            return(-1);

        if(state->dz.prev_reg_id != DARSHAN_HEADER_REGION_ID)
        {
            ret = darshan_log_libz_flush(fd, state->dz.prev_reg_id);
            if(ret < 0)
                return(-1);
        }

        state->dz.prev_reg_id = region_id;
    }

    if(region_id == DARSHAN_JOB_REGION_ID)
        map_p = &(fd->job_map);
    else if(region_id == DARSHAN_REC_MAP_REGION_ID)
        map_p = &(fd->rec_map);
    else
        map_p = &(fd->mod_map[region_id]);

    z_strmp->avail_in = len;
    z_strmp->next_in = buf;

    /* compress input data until none left */
    while(z_strmp->avail_in)
    {
        /* if we are out of output, flush to log file */
        if(z_strmp->avail_out == 0)
        {
            assert(state->dz.size == DARSHAN_DEF_COMP_BUF_SZ);

            ret = darshan_log_dzunload(fd, map_p);
            if(ret < 0)
                return(-1);

            z_strmp->avail_out = DARSHAN_DEF_COMP_BUF_SZ;
            z_strmp->next_out = state->dz.buf;
        }

        tmp_in_bytes = z_strmp->total_in;
        tmp_out_bytes = z_strmp->total_out;
        ret = deflate(z_strmp, Z_NO_FLUSH);
        if(ret != Z_OK)
        {
            fprintf(stderr, "Error: unable to deflate darshan log data.\n");
            return(-1);
        }
        total_bytes += (z_strmp->total_in - tmp_in_bytes);
        state->dz.size += (z_strmp->total_out - tmp_out_bytes);
    }

    return(total_bytes);
}

static int darshan_log_libz_flush(darshan_fd fd, int region_id)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    int tmp_out_bytes;
    struct darshan_log_map *map_p;
    z_stream *z_strmp = (z_stream *)state->dz.strm;

    assert(z_strmp);

    if(region_id == DARSHAN_JOB_REGION_ID)
        map_p = &(fd->job_map);
    else if(region_id == DARSHAN_REC_MAP_REGION_ID)
        map_p = &(fd->rec_map);
    else
        map_p = &(fd->mod_map[region_id]);

    /* make sure deflate finishes this stream */
    z_strmp->avail_in = 0;
    z_strmp->next_in = NULL;
    do
    {
        tmp_out_bytes = z_strmp->total_out;
        ret = deflate(z_strmp, Z_FINISH);
        if(ret < 0)
        {
            fprintf(stderr, "Error: unable to deflate darshan log data.\n");
            return(-1);
        }
        state->dz.size += (z_strmp->total_out - tmp_out_bytes);

        if(state->dz.size)
        {
            /* flush to file */
            if(darshan_log_dzunload(fd, map_p) < 0)
                return(-1);

            z_strmp->avail_out = DARSHAN_DEF_COMP_BUF_SZ;
            z_strmp->next_out = state->dz.buf;
        }
    } while (ret != Z_STREAM_END);

    deflateReset(z_strmp);
    return(0);
}

#ifdef HAVE_LIBBZ2

static int darshan_log_bzip2_read(darshan_fd fd, int region_id, void *buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    int total_bytes = 0;
    int tmp_out_bytes;
    struct darshan_log_map map;
    bz_stream *bz_strmp = (bz_stream *)state->dz.strm;

    assert(bz_strmp);

    /* if new log region, we reload buffers and clear eor flag */
    if(region_id != state->dz.prev_reg_id)
    {
        bz_strmp->avail_in = 0;
        state->dz.eor = 0;
        state->dz.prev_reg_id = region_id;
    }

    if(region_id == DARSHAN_JOB_REGION_ID)
        map = fd->job_map;
    else if(region_id == DARSHAN_REC_MAP_REGION_ID)
        map = fd->rec_map;
    else
        map = fd->mod_map[region_id];

    bz_strmp->avail_out = len;
    bz_strmp->next_out = buf;

    /* we just decompress until the output buffer is full, assuming there
     * is enough compressed data in file to satisfy the request size.
     */
    while(bz_strmp->avail_out)
    {
        /* check if we need more compressed data */
        if(bz_strmp->avail_in == 0)
        {
            /* if the eor flag is set, clear it and return -- future
             * reads of this log region will restart at the beginning
             */
            if(state->dz.eor)
            {
                state->dz.eor = 0;
                break;
            }

            /* read more data from input file */
            ret = darshan_log_dzload(fd, map);
            if(ret < 0)
                return(-1);
            assert(state->dz.size > 0);

            bz_strmp->avail_in = state->dz.size;
            bz_strmp->next_in = (char *)state->dz.buf;
        }

        tmp_out_bytes = bz_strmp->total_out_lo32;
        ret = BZ2_bzDecompress(bz_strmp);
        if(ret != BZ_OK && ret != BZ_STREAM_END)
        {
            fprintf(stderr, "Error: unable to decompress darshan log data.\n");
            return(-1);
        }
        total_bytes += (bz_strmp->total_out_lo32 - tmp_out_bytes);

        /* reset the decompression if we encountered end of stream */
        if(ret == BZ_STREAM_END)
        {
            BZ2_bzDecompressEnd(bz_strmp);
            BZ2_bzDecompressInit(bz_strmp, 1, 0);
        }
    }

    return(total_bytes);
}

static int darshan_log_bzip2_write(darshan_fd fd, int region_id, void *buf, int len)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    int total_bytes = 0;
    int tmp_in_bytes;
    int tmp_out_bytes;
    struct darshan_log_map *map_p;
    bz_stream *bz_strmp = (bz_stream *)state->dz.strm;

    assert(bz_strmp);

    /* if new log region, finish prev region's zstream and flush to log file */
    if(region_id != state->dz.prev_reg_id)
    {
        /* error out if the region we are writing to precedes the previous
         * region we wrote -- we shouldn't be moving backwards in the log
         */
        if(region_id < state->dz.prev_reg_id)
            return(-1);

        if(state->dz.prev_reg_id != DARSHAN_HEADER_REGION_ID)
        {
            ret = darshan_log_bzip2_flush(fd, state->dz.prev_reg_id);
            if(ret < 0)
                return(-1);
        }

        state->dz.prev_reg_id = region_id;
    }

    if(region_id == DARSHAN_JOB_REGION_ID)
        map_p = &(fd->job_map);
    else if(region_id == DARSHAN_REC_MAP_REGION_ID)
        map_p = &(fd->rec_map);
    else
        map_p = &(fd->mod_map[region_id]);

    bz_strmp->avail_in = len;
    bz_strmp->next_in = buf;

    /* compress input data until none left */
    while(bz_strmp->avail_in)
    {
        /* if we are out of output, flush to log file */
        if(bz_strmp->avail_out == 0)
        {
            assert(state->dz.size == DARSHAN_DEF_COMP_BUF_SZ);

            ret = darshan_log_dzunload(fd, map_p);
            if(ret < 0)
                return(-1);

            bz_strmp->avail_out = DARSHAN_DEF_COMP_BUF_SZ;
            bz_strmp->next_out = (char *)state->dz.buf;
        }

        tmp_in_bytes = bz_strmp->total_in_lo32;
        tmp_out_bytes = bz_strmp->total_out_lo32;
        ret = BZ2_bzCompress(bz_strmp, BZ_RUN);
        if(ret != BZ_RUN_OK)
        {
            fprintf(stderr, "Error: unable to compress darshan log data.\n");
            return(-1);
        }
        total_bytes += (bz_strmp->total_in_lo32 - tmp_in_bytes);
        state->dz.size += (bz_strmp->total_out_lo32 - tmp_out_bytes);
    }

    return(total_bytes);
}

static int darshan_log_bzip2_flush(darshan_fd fd, int region_id)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    int tmp_out_bytes;
    struct darshan_log_map *map_p;
    bz_stream *bz_strmp = (bz_stream *)state->dz.strm;

    assert(bz_strmp);

    if(region_id == DARSHAN_JOB_REGION_ID)
        map_p = &(fd->job_map);
    else if(region_id == DARSHAN_REC_MAP_REGION_ID)
        map_p = &(fd->rec_map);
    else
        map_p = &(fd->mod_map[region_id]);

    /* make sure deflate finishes this stream */
    bz_strmp->avail_in = 0;
    bz_strmp->next_in = NULL;
    do
    {
        tmp_out_bytes = bz_strmp->total_out_lo32;
        ret = BZ2_bzCompress(bz_strmp, BZ_FINISH);
        if(ret < 0)
        {
            fprintf(stderr, "Error: unable to compress darshan log data.\n");
            return(-1);
        }
        state->dz.size += (bz_strmp->total_out_lo32 - tmp_out_bytes);

        if(state->dz.size)
        {
            /* flush to file */
            if(darshan_log_dzunload(fd, map_p) < 0)
                return(-1);

            bz_strmp->avail_out = DARSHAN_DEF_COMP_BUF_SZ;
            bz_strmp->next_out = (char *)state->dz.buf;
        }
    } while (ret != BZ_STREAM_END);

    
    BZ2_bzCompressEnd(bz_strmp);
    BZ2_bzCompressInit(bz_strmp, 9, 1, 30);
    return(0);
}

#endif

static int darshan_log_dzload(darshan_fd fd, struct darshan_log_map map)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    unsigned int remaining;
    unsigned int read_size;
    unsigned int read_so_far = 0;

    state->dz.size = 0;

    /* seek to the appropriate portion of the log file, if out of range */
    if((state->pos < map.off) || (state->pos >= (map.off + map.len)))
    {
        ret = darshan_log_seek(fd, map.off);
        if(ret < 0)
        {
            fprintf(stderr, "Error: unable to seek in darshan log file.\n");
            return(-1);
        }
    }

    /* read more compressed data from file to staging buffer */
    remaining = (map.off + map.len) - state->pos;
    read_size = (remaining > DARSHAN_DEF_COMP_BUF_SZ) ?
        DARSHAN_DEF_COMP_BUF_SZ : remaining;
    do
    {
        ret = darshan_log_read(fd, state->dz.buf + read_so_far,
            read_size - read_so_far);
        if(ret <= 0)
            break;
        read_so_far += ret;
    } while(read_so_far < read_size);
    if(ret < 0)
    {
        fprintf(stderr, "Error: unable to read compressed data from file.\n");
        return(-1);
    }
    if((read_size == remaining) || (ret == 0))
    {
        state->dz.eor = 1;
    }

    state->dz.size = read_size;
    return(0);
}

static int darshan_log_dzunload(darshan_fd fd, struct darshan_log_map *map_p)
{
    struct darshan_fd_int_state *state = fd->state;
    int ret;
    unsigned int write_so_far = 0;

    /* initialize map structure for this log region */
    if(map_p->off == 0)
        map_p->off = state->pos;

    /* write more compressed data from staging buffer to file */
    do
    {
        ret = darshan_log_write(fd, state->dz.buf + write_so_far,
            state->dz.size - write_so_far);
        if(ret <= 0)
        {
            fprintf(stderr, "Error: unable to write compressed data to file.\n");
            return(-1);
        }
        write_so_far += ret;
    } while(write_so_far < state->dz.size);

    map_p->len += state->dz.size;
    state->dz.size = 0;
    return (0);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
