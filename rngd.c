/*
 * rngd.c -- Random Number Generator daemon
 *
 * rngd reads data from a hardware random number generator, verifies it
 * looks like random data, and adds it to /dev/random's entropy store.
 *
 * In theory, this should allow you to read very quickly from
 * /dev/random; rngd also adds bytes to the entropy store periodically
 * when it's full, which makes predicting the entropy store's contents
 * harder.
 *
 * Copyright (C) 2001 Philipp Rumpf
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335  USA
 */

#define _GNU_SOURCE

#ifndef HAVE_CONFIG_H
#error Invalid or missing autoconf build environment
#endif

#include "rng-tools-config.h"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <argp.h>
#include <syslog.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

#include "rngd.h"
#include "fips.h"
#include "exits.h"
#include "rngd_entsource.h"
#include "rngd_linux.h"

/*
 * Globals
 */
int kent_pool_size;

/* Background/daemon mode */
bool am_daemon = false;			/* True if we went daemon */
bool msg_squash = false;		/* True if we want no messages on the console */
bool quiet = false;			/* True if we want no console output at all */
volatile bool server_running = true;	/* set to false, to stop daemon */
bool do_reseed = false;			/* force a reseed event */
bool ignorefail = false; /* true if we ignore MAX_RNG_FAILURES */

/* Command line arguments and processing */
const char *rngd_program_version =
	"rngd " VERSION "\n"
	"Copyright 2001-2004 Jeff Garzik\n"
	"Copyright 2017 Neil Horman\n"
	"Copyright (c) 2001 by Philipp Rumpf\n"
	"This is free software; see the source for copying conditions.  There is NO "
	"warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.";

const char *argp_program_bug_address = PACKAGE_BUGREPORT;

static char doc[] =
	"Check and feed random data from hardware device to kernel entropy pool.\n";

static struct argp_option options[] = {
	{ "debug", 'd', 0, 0, "Enable debug output" },

	{ "foreground",	'f', 0, 0, "Do not fork and become a daemon" },

	{ "ignorefail", 'i', 0, 0, "Ignore repeated fips failures" },

	{ "background", 'b', 0, 0, "Become a daemon (default)" },

	{ "exclude", 'x', "n", 0, "Disable the numbered entropy source specified" },

	{ "include", 'n', "n", 0, "Enable the numbered entropy source specified" },

	{ "list", 'l', 0, 0, "List the operational entropy sources on this system and exit" },

	{ "option", 'O', "options", 0, "rng specific options in the form source:key:value"},

	{ "random-device", 'o', "file", 0,
	  "Kernel device used for random number output (default: /dev/random)" },

	{ "rng-device", 'r', "file", 0,
	  "Kernel device used for random number input (default: /dev/hwrng)" },

	{ "test", 't', 0, 0, "Enter test mode and report entropy production rates" },

	{ "pid-file", 'p', "file", 0,
	  "File used for recording daemon PID, and multiple exclusion (default: /var/run/rngd.pid)" },

	{ "random-step", 's', "nnn", 0,
	  "Number of bytes written to random-device at a time (default: 64)" },

	{ "fill-watermark", 'W', "n", 0,
	  "Do not stop feeding entropy to random-device until at least n bits of entropy are available in the pool (default: 3/4 of poolsize), 0 <= n <= 4096" },

	{ "quiet", 'q', 0, 0, "Suppress all messages" },

	{ "version" ,'v', 0, 0, "List rngd version" },

	{ "entropy-count", 'e', "n", 0, "Number of entropy bits to support (default: 8), 1 <= n <= 8" },

	{ "force-reseed", 'R', "n", 0, "Time in seconds to force adding entropy to the random device" },

	{ "drop-privileges", 'D', "user:group", 0, "Drop privileges to a user and group specified" },

	{ 0 },
};

static struct arguments default_arguments = {
	.random_name	= "/dev/random",
	.pid_file	= "/var/run/rngd.pid",
	.random_step	= 64,
	.fill_watermark = -1,
	.daemon		= true,
	.test		= false,
	.list		= false,
	.ignorefail	= false,
	.entropy_count	= 8,
	.force_reseed	= 60 * 5,
	.drop_privs = false,
};
struct arguments *arguments = &default_arguments;

static unsigned long ent_gathered = 0;
static unsigned long test_iterations = 0;
static double sum_entropy = 0;
static struct timespec start_test, end_test;
static bool test_running = false;

