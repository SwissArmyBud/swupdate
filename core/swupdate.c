/*
 * (C) Copyright 2012-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>
#include <fnmatch.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include "bsdqueue.h"
#include "cpiohdr.h"
#include "util.h"
#include "swupdate.h"
#include "parsers.h"
#include "network_interface.h"
#include "handler.h"
#include "installer.h"
#ifdef CONFIG_MTD
#include "flash.h"
#endif
#include "lua_util.h"
#include "mongoose_interface.h"
#include "download_interface.h"
#include "network_ipc.h"
#include "sslapi.h"
#include "suricatta/suricatta.h"
#include "progress.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "pctl.h"
#include "bootloader.h"

#define MODULE_NAME	"swupdate"

static pthread_t network_daemon;

/* Tree derived from the configuration file */
static struct swupdate_cfg swcfg;

#ifdef CONFIG_MTD
/* Global MTD configuration */
static struct flash_description flashdesc;

struct flash_description *get_flash_info(void) {
	return &flashdesc;
}
#endif

static struct option long_options[] = {
	{"verbose", no_argument, NULL, 'v'},
	{"image", required_argument, NULL, 'i'},
	{"file", required_argument, NULL, 'f'},
	{"loglevel", required_argument, NULL, 'l'},
	{"syslog", no_argument, NULL, 'L' },
	{"select", required_argument, NULL, 'e'},
#ifdef CONFIG_SIGNED_IMAGES
	{"key", required_argument, NULL, 'k'},
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
	{"key-aes", required_argument, NULL, 'K'},
#endif
#ifdef CONFIG_MTD
	{"blacklist", required_argument, NULL, 'b'},
#endif
	{"help", no_argument, NULL, 'h'},
#ifdef CONFIG_HW_COMPATIBILITY
	{"hwrevision", required_argument, NULL, 'H'},
#endif
#ifdef CONFIG_DOWNLOAD
	{"download", required_argument, NULL, 'd'},
#endif
#ifdef CONFIG_SURICATTA
	{"suricatta", required_argument, NULL, 'u'},
#endif
#ifdef CONFIG_WEBSERVER
	{"webserver", required_argument, NULL, 'w'},
#endif
	{"check", no_argument, NULL, 'c'},
	{"postupdate", required_argument, NULL, 'p'},
	{NULL, 0, NULL, 0}
};

int loglevel = ERRORLEVEL;

static void usage(char *programname)
{
	fprintf(stdout, "%s (compiled %s)\n", programname, __DATE__);
	fprintf(stdout, "Usage %s [OPTION]\n",
			programname);
	fprintf(stdout,
		" -f, --file <filename>          : configuration file to use\n"
#ifdef CONFIG_MTD
		" -b, --blacklist <list of mtd>  : MTDs that must not be scanned for UBI\n"
#endif
		" -p, --postupdate               : execute post-update command\n"
		" -e, --select <software>,<mode> : Select software images set and source\n"
		"                                  Ex.: stable,main\n"
		" -i, --image <filename>         : Software to be installed\n"
		" -l, --loglevel <level>         : logging level\n"
		" -L, --syslog                   : enable syslog logger\n"
#ifdef CONFIG_SIGNED_IMAGES
		" -k, --key <public key file>    : file with public key to verify images\n"
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
		" -K, --key-aes <key file>       : the file contains the symmetric key to be used\n"
		"                                  to decrypt images\n"
#endif
		" -v, --verbose                  : be verbose, set maximum loglevel\n"
#ifdef CONFIG_HW_COMPATIBILITY
		" -H, --hwrevision <board>:<rev> : Set hardware revision\n"
#endif
		" -c, --check                    : check image and exit, use with -i <filename>\n"
		" -h, --help                     : print this help and exit\n"
		);
#ifdef CONFIG_DOWNLOAD
	fprintf(stdout,
		" -d, --download [OPTIONS]       : Parameters to be passed to the downloader\n");
	download_print_help();
#endif
#ifdef CONFIG_SURICATTA
	fprintf(stdout,
		" -u, --suricatta [OPTIONS]      : Parameters to be passed to suricatta\n");
	suricatta_print_help();
#endif
#ifdef CONFIG_WEBSERVER
	fprintf(stdout,
		" -w, --webserver [OPTIONS]      : Parameters to be passed to webserver\n");
	mongoose_print_help();
#endif
}

