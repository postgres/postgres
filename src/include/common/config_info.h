/*
 * config_info.h
 *		Common code for pg_config output
 *
 *	Copyright (c) 2016-2020, PostgreSQL Global Development Group
 *
 *	src/include/common/config_info.h
 */
#ifndef COMMON_CONFIG_INFO_H
#define COMMON_CONFIG_INFO_H

typedef struct ConfigData
{
	char	   *name;
	char	   *setting;
} ConfigData;

extern ConfigData *get_configdata(const char *my_exec_path,
								  size_t *configdata_len);

#endif							/* COMMON_CONFIG_INFO_H */