static enum {
	ENT_HWRNG = 0,
	ENT_TPM = 1,
	ENT_RDRAND,
	ENT_DARN,
	ENT_RNDR,
	ENT_NISTBEACON,
	ENT_JITTER,
	ENT_PKCS11,
	ENT_RTLSDR,
	ENT_QRYPT,
	ENT_MAX
} entropy_indexes __attribute__((used));


static struct rng_option drng_options[] = {
	[DRNG_OPT_AES] = {
		.key = "use_aes",
		.type = VAL_INT,
		.int_val = 0,
	},
	{
		.key = NULL,
	},
};

static struct rng_option darn_options[] = {
	[DARN_OPT_AES] = {
		.key = "use_aes",
		.type = VAL_INT,
		.int_val = 1,
	},
	{
		.key = NULL,
	}
};

static struct rng_option jitter_options[] = {
	[JITTER_OPT_THREADS] = {
		.key = "thread_count",
		.type = VAL_INT,
		.int_val = 4,
	},
	[JITTER_OPT_BUF_SZ] = {
		.key = "buffer_size",
		.type = VAL_INT,
		.int_val = 16535,
	},
	[JITTER_OPT_REFILL] = {
		.key = "refill_thresh",
		.type = VAL_INT,
		.int_val = 16535,
	},
	[JITTER_OPT_RETRY_COUNT] = {
		.key = "retry_count",
		.type = VAL_INT,
		.int_val = 1,
	},
	[JITTER_OPT_RETRY_DELAY] = {
		.key = "retry_delay",
		.type = VAL_INT,
		.int_val = -1,
	},
	[JITTER_OPT_USE_AES] = {
		.key = "use_aes",
		.type = VAL_INT,
		.int_val = 1,
	},
	[JITTER_OPT_FORCE_INT_TIMER] = {
		.key = "force_soft_timer",
		.type = VAL_INT,
		.int_val = 0,
	},
	{
		.key = NULL,
	}
};

#ifndef DEFAULT_PKCS11_ENGINE
#define DEFAULT_PKCS11_ENGINE "/usr/lib64/opensc-pkcs11.so"
#endif

static struct rng_option pkcs11_options[] = {
	[PKCS11_OPT_ENGINE] = {
		.key = "engine_path",
		.type = VAL_STRING,
		.str_val = DEFAULT_PKCS11_ENGINE,
	},
	[PKCS11_OPT_CHUNK] = {
		.key = "chunk_size",
		.type = VAL_INT,
		.int_val = 1,
	},
	{
		.key = NULL,
	}
};

static struct rng_option nist_options[] = {
	[NIST_OPT_USE_AES] = {
		.key = "use_aes",
		.type = VAL_INT,
		.int_val = 1,
	},
	{
		.key = NULL,
	}
};

static struct rng_option rtlsdr_options[] = {
	[RTLSDR_OPT_DEVID] = {
		.key = "device_id",
		.type = VAL_INT,
		.int_val = 0,
	},
	[RTLSDR_OPT_FREQ_MIN] = {
		.key = "freq_min",
		.type = VAL_INT,
		.int_val = 90000000,
	},
	[RTLSDR_OPT_FREQ_MAX] = {
		.key = "freq_max",
		.type = VAL_INT,
		.int_val = 110000000,
	},
	[RTLSDR_OPT_SRATE_MIN] = {
		.key = "sample_min",
		.type = VAL_INT,
		.int_val = 1000000,
	},
	[RTLSDR_OPT_SRATE_MAX] = {
		.key = "sample_max",
		.type = VAL_INT,
		.int_val = 2800000,
	},
	{
		.key = NULL,
	}
};

static struct rng_option qrypt_options[] = {
	[QRYPT_OPT_TOKEN_FILE] = {
		.key = "tokenfile",
		.type = VAL_STRING,
		.str_val = "/etc/qrypt.token",
	},
	{
		.key = NULL,
	}
};

