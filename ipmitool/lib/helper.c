/*
 * Copyright (c) 2003 Sun Microsystems, Inc.  All Rights Reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistribution of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * Redistribution in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of Sun Microsystems, Inc. or the names of
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * 
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * SUN MICROSYSTEMS, INC. ("SUN") AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * SUN OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF SUN HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * 
 * You acknowledge that this software is not designed or intended for use
 * in the design, construction, operation or maintenance of any nuclear
 * facility.
 */

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <ipmitool/helper.h>
#include <ipmitool/log.h>

extern int verbose;

uint32_t buf2long(uint8_t * buf)
{
	return (uint32_t)(buf[3] << 24 | buf[2] << 16 | buf[1] << 8 | buf[0]);
}

uint16_t buf2short(uint8_t * buf)
{
	return (uint16_t)(buf[1] << 8 | buf[0]);
}

const char * buf2str(uint8_t * buf, int len)
{
	static char str[1024];
	int i;

	if (len <= 0 || len > 1024)
		return NULL;

	memset(str, 0, 1024);

	for (i=0; i<len; i++)
		sprintf(str+i+i, "%2.2x", buf[i]);

	str[len*2] = '\0';

	return (const char *)str;
}

void printbuf(const uint8_t * buf, int len, const char * desc)
{
	int i;

	if (len <= 0)
		return;

	if (verbose < 1)
		return;

	fprintf(stderr, "%s (%d bytes)\n", desc, len);
	for (i=0; i<len; i++) {
		if (((i%16) == 0) && (i != 0))
			fprintf(stderr, "\n");
		fprintf(stderr, " %2.2x", buf[i]);
	}
	fprintf(stderr, "\n");
}

const char * val2str(uint16_t val, const struct valstr *vs)
{
	static char un_str[16];
	int i = 0;

	while (vs[i].str != NULL) {
		if (vs[i].val == val)
			return vs[i].str;
		i++;
	}

	memset(un_str, 0, 16);
	snprintf(un_str, 16, "Unknown (0x%02x)", val);

	return un_str;
}

uint16_t str2val(const char *str, const struct valstr *vs)
{
	int i = 0;

	while (vs[i].str != NULL) {
		if (strncasecmp(vs[i].str, str, __maxlen(str, vs[i].str)) == 0)
			return vs[i].val;
		i++;
	}

	return vs[i].val;
}

/* ipmi_csum  -  calculate an ipmi checksum
 *
 * @d:		buffer to check
 * @s:		position in buffer to start checksum from
 */
uint8_t
ipmi_csum(uint8_t * d, int s)
{
	uint8_t c = 0;
	for (; s > 0; s--, d++)
		c += *d;
	return -c;
}

/* ipmi_open_file  -  safely open a file for reading or writing
 *
 * @file:	filename
 * @rw:		read-write flag, 1=write
 *
 * returns pointer to file handler on success
 * returns NULL on error
 */
FILE *
ipmi_open_file(const char * file, int rw)
{
	struct stat st1, st2;
	FILE * fp;

	/* verify existance */
	if (lstat(file, &st1) < 0) {
		if (rw) {
			/* does not exist, ok to create */
			fp = fopen(file, "w");
			if (fp == NULL) {
				lperror(LOG_ERR, "Unable to open file %s "
					"for write", file);
				return NULL;
			}
			/* created ok, now return the descriptor */
			return fp;
		} else {
			lprintf(LOG_ERR, "File %s does not exist", file);
			return NULL;
		}
	}

	/* it exists - only regular files, not links */
	if (S_ISREG(st1.st_mode) == 0) {
		lprintf(LOG_ERR, "File %s has invalid mode: %d",
			file, st1.st_mode);
		return NULL;
	}

	/* allow only files with 1 link (itself) */
	if (st1.st_nlink != 1) {
		lprintf(LOG_ERR, "File %s has invalid link count: %d != 1",
		       file, (int)st1.st_nlink);
		return NULL;
	}

	fp = fopen(file, rw ? "w+" : "r");
	if (fp == NULL) {
		lperror(LOG_ERR, "Unable to open file %s", file);
		return NULL;
	}

	/* stat again */
	if (fstat(fileno(fp), &st2) < 0) {
		lperror(LOG_ERR, "Unable to stat file %s", file);
		fclose(fp);
		return NULL;
	}

	/* verify inode */
	if (st1.st_ino != st2.st_ino) {
		lprintf(LOG_ERR, "File %s has invalid inode: %d != %d",
			file, st1.st_ino, st2.st_ino);
		fclose(fp);
		return NULL;
	}

	/* verify owner */
	if (st1.st_uid != st2.st_uid) {
		lprintf(LOG_ERR, "File %s has invalid user id: %d != %d",
			file, st1.st_uid, st2.st_uid);
		fclose(fp);
		return NULL;
	}

	/* verify inode */
	if (st2.st_nlink != 1) {
		lprintf(LOG_ERR, "File %s has invalid link count: %d != 1",
			file, st2.st_nlink);
		fclose(fp);
		return NULL;
	}

	return fp;
}

void
ipmi_start_daemon(void)
{
	pid_t pid;
	int fd;
#ifdef SIGHUP
	sigset_t sighup;
#endif

	/* if we are started from init no need to become daemon */
	if (getppid() == 1)
		return;

#ifdef SIGHUP
	sigemptyset(&sighup);
	sigaddset(&sighup, SIGHUP);
	if (sigprocmask(SIG_UNBLOCK, &sighup, NULL) < 0)
		fprintf(stderr, "ERROR: could not unblock SIGHUP signal\n");
	signal(SIGHUP, SIG_IGN);
#endif
#ifdef SIGTTOU
	signal(SIGTTOU, SIG_IGN);
#endif
#ifdef SIGTTIN
	signal(SIGTTIN, SIG_IGN);
#endif
#ifdef SIGQUIT
	signal(SIGQUIT, SIG_IGN);
#endif
#ifdef SIGTSTP
	signal(SIGTSTP, SIG_IGN);
#endif

	pid = (pid_t) fork();
	if (pid < 0 || pid > 0)
		exit(0);
	
#if defined(SIGTSTP) && defined(TIOCNOTTY)
	if (setpgid(0, getpid()) == -1)
		exit(1);
	if ((fd = open(_PATH_TTY, O_RDWR)) >= 0) {
		ioctl(fd, TIOCNOTTY, NULL);
		close(fd);
	}
#else
	if (setpgrp() == -1)
		exit(1);
	pid = (pid_t) fork();
	if (pid < 0 || pid > 0)
		exit(0);
#endif

	chdir("/");
	umask(0);

	for (fd=0; fd<64; fd++)
		close(fd);

	open("/dev/null", O_RDWR);
	dup(0);
	dup(0);
}