static int check_provided(struct imglist *list)
{
	int ret = 0;
	struct img_type *p;

	for (p = list->lh_first; p != NULL;
		p = p->next.le_next) {
		if (!p->provided) {
			ERROR("Requested file not found in image: %s", \
				p->fname);
			ret = -1;
		}
	}

	return ret;
}

struct swupdate_cfg *get_swupdate_cfg(void) {
	return &swcfg;
}

/*
 * Extract board and revision number from command line
 * The parameter is in the format <board>:<revision>
 */
static int opt_to_hwrev(char *param, struct hw_type *hw)
{
	char *s;

	/* skip if there is no string */
	if (!param)
		return 0;

	s = strchr(param, ':');

	if (!s) {
		ERROR("You pass Hardware Revision in wrong format: %s\n",
				param);
		return -EINVAL;
	}

	strncpy(hw->revision, s + 1, sizeof(hw->revision));
	*s = '\0';
	strncpy(hw->boardname, param, sizeof(hw->boardname));

	if (!strlen(hw->boardname) || !strlen(hw->revision))
		return -EINVAL;

	return 0;
}

static int searching_for_image(char *name)
{
	char *dir, *dirc, *basec;
	char *fpattern;
	DIR *path;
	struct dirent *dp;
	int fd = -1;
	int found;
	char fname[MAX_IMAGE_FNAME];
	char *buf;
	char hex[4];

	dirc = strdup(name);
	basec = strdup(name);
	dir = dirname(dirc);
	fpattern = basename(basec);
	path = opendir(dir);

	TRACE("Searching image: check %s into %s\n",
			basec, dirc);
	if (!path) {
		free(dirc);
		free(basec);
		return -EBADF;
	}

	dp = readdir(path);
	do {
		if (!dp)
			break;
		if (!strcmp(dp->d_name, ".") ||
				!strcmp(dp->d_name, "..") ||
				!strlen(dp->d_name))
			continue;
		found = !fnmatch(fpattern, dp->d_name, FNM_CASEFOLD);

		if (found) {
			TRACE("File found: %s :\n", dp->d_name);
			/* Buffer for hexa output */
			buf = (char *)malloc(3 * strlen(dp->d_name) + 1);
			if (buf) {
				for (size_t i = 0; i < strlen(dp->d_name); i++) {
					snprintf(hex, sizeof(hex), "%x ", dp->d_name[i]);
					memcpy(&buf[3 * i], hex, 3);
				}
				buf[3 * strlen(dp->d_name)] = '\0';
				TRACE("File name (hex): %s\n", buf);
			}
			/* Take the first one as image */
			if (fd < 0) {
				if (snprintf(fname, sizeof(fname), "%s/%s",
					     dirc, dp->d_name) >= (int)sizeof(fname)) {
					ERROR("Path too long: %s/%s", dirc, dp->d_name);
				}
				fd = open(fname, O_RDONLY);
				if (fd > 0)
					TRACE("\t\t**Used for upgrade\n");
			}
			free(buf);
		}

	} while ((dp = readdir(path)) !=NULL);

	free(dirc);
	free(basec);

	return fd;
}