static struct rng entropy_sources[ENT_MAX] = {
	/* Note, the special char dev must be the first entry */
	{
		.rng_name	= "Hardware RNG Device",
		.rng_sname	= "hwrng",
		.rng_fname      = "/dev/hwrng",
		.rng_fd	 = -1,
		.flags		= { 0 },
		.xread	  = xread,
		.init	   = init_entropy_source,
		.rng_options	= NULL,
	},
	/* must be at index 1 */
	{
		.rng_name	= "TPM RNG Device",
		.rng_sname	= "tpm",
		.rng_fname      = "/dev/tpm0",
		.rng_fd	 = -1,
		.flags		= { 0 },
		.xread	  = xread_tpm,
		.init	   = init_tpm_entropy_source,
		.rng_options	= NULL,
		.disabled	= true,
	},
	{
		.rng_name       = "Intel RDRAND Instruction RNG",
		.rng_sname	= "rdrand",
		.rng_fd	 = -1,
		.flags		= { 0 },
#ifdef HAVE_RDRAND
		.xread	  = xread_drng,
		.init	   = init_drng_entropy_source,
#else
		.disabled	= true,
#endif
		.rng_options	= drng_options,
	},
	{
		.rng_name       = "Power9 DARN Instruction RNG",
		.rng_sname	= "darn",
		.rng_fd	 = -1,
		.flags		= { 0 },
#ifdef HAVE_DARN
		.xread	  = xread_darn,
		.init	   = init_darn_entropy_source,
#else
		.disabled	= true,
#endif
		.rng_options	= darn_options,
	},
	{
		.rng_name       = "ARM v8.5 RNDR Instruction RNG",
		.rng_sname	= "rndr",
		.rng_fd	 = -1,
		.flags		= { 0 },
#ifdef HAVE_RNDR
		.xread	  = xread_rndr,
		.init	   = init_rndr_entropy_source,
#else
		.disabled	= true,
#endif
		.rng_options	= drng_options,
	},
	{
		.rng_name	= "NIST Network Entropy Beacon",
		.rng_sname	= "nist",
		.rng_fd		= -1,
		.flags		= {
			.slow_source = 1,
		},
#ifdef HAVE_NISTBEACON
		.xread		= xread_nist,
		.init		= init_nist_entropy_source,
#endif
		.disabled	= true,
		.rng_options	= nist_options,
	},
	{
		.rng_name	= "JITTER Entropy generator",
		.rng_sname	= "jitter",
		.rng_fd		= -1,
		.flags		= {
			.slow_source = 1,
		},
#ifdef HAVE_JITTER
		.xread		= xread_jitter,
		.init		= init_jitter_entropy_source,
		.close		= close_jitter_entropy_source,
#else
		.disabled	= true,
#endif
		.rng_options	= jitter_options,
	},
	{
		.rng_name	= "PKCS11 Entropy generator",
		.rng_sname	= "pkcs11",
		.rng_fd		= -1,
		.flags		= {
			.slow_source = 1,
		},
#ifdef HAVE_PKCS11
		.xread		= xread_pkcs11,
		.init		= init_pkcs11_entropy_source,
		.close		= close_pkcs11_entropy_source,
#else
		.disabled	= true,
#endif
		.rng_options	= pkcs11_options,
	},
	{
		.rng_name       = "RTLSDR software defined radio generator",
		.rng_sname      = "rtlsdr",
		.rng_fd	 = -1,
		.flags	  = { 0 },
#ifdef HAVE_RTLSDR
		.xread	  = xread_rtlsdr,
		.init	   = init_rtlsdr_entropy_source,
		.close	  = close_rtlsdr_entropy_source,
#else
		.disabled       = true,
#endif
		.rng_options    = rtlsdr_options,
	},
	{
		.rng_name	= "Qrypt quantum entropy beacon",
		.rng_sname	= "qrypt",
		.rng_fd		= -1,
		.flags		= { 0 },
		.xread		= xread_qrypt,
		.init		= init_qrypt_entropy_source,
		.close		= close_qrypt_entropy_source,
		.disabled	= true,
		.rng_options	= qrypt_options,
	}

};

static int find_ent_src_idx_by_sname(const char *sname)
{
	int i;

	for (i = 0; i < ENT_MAX; i++) {
		if (!strncmp(sname, entropy_sources[i].rng_sname,
			strlen(entropy_sources[i].rng_sname)))
			return i;
	}

	return -1;
}

static int find_ent_src_idx(const char *name_idx)
{
	int idx;

	if (isalpha(name_idx[0])) {
		idx = find_ent_src_idx_by_sname(name_idx);
		if (idx == -1) {
			message(LOG_CONS|LOG_WARNING, "Unknown entropy source %s\n", name_idx);
			return -EINVAL;
		}
	} else {
		idx = strtoul(name_idx, NULL, 10);
		if ((idx == LONG_MAX) || (idx >= ENT_MAX)) {
			message(LOG_CONS|LOG_INFO, "option index out of range: %d\n", idx);
			return -ERANGE;
		}
		message(LOG_CONS|LOG_INFO, "Note, reference of entropy sources by index "
			"is deprecated, use entropy source short name instead\n");
	}

	return idx;
}

/*
 * command line processing
 */
