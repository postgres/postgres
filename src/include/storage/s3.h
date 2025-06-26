#ifndef S3_H
#define S3_H

extern int S3CacheSizeMB;
extern int S3LocalDiskLimitMB;

void InitS3Async(void);
void S3ScheduleUpload(const char *path);
bool S3FetchFile(const char *path);

#endif /* S3_H */