static int install_from_file(char *fname, int check)
{
	int fdsw;
	off_t pos;
	int ret;


	if (!strlen(fname)) {
		ERROR("Image not found...please reboot\n");
		exit(1);
	}

	fdsw = open(fname, O_RDONLY);
	if (fdsw < 0) {
		fdsw = searching_for_image(fname);
		if (fdsw < 0) {
			ERROR("Image Software cannot be read...exiting !\n");
			exit(1);
		}
	}

	pos = extract_sw_description(fdsw, SW_DESCRIPTION_FILENAME, 0);
#ifdef CONFIG_SIGNED_IMAGES
	pos = extract_sw_description(fdsw, SW_DESCRIPTION_FILENAME ".sig",
		pos);
#endif

	char* swdescfilename = alloca(strlen(get_tmpdir())+strlen(SW_DESCRIPTION_FILENAME)+1);
	sprintf(swdescfilename, "%s%s", get_tmpdir(), SW_DESCRIPTION_FILENAME);
	ret = parse(&swcfg, swdescfilename);
	if (ret) {
		ERROR("failed to parse " SW_DESCRIPTION_FILENAME "!\n");
		exit(1);
	}


	if (check_hw_compatibility(&swcfg)) {
		ERROR("SW not compatible with hardware\n");
		exit(1);
	}

	if (cpio_scan(fdsw, &swcfg, pos) < 0) {
		ERROR("failed to scan for pos '%ld'!", pos);
		close(fdsw);
		exit(1);
	}

	/*
	 * Check if all files described in sw-description
	 * are in the image
	 */
	ret = check_provided(&swcfg.images);
	if (ret) {
		ERROR("failed to check images!\n");
		exit(1);
	}
	ret = check_provided(&swcfg.scripts);
	if (ret) {
		ERROR("failed to check scripts!\n");
		exit(1);
	}

	if (check) {
		fprintf(stdout, "successfully checked '%s'\n", fname);
		exit(0);
	}

#ifdef CONFIG_MTD
		mtd_cleanup();
		scan_mtd_devices();
#endif
	/*
	 * Set "recovery_status" as begin of the transaction"
	 */
	bootloader_env_set("recovery_status", "in_progress");

	ret = install_images(&swcfg, fdsw, 1);

	swupdate_progress_end(ret == 0 ? SUCCESS : FAILURE);

	close(fdsw);

	if (ret) {
		fprintf(stdout, "Software updated failed\n");
		bootloader_env_set("recovery_status", "failed");
		exit(1);
	}

	bootloader_env_unset("recovery_status");
	fprintf(stdout, "Software updated successfully\n");
	fprintf(stdout, "Please reboot the device to start the new software\n");

	return 0;
}

static int parse_image_selector(const char *selector, struct swupdate_cfg *sw)
{
	char *pos;
	size_t len;

	pos = strchr(selector, ',');
	if (pos == NULL)
		return -EINVAL;

	len = pos - selector;
	if (len > sizeof(sw->software_set))
		len = sizeof(sw->software_set);

	strncpy(sw->software_set, selector, len);
	/* pos + 1 will either be NULL or valid text */
	strncpy(sw->running_mode, pos + 1, sizeof(sw->running_mode));

	if (strlen(sw->software_set) == 0 || strlen(sw->running_mode) == 0)
		return -EINVAL;

	return 0;
}

static void create_directory(const char* path) {
	char* dpath = alloca(strlen(get_tmpdir())+strlen(path)+1);
	sprintf(dpath, "%s%s", get_tmpdir(), path);
	mkdir(dpath, 0777);
}

static void swupdate_init(struct swupdate_cfg *sw)
{
	/* Initialize internal tree to store configuration */
	memset(sw, 0, sizeof(*sw));
	LIST_INIT(&sw->images);
	LIST_INIT(&sw->partitions);
	LIST_INIT(&sw->hardware);
	LIST_INIT(&sw->scripts);
	LIST_INIT(&sw->bootloader);
	LIST_INIT(&sw->extprocs);


	/* Create directories for scripts */
	create_directory(SCRIPTS_DIR_SUFFIX);
	create_directory(DATASRC_DIR_SUFFIX);
	create_directory(DATADST_DIR_SUFFIX);

#ifdef CONFIG_MTD
	mtd_init();
	ubi_init();
#endif
}

static int read_globals_settings(void *elem, void *data)
{
	struct swupdate_cfg *sw = (struct swupdate_cfg *)data;

	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"public-key-file", sw->globals.publickeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"aes-key-file", sw->globals.aeskeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"mtd-blacklist", sw->globals.mtdblacklist);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"postupdatecmd", sw->globals.postupdatecmd);
	get_field(LIBCFG_PARSER, elem, "verbose", &sw->globals.verbose);
	get_field(LIBCFG_PARSER, elem, "loglevel", &sw->globals.loglevel);
	get_field(LIBCFG_PARSER, elem, "syslog", &sw->globals.syslog_enabled);

	return 0;
}