static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	char *optkey;
	long int idx;
	long int val;
	char *strval;
	bool restore = false;
	char *search, *last_search;
	struct rng_option *options;

	switch(key) {
	case 'd':
		arguments->debug = true;
		break;
	case 'o':
		arguments->random_name = arg;
		break;
	case 'O':

		search = strchrnul(arg, ':');

		if (*search != '\0') {
			*search = '\0';
			restore = true;
		}
	
		idx = find_ent_src_idx(arg);
		if (idx < 0)
			return idx;
	
		if (restore == true)
			*search = ':';
	
		if (*search == '\0') {
			message(LOG_CONS|LOG_INFO, "Available options for %s (%s)\n",
				entropy_sources[idx].rng_name, entropy_sources[idx].rng_sname);
			options = entropy_sources[idx].rng_options;
			while (options && options->key) {
				if (options->type == VAL_INT)
					message(LOG_CONS|LOG_INFO, "key: [%s]\tdefault value: [%d]\n", options->key, options->int_val);
				else
					message(LOG_CONS|LOG_INFO, "key: [%s]\tdefault value: [%s]\n", options->key, options->str_val);
				options++;
			}
			return -ERANGE;
		}

		last_search = search = search + 1;
		search = strchr(search, ':');
		if (!search) {
			message(LOG_CONS|LOG_ERR, "Options tuple not specified correctly\n");
			return -EINVAL;
		}

		*search = '\0';
		optkey = strdupa(last_search);
		*search = ':';

		last_search = search + 1;
		strval = last_search;
		val = strtoul(last_search, NULL, 10);
		if (val == LONG_MAX) {
			message(LOG_CONS|LOG_INFO, "rng option was not parsable\n");
			return -ERANGE;
		}

		options = entropy_sources[idx].rng_options;
		while (options && options->key) {
			if (!strcmp(optkey, options->key)) {
				if (options->type == VAL_INT)
					options->int_val = val;
				else
					options->str_val = strdup(strval);

				return 0;
			}
			options++;
		}
		message(LOG_CONS|LOG_INFO, "Option %s not found for source idx %ld\n", optkey, idx);
		return -ERANGE;
		break;

	case 'x':
		idx = find_ent_src_idx(arg);
		if (idx < 0)
			return idx;

		entropy_sources[idx].disabled = true;
		message(LOG_CONS|LOG_INFO, "Disabling %ld: %s (%s)\n", idx,
			entropy_sources[idx].rng_name, entropy_sources[idx].rng_sname);
		break;
	case 'n':
		idx = find_ent_src_idx(arg);
		if (idx < 0)
			return idx;

		entropy_sources[idx].disabled = false;
		message(LOG_CONS|LOG_INFO, "Enabling %ld: %s (%s)\n", idx,
			entropy_sources[idx].rng_name, entropy_sources[idx].rng_sname);
		break;
	case 'l':
		arguments->list = true;
		break;
	case 'p':
		arguments->pid_file = arg;
		break;
	case 'r':
		entropy_sources[ENT_HWRNG].rng_fname = arg;
		break;
	case 'f':
		arguments->daemon = false;
		break;
	case 't':
		arguments->daemon = false;
		arguments->test = true;
		break;
	case 'b':
		arguments->daemon = true;
		break;
	case 'i':
		arguments->ignorefail = true;
		break;
	case 's':
		if (sscanf(arg, "%i", &arguments->random_step) == 0)
			argp_usage(state);
		break;
	case 'W': {
		int n;
		if ((sscanf(arg, "%i", &n) == 0) || (n < 0) || (n > 4096))
			argp_usage(state);
		else
			arguments->fill_watermark = n;
		break;
	}
	case 'q':
		quiet = true;
		break;
	case 'v':
		message(LOG_CONS|LOG_INFO, "%s\n", rngd_program_version);
		exit(0);
		break;
	case 'e': {
		int e;
		if ((sscanf(arg,"%i", &e) == 0) || (e < 0) || (e > 8))
			argp_usage(state);
		else
			arguments->entropy_count = e;
		break;
	}
	case 'R': {
		int R;
		if ((sscanf(arg,"%i", &R) == 0) || (R < 0))
			argp_usage(state);
		else
			arguments->force_reseed = R;
		break;
	}
	case 'D': {
		struct passwd *usrent;
		struct group *grpent;
		char *endptr;
		long int nuid, ngid;

		search = strchr(arg, ':');

		/* Check for corner cases */
		if (search == NULL) {
			message(LOG_CONS|LOG_ERR, "No colon found in user:group tuple\n");
			return -EINVAL;
		}
		if (search == arg || search[1] == '\0') {
			message(LOG_CONS|LOG_ERR, "No user or group name found in user:group tuple\n");
			return -EINVAL;
		}

		*search = '\0';

		/* Translate user argument into a user struct pointer.
		 * First, try to get it as specified. If that fails,
		 * try it as a number.
		 */
		usrent = getpwnam(arg);
		if (usrent == NULL) {
			/* Try as a number */
			nuid = strtol(arg, &endptr, 10);

			if (*endptr || !(usrent = getpwuid(nuid))) {
				message(LOG_CONS|LOG_ERR, "User '%s' not found\n", arg);
				*search = ':';
				return -EINVAL;
			}
		}
		*search = ':';

		/* Do the same with a group name or number */
		grpent = getgrnam(search + 1);
		if (grpent == NULL) {

			/* Try as a number */
			ngid = strtol(search + 1, &endptr, 10);
			if (*endptr || !(grpent = getgrgid(ngid))) {
				message(LOG_CONS|LOG_ERR, "Group '%s' not found\n", search + 1);
				return -EINVAL;
			}
		}

		/* Found both user and group in a system */
		arguments->drop_uid = usrent->pw_uid;
		arguments->drop_gid = grpent->gr_gid;
		arguments->drop_privs = true;

		message(LOG_CONS|LOG_DEBUG, "Trying to drop privileges to %s(%d)/%s(%d)\n",
			usrent->pw_name, usrent->pw_uid, grpent->gr_name, grpent->gr_gid);

		break;
	}
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static struct argp argp = { options, parse_opt, NULL, doc };


static int update_kernel_random(struct rng *rng, int random_step,
	unsigned char *buf, fips_ctx_t *fipsctx_in)
{
	unsigned char *p;
	int fips;
	int rc;

	fips = fips_run_rng_test(fipsctx_in, buf);
	if (fips && !arguments->ignorefail)
		return 1;

	for (p = buf; p + random_step <= &buf[FIPS_RNG_BUFFER_SIZE];
		 p += random_step) {
		if (!server_running)
			return 0;
		if (do_reseed) {
			do_reseed = false;
			alarm(arguments->force_reseed);
		}
		rc = random_add_entropy(p, random_step);
		if (rc == -1)
			return 1;
		message(LOG_DAEMON|LOG_DEBUG, "Added %d/%d bits entropy\n", rc, kent_pool_size);
		if (rc >= kent_pool_size-64) {
			message(LOG_DAEMON|LOG_DEBUG, "Pool full at %d, sleeping!\n",
				kent_pool_size);
			random_sleep();
		}
	}

	return 0;
}

static int random_test_sink(struct rng *rng, int random_step,
	unsigned char *buf, fips_ctx_t *fipsctx_in)
{
	if (!ent_gathered)
		alarm(1);
	ent_gathered += FIPS_RNG_BUFFER_SIZE;
	return 0;
}

static int drop_privileges(uid_t drop_uid, gid_t drop_gid)
{
	cap_value_t ioctl_caps[1] = { CAP_SYS_ADMIN };
	cap_t caps;
	uid_t curr_uid = geteuid();
	gid_t curr_gid = getegid(), supp_gid = drop_gid;

	/* We need CAP_SYS_ADMIN capability to perform privileged
	 * ioctl(2) operations on the /dev/random device (see random(4)).
	 */
	if (!CAP_IS_SUPPORTED(CAP_SYS_ADMIN)) {
		message(LOG_DAEMON|LOG_ERR, "Capability CAP_SYS_ADMIN is not present\n");
		return 1;
	}

	/* We wish to retain the capabilities across the identity change,
	 * so we need to tell the kernel. See prctl(2).
	 */
	if (prctl(PR_SET_KEEPCAPS, 1L)) {
		message(LOG_DAEMON|LOG_ERR, "Cannot keep capabilities after dropping privileges: %s\n",
			strerror(errno));
		return 1;
	}

	/* Actually try to drop privileges */
	if (setgroups(1, &supp_gid)) {
		message(LOG_DAEMON|LOG_ERR, "setgroups() failed: %s\n", strerror(errno));
		return 1;
	}

	if (setresgid(drop_gid, drop_gid, drop_gid)) {
		message(LOG_DAEMON|LOG_ERR, "setresgid() failed: %s\n", strerror(errno));
		return 1;
	}

	if (setresuid(drop_uid, drop_uid, drop_uid)) {
		message(LOG_DAEMON|LOG_ERR, "setresuid() failed: %s\n", strerror(errno));
		return 1;
	}

	/* Drop all the capabilities except CAP_SYS_ADMIN. We can do this only
	 * if CAP_SYS_ADMIN is present in the PERMITTED subset initially.
	 */
	caps = cap_init();
	if (caps == NULL) {
		message(LOG_DAEMON|LOG_ERR, "cap_init() failed: %s\n", strerror(errno));
		return 1;
	}

	/* We need CAP_SYS_ADMIN capability in the EFFECTIVE and PERMITTED subsets */
	if (cap_set_flag(caps, CAP_PERMITTED, 1, ioctl_caps, CAP_SET) ||
	    cap_set_flag(caps, CAP_EFFECTIVE, 1, ioctl_caps, CAP_SET)) {
		message(LOG_DAEMON|LOG_ERR, "Cannot manipulate capability data structure: %s\n",
			strerror(errno));
		cap_free(caps);
		return 1;
	}

	/* Above, we just manipulated the data structure describing the flags,
	 * not the capabilities themselves. So, set those capabilities now.
	 */
	if (cap_set_proc(caps)) {
		message(LOG_DAEMON|LOG_ERR, "Cannot set CAP_SYS_ADMIN capability: %s\n",
			strerror(errno));
		cap_free(caps);
		return 1;
	}

	/* Free capabilities data */
	if (cap_free(caps))
		message(LOG_DAEMON|LOG_DEBUG, "cap_free() failed: %s\n", strerror(errno));
		/* We can continue with this error */

	/* Tell the kernel we do not want to retain the capability over
	 * any further identity changes (be paranoid)
	 */
	if (prctl(PR_SET_KEEPCAPS, 0L)) {
		message(LOG_DAEMON|LOG_ERR, "prctl() failed: %s\n", strerror(errno));
		return 1;
	}

	/* Be paranoid, verify that the changes were successful.
	 * Fail if current user or group is not ones to drop to
	 * or if older user or group can be obtained.
	 */
	if (curr_gid != drop_gid && (getegid() != drop_gid || setegid(curr_gid) >= 0)) {
		message(LOG_DAEMON|LOG_ERR, "Group privileges drop was not successfull\n");
		return 1;
	}
	if (curr_uid != drop_uid && (geteuid() != drop_uid || seteuid(curr_uid) >= 0)) {
		message(LOG_DAEMON|LOG_ERR, "User privileges drop was not successfull\n");
		return 1;
	}
	/* The same check for supplemental groups */
	int ret = getgroups(1, &supp_gid);
	if (ret < 0 || ret > 1 || (ret == 1 && supp_gid != drop_gid)) {
		message(LOG_DAEMON|LOG_ERR, "Supplemental groups drop was not successfull\n");
		return 1;
	}

	message(LOG_DAEMON|LOG_INFO, "Process privileges have been dropped to %d:%d\n",
			geteuid(), getegid());
	return 0;
}

static void do_loop(int random_step)
{
	unsigned char buf[FIPS_RNG_BUFFER_SIZE];
	int no_work;
	bool work_done;
	int sources_left;
	int i;
	int retval;
	struct rng *iter;
	bool try_slow_sources = false;

	int (*random_add_fn)(struct rng *rng, int random_step,
		unsigned char *buf, fips_ctx_t *fipsctx_in);

	random_add_fn = arguments->test ? random_test_sink : update_kernel_random;

continue_trying:
	for (no_work = 0; no_work < 100; no_work = (work_done ? 0 : no_work+1)) {

		work_done = false;

		/*
		 * Exclude slow sources when faster sources are working well
		 * sources like jitterentropy can provide some entropy when needed
		 * but can actually hinder performance when large amounts of entropy are needed
		 * owing to the fact that they may block while generating said entropy
		 * So, lets prioritize the faster sources. Start by only trying to collect
		 * entropy from the fast sources, then iff that fails, start including the slower
		 * sources as well. Once we get some entropy, return to only using fast sources
		 */
		if (no_work)
			try_slow_sources = true;
		else
			try_slow_sources = false;

		for (i = 0; i < ENT_MAX; ++i)
		{
			int rc;
			/*message(LOG_CONS|LOG_INFO, "I is %d\n", i);*/
			iter = &entropy_sources[i];
			if (!try_slow_sources && iter->flags.slow_source)
				continue;

		retry_same:
			if (!server_running)
				return;

			if (iter->disabled)
				continue;	/* failed, no work */

			message(LOG_DAEMON|LOG_DEBUG, "Reading entropy from %s\n", iter->rng_name);

			retval = iter->xread(buf, sizeof buf, iter);
			if (retval)
				continue;	/* failed, no work */

			work_done = true;

			rc = random_add_fn(iter, random_step, buf, iter->fipsctx);

			if (rc == 0) {
				iter->success++;
				if (iter->success >= RNG_OK_CREDIT) {
					if (iter->failures)
						iter->failures--;
					iter->success = 0;
				}
				/* succeeded */
				continue;
			}

			iter->failures++;
			if (iter->failures <= MAX_RNG_FAILURES/4) {
				/* FIPS tests have false positives */
				goto retry_same;
			}

			if (iter->failures >= MAX_RNG_FAILURES && !ignorefail) {
				message(LOG_DAEMON|LOG_ERR,
				"too many FIPS failures, disabling entropy source\n");
				if (iter->close)
					iter->close(iter);
				iter->disabled = true;
			}
		}
	}

	/*
	 * No entropy source produced entropy in
	 * 100 rounds, disable anything that isn't
	 * flagged as a slow source
	 */
	sources_left = 0;
	for (i = 0; i < ENT_MAX; ++i) {
		iter = &entropy_sources[i];
		if (!iter->flags.slow_source && !iter->disabled) {
			message(LOG_DAEMON|LOG_WARNING, "Too Slow: Disabling %s\n",
				iter->rng_name);
			iter->disabled = 1;
		}
		if (!iter->disabled)
			sources_left++;
	}

	if (sources_left) {
		message(LOG_DAEMON|LOG_WARNING,
			"Entropy Generation is slow, consider tuning/adding sources\n");
		goto continue_trying;
	}

	message(LOG_DAEMON|LOG_ERR,
	"No entropy sources working, exiting rngd\n");
}

static void term_signal(int signo)
{
	server_running = false;
}

static void force_reseed_signal(int signo)
{
	do_reseed = true;
}

static void alarm_signal(int signo)
{
	double bits_gathered;

	if (!test_running) {
		clock_gettime(CLOCK_MONOTONIC, &start_test);
		test_running = true;
	} else {
		bits_gathered = ent_gathered * 8.0;
		message(LOG_CONS|LOG_INFO, "Entropy gathered: %.6e bits\n",
			bits_gathered);
		sum_entropy += bits_gathered;
		test_iterations++;
	}
	ent_gathered = 0;
	clock_gettime(CLOCK_MONOTONIC, &end_test);
}

static int discard_initial_data(struct rng *ent_src)
{
	/* Trash 32 bits of what is probably stale (non-random)
	 * initial state from the RNG.  For Intel's, 8 bits would
	 * be enough, but since AMD's generates 32 bits at a time...
	 *
	 * The kernel drivers should be doing this at device powerup,
	 * but at least up to 2.4.24, it doesn't. */
	unsigned char tempbuf[4];
	ent_src->xread(tempbuf, sizeof(tempbuf), ent_src);

	/* Return 32 bits of bootstrap data */
	ent_src->xread(tempbuf, sizeof(tempbuf), ent_src);

	return tempbuf[0] | (tempbuf[1] << 8) |
		(tempbuf[2] << 16) | (tempbuf[3] << 24);
}

void close_all_entropy_sources()
{
	struct rng *ent_src;
	int i;
	for (i=0; i < ENT_MAX; i++) {
		ent_src = &entropy_sources[i];
		if (ent_src->disabled == false)
			message_entsrc(ent_src, LOG_DAEMON|LOG_INFO, "Shutting down\n");
		if (ent_src->close && ent_src->disabled == false) {
			ent_src->close(ent_src);
			free(ent_src->fipsctx);
		}
	}
}

int main(int argc, char **argv)
{
	int i;
	int ent_sources = 0;
	pid_t pid_fd = -1;
	double test_time;
	struct rng *ent_src;

	openlog("rngd", 0, LOG_DAEMON);

	/* Parsing of commandline parameters */
	if (argp_parse(&argp, argc, argv, 0, 0, arguments) < 0)
		return 1;

	if (arguments->daemon && !arguments->list) {
		am_daemon = true;

		if (daemon(0, 0) < 0) {
			message(LOG_CONS|LOG_INFO, "can't daemonize: %s\n",
			strerror(errno));
			return 1;
		}

		/* require valid, locked PID file to proceed */
		pid_fd = write_pid_file(arguments->pid_file);
		if (pid_fd < 0)
			return 1;
	}

	if (arguments->list) {
		int found = 0;
		message(LOG_CONS|LOG_INFO, "Entropy sources that are available but disabled\n");
		for (i=0; i < ENT_MAX; i++)
			if (entropy_sources[i].init && entropy_sources[i].disabled == true) {
				found = 1;
				message(LOG_CONS|LOG_INFO, "%d: %s (%s)\n", i,
					entropy_sources[i].rng_name, entropy_sources[i].rng_sname);
				entropy_sources[i].failed_init = false;
			}
		if (!found)
			message(LOG_CONS|LOG_INFO, "None\n");
		msg_squash = true;
	} else
		message(LOG_DAEMON|LOG_INFO, "Initializing available sources\n");

	/* Init entropy sources */
	for (i=0; i < ENT_MAX; i++) {
		ent_src = &entropy_sources[i];
		if (ent_src->init && ent_src->disabled == false) {
			if (!ent_src->init(ent_src)) {
				ent_sources++;
				ent_src->fipsctx = malloc(sizeof(fips_ctx_t));
				fips_init(ent_src->fipsctx, discard_initial_data(ent_src));
				message_entsrc(ent_src, LOG_DAEMON|LOG_INFO, "Initialized\n");
				ent_src->failed_init = false;
			} else {
				message_entsrc(ent_src, LOG_DAEMON|LOG_ERR, "Initialization Failed\n");
				ent_src->disabled = true;
				ent_src->failed_init = true;
			}
		}
	}

	if (arguments->list) {
		int rc = 1;
		msg_squash = false;
		message(LOG_CONS|LOG_INFO, "Available and enabled entropy sources:\n");
		for (i=0; i < ENT_MAX; i++)
			if (entropy_sources[i].init && entropy_sources[i].disabled == false) {
				rc = 1;
				message(LOG_CONS|LOG_INFO, "%d: %s (%s)\n", i,
					entropy_sources[i].rng_name, entropy_sources[i].rng_sname);
			}
		message(LOG_CONS|LOG_INFO, "Available entropy sources that failed initalization:\n");
		for (i=0; i < ENT_MAX; i++)
			if (entropy_sources[i].init && entropy_sources[i].disabled == true && entropy_sources[i].failed_init == true) {
				rc = 1;
				message(LOG_CONS|LOG_INFO, "%d: %s (%s)\n", i,
					entropy_sources[i].rng_name, entropy_sources[i].rng_sname);
			}

		quiet = true;
		close_all_entropy_sources();
		return rc;
	}

	if (!ent_sources) {
		message(LOG_DAEMON|LOG_ERR,
			"Can't open any entropy source\n");
		message(LOG_DAEMON|LOG_ERR,
			"Maybe RNG device modules are not loaded\n");
		return 1;
	}
	/* Init entropy sink and open random device */
	init_kernel_rng(arguments->random_name);

	/* All privileged operations are completed,
	 * drop privileges if it was requested
	 */
	if (arguments->drop_privs &&
		drop_privileges(arguments->drop_uid, arguments->drop_gid))
		return 1;

	/*
	 * We always catch these to ensure that we gracefully shutdown
	 */
	signal(SIGINT, term_signal);
	signal(SIGTERM, term_signal);

	if (arguments->test) {
		message(LOG_CONS|LOG_INFO, "Entering test mode...no entropy will "
			"be delivered to the kernel\n");
		signal(SIGALRM, alarm_signal);
	} else {
		/*
		 * Use the alarm signal instead to wake us up on the force_reseed value
		 */
		if (arguments->force_reseed) {
			signal(SIGALRM, force_reseed_signal);
			alarm(arguments->force_reseed);
		}
	}

	if (arguments->ignorefail)
		ignorefail = true;

	do_loop(arguments->random_step);

	close_all_entropy_sources();

	if (arguments->test && test_iterations) {
		test_time = (end_test.tv_sec - start_test.tv_sec);
		test_time = ((test_time * NSECS_IN_SECOND) + (end_test.tv_nsec - start_test.tv_nsec)) / NSECS_IN_SECOND;

		if ((sum_entropy/test_time) >= MEGABITS) {
			message(LOG_CONS|LOG_INFO, "\nEntropy rate: %6.4g Mbits/sec averaged over %lu iterations for %6.4g seconds\n",
				(sum_entropy/test_time/MEGABITS), test_iterations, test_time);
		} else {
			message(LOG_CONS|LOG_INFO, "\nEntropy rate: %6.4g Kbits/sec averaged over %lu iterations for %6.4g seconds\n",
				(sum_entropy/test_time/KILOBITS), test_iterations, test_time);
		}
	}

	if (pid_fd >= 0)
		unlink(arguments->pid_file);

	return 0;
}
