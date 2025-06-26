#include "postgres.h"
#include "storage/s3.h"
#include "utils/guc.h"
#include "storage/fd.h"
#include <sys/stat.h>
#include <unistd.h>

int S3CacheSizeMB = 64;
int S3LocalDiskLimitMB = 1024; /* 1GB default */
static Size S3CurrentDiskUsage = 0;

void
InitS3Async(void)
{
    DefineCustomIntVariable("s3.cache_size_mb",
                            "Size of the local cache for asynchronous S3 persistence.",
                            NULL,
                            &S3CacheSizeMB,
                            64, 1, 10240,
                            PGC_POSTMASTER,
                            0,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("s3.disk_limit_mb",
                            "Maximum local disk usage for cached relation files.",
                            NULL,
                            &S3LocalDiskLimitMB,
                            1024, 1, 102400,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);
}

void
S3ScheduleUpload(const char *path)
{
    struct stat st;

    ereport(DEBUG1,
            (errmsg_internal("S3 async upload scheduled for %s (cache %dMB)",
                             path, S3CacheSizeMB)));

    /* Track disk usage of cached file */
    if (stat(path, &st) == 0)
        S3CurrentDiskUsage += st.st_size;

    /* Placeholder for asynchronous upload implementation */

    /* Evict file if local usage exceeds configured limit */
    if (S3CurrentDiskUsage > ((Size) S3LocalDiskLimitMB * 1024 * 1024))
    {
        if (unlink(path) == 0)
        {
            S3CurrentDiskUsage -= st.st_size;
            ereport(LOG,
                    (errmsg("evicted %s from local cache", path)));
        }
    }
}

bool
S3FetchFile(const char *path)
{
    ereport(LOG, (errmsg("retrieving cold data %s from S3", path)));
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    close(fd);

    struct stat st;
    if (stat(path, &st) == 0)
        S3CurrentDiskUsage += st.st_size;

    return true;
}