static int read_processes_settings(void *settings, void *data)
{
	struct swupdate_cfg *sw = (struct swupdate_cfg *)data;
	void *elem;
	int count, i;
	struct extproc *proc, *last = NULL;

	count = get_array_length(LIBCFG_PARSER, settings);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(LIBCFG_PARSER, settings, i);

		if (!elem)
			continue;

		if(!(exist_field_string(LIBCFG_PARSER, elem, "name")))
			continue;
		if(!(exist_field_string(LIBCFG_PARSER, elem, "exec")))
			continue;

		proc = (struct extproc *)calloc(1, sizeof(struct extproc));

		GET_FIELD_STRING(LIBCFG_PARSER, elem, "name", proc->name);
		GET_FIELD_STRING(LIBCFG_PARSER, elem, "exec", proc->exec);

		if (!last)
			LIST_INSERT_HEAD(&sw->extprocs, proc, next);
		else
			LIST_INSERT_AFTER(last, proc, next);

		last = proc;

		TRACE("External process \"%s\": \"%s %s\" will be started",
		       proc->name, proc->exec, proc->options);
	}

	return 0;
}

static void sigterm_handler(int __attribute__ ((__unused__)) signum)
{
	pthread_cancel(network_daemon);
}

int main(int argc, char **argv)
{
	int c;
	char fname[MAX_IMAGE_FNAME];
	char *cfgfname = NULL;
	const char *software_select = NULL;
	int opt_i = 0;
	int opt_e = 0;
	int opt_c = 0;
	char image_url[MAX_URL];
	char main_options[256];
	unsigned int public_key_mandatory = 0;
	struct sigaction sa;
#ifdef CONFIG_SURICATTA
	int opt_u = 0;
	char suricattaoptions[1024];
	char **argvalues = NULL;
	int argcount = 0;
#endif
#ifdef CONFIG_WEBSERVER
	int opt_w = 0;
	char weboptions[1024];
	char **av = NULL;
	int ac = 0;
#endif
#ifdef CONFIG_DOWNLOAD
	int opt_d = 0;
	char dwloptions[1024];
#endif
	char **dwlav = NULL;
	int dwlac = 0;

#ifdef CONFIG_MTD
	memset(&flashdesc, 0, sizeof(flashdesc));
#endif
	memset(main_options, 0, sizeof(main_options));
	memset(image_url, 0, sizeof(image_url));
	strcpy(main_options, "vhi:e:l:Lcf:p:");
#ifdef CONFIG_MTD
	strcat(main_options, "b:");
#endif
#ifdef CONFIG_DOWNLOAD
	strcat(main_options, "d:");
#endif
#ifdef CONFIG_SURICATTA
	strcat(main_options, "u:");
#endif
#ifdef CONFIG_WEBSERVER
	strcat(main_options, "w:");
#endif
#ifdef CONFIG_HW_COMPATIBILITY
	strcat(main_options, "H:");
#endif
#ifdef CONFIG_SIGNED_IMAGES
	strcat(main_options, "k:");
	public_key_mandatory = 1;
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
	strcat(main_options, "K:");
#endif

	memset(fname, 0, sizeof(fname));

	printf("%s\n", BANNER);
	printf("Licensed under GPLv2. See source distribution for detailed "
		"copyright notices.\n\n");

	/* Initialize internal database */
	swupdate_init(&swcfg);

	/*
	 * Initialize notifier to enable at least output
	 * on the console
	 */
	notify_init();

	/*
	 * Check if there is a configuration file and parse it
	 * Parse once the command line just to find if a
	 * configuration file is passed
	 */
	while ((c = getopt_long(argc, argv, main_options,
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'f':
			cfgfname = sdup(optarg);
			if (read_module_settings(cfgfname, "globals",
				read_globals_settings, &swcfg)) {
				fprintf(stderr,
					 "Error parsing configuration file, exiting..\n");
				exit(1);
			}

			loglevel = swcfg.globals.loglevel;
			if (swcfg.globals.verbose)
				loglevel = TRACELEVEL;

			int ret = read_module_settings(cfgfname, "processes",
							read_processes_settings,
							&swcfg);
			/*
			 * ignore other errors, check only if file is parsed
			 */
			if (ret == -EINVAL) {
				fprintf(stderr,
					 "Error parsing configuration file, exiting..\n");
				exit(1);
			}
			break;
		}
	}

	/*
	 * Command line should be parsed a second time
	 * This let line parameters overload
	 * setup in the configuration file
	 * According to getopt(), this can be done by setting to 0
	 * the external global optind variable
	 */
	optind = 0;

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, main_options,
				long_options, NULL)) != EOF) {
		if (optarg && *optarg == '-' && (c != 'd' && c != 'u' && c != 'w')) {
			/* An option's value starting with '-' is not allowed except
			 * for downloader, webserver, and suricatta doing their own
			 * argv parsing.
			 */
			c = '?';
		}
		switch (c) {
		case 'v':
			loglevel = TRACELEVEL;
			break;
#ifdef CONFIG_MTD
		case 'b':
			mtd_set_ubiblacklist(optarg);
			break;
#endif
		case 'i':
			strncpy(fname, optarg, sizeof(fname));
			opt_i = 1;
			break;
		case 'l':
			loglevel = strtoul(optarg, NULL, 10);
			break;
		case 'L':
			swcfg.globals.syslog_enabled = 1;
			break;
		case 'k':
			strncpy(swcfg.globals.publickeyfname,
				optarg,
			       	sizeof(swcfg.globals.publickeyfname));
			break;
#ifdef CONFIG_ENCRYPTED_IMAGES
		case 'K':
			strncpy(swcfg.globals.aeskeyfname,
				optarg,
			       	sizeof(swcfg.globals.aeskeyfname));
			break;
#endif
		case 'e':
			software_select = optarg;
			opt_e = 1;
			break;
		/* Configuration file already parsed, ignores it */
		case 'f':
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
#ifdef CONFIG_DOWNLOAD
		case 'd':
			(void)snprintf(dwloptions, sizeof(dwloptions), "%s %s", argv[0], optarg);
			dwlav = splitargs(dwloptions, &dwlac);
			opt_d = 1;
			break;
#endif
		case 'H':
			if (opt_to_hwrev(optarg, &swcfg.hw) < 0)
				exit(1);
			break;
#ifdef CONFIG_SURICATTA
		case 'u':
			(void)snprintf(suricattaoptions, sizeof(suricattaoptions), "%s %s", argv[0], optarg);
			argvalues = splitargs(suricattaoptions, &argcount);
			opt_u = 1;
			break;
#endif
#ifdef CONFIG_WEBSERVER
		case 'w':
			snprintf(weboptions, sizeof(weboptions), "%s %s", argv[0], optarg);
			av = splitargs(weboptions, &ac);
			opt_w = 1;
			break;
#endif
		case 'c':
			opt_c = 1;
			break;
		case 'p':
			strncpy(swcfg.globals.postupdatecmd, optarg,
				sizeof(swcfg.globals.postupdatecmd));
			break;
		default:
			usage(argv[0]);
			exit(1);
			break;
		}
	}

	if (optind < argc) {
		/* SWUpdate has no non-option arguments, fail on them */
		usage(argv[0]);
		exit(1);
	}

	/*
	 * Parameters are parsed: now performs plausibility
	 * tests before starting processes and threads
	 */
	if (public_key_mandatory && !strlen(swcfg.globals.publickeyfname)) {
		fprintf(stderr,
			 "swupdate built for signed image, provide a public key file\n");
		usage(argv[0]);
		exit(1);
	}

	if (opt_c && !opt_i) {
		fprintf(stderr,
			"request check for local image, it requires -i\n");
		usage(argv[0]);
		exit(1);
	}

#ifdef CONFIG_SURICATTA
	if (opt_u && (opt_c || opt_i)) {
		fprintf(stderr, "invalid mode combination with suricatta.\n");
		exit(1);
	}
#endif

	swupdate_crypto_init();

	if (strlen(swcfg.globals.publickeyfname)) {
		if (swupdate_dgst_init(&swcfg, swcfg.globals.publickeyfname)) {
			fprintf(stderr,
				 "Crypto cannot be initialized\n");
			exit(1);
		}
	}

	/*
	 * Install a child handler to check if a subprocess
	 * dies
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sigaction(SIGCHLD, &sa, NULL);

	/*
	 * If just a check is required, do not
	 * start background processes and threads
	 */
	if (!opt_c) {
		/* Start embedded web server */
#if defined(CONFIG_MONGOOSE)
		if (opt_w) {
			start_subprocess(SOURCE_WEBSERVER, "webserver",
						cfgfname, ac, av,
						start_mongoose);
			freeargs(av);
		}
#endif

#if defined(CONFIG_SURICATTA)
		if (opt_u) {
			start_subprocess(SOURCE_SURICATTA, "suricatta",
					 cfgfname, argcount,
				       	 argvalues, start_suricatta);

			freeargs(argvalues);
		}
#endif

#ifdef CONFIG_DOWNLOAD
		if (opt_d) {
			start_subprocess(SOURCE_DOWNLOADER, "download",
				       	 cfgfname, dwlac,
				       	 dwlav, start_download);
			freeargs(dwlav);
		}
#endif

		/*
		 * Start all processes added in the config file
		 */
		struct extproc *proc;

		LIST_FOREACH(proc, &swcfg.extprocs, next) {
			dwlav = splitargs(proc->exec, &dwlac);

			dwlav[dwlac] = NULL;

			start_subprocess_from_file(SOURCE_UNKNOWN, proc->name,
						   cfgfname, dwlac,
						   dwlav, dwlav[0]);

			freeargs(dwlav);
		}
	}

#ifdef CONFIG_MTD
	if (strlen(swcfg.globals.mtdblacklist))
		mtd_set_ubiblacklist(swcfg.globals.mtdblacklist);
#endif

	/*
	 * If a aes key is passed, load it to allow
	 * to decrypt images
	 */
	if (strlen(swcfg.globals.aeskeyfname)) {
		if (load_decryption_key(swcfg.globals.aeskeyfname)) {
			fprintf(stderr,
				"Key file does not contain a valid AES key\n");
			exit(1);
		}
	}

	if (lua_handlers_init())
		printf("\nCustom handlers not found, no error, skipping...\n\n");

	if(!get_hw_revision(&swcfg.hw))
		printf("Running on %s Revision %s\n", swcfg.hw.boardname, swcfg.hw.revision);

	print_registered_handlers();
	if (swcfg.globals.syslog_enabled) {
		if (syslog_init()) {
			ERROR("failed to initialize syslog notifier");
		}
	}

	if (opt_e) {
		if (parse_image_selector(software_select, &swcfg)) {
			fprintf(stderr, "Incorrect select option format\n");
			exit(1);
		}
		fprintf(stderr, "software set: %s mode: %s\n",
			swcfg.software_set, swcfg.running_mode);
	}

	/* Read sw-versions */
	get_sw_versions(cfgfname, &swcfg);

	/*
	 *  Do not start daemon if just a check is required
	 *  SWUpdate will exit after the check
	 */
	if (!opt_c) {
		network_daemon = start_thread(network_initializer, &swcfg);

		start_thread(progress_bar_thread, NULL);
	}

	if (opt_i) {

		install_from_file(fname, opt_c);
		cleanup_files(&swcfg);

		notify(SUCCESS, 0, 0);
		if (postupdate(&swcfg, NULL) != 0) {
			ERROR("Post-update command execution failed.");
		}
	}

	/*
	 * Install a handler for SIGTERM that cancels
	 * the network_daemon thread to allow atexit()
	 * registered functions to run.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sigaction(SIGTERM, &sa, NULL);

	/*
	 * Go into supervisor loop
	 */
	if (!opt_c && !opt_i)
		pthread_join(network_daemon, NULL);

	return EXIT_SUCCESS;
}
