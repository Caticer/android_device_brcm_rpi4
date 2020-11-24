// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libgpiod.
 *
 * Copyright (C) 2017-2018 Bartosz Golaszewski <bartekgola@gmail.com>
 */

#include <gpiod.h>
#include "tools-common.h"

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>

#include <utils/Log.h>

#include <string>
#include <vector>
#include <sstream>

static const struct option longopts[] = {
	{ "help",		no_argument,		NULL,	'h' },
	{ "version",		no_argument,		NULL,	'v' },
	{ "active-low",		no_argument,		NULL,	'l' },
	{ "mode",		required_argument,	NULL,	'm' },
	{ "sec",		required_argument,	NULL,	's' },
	{ "usec",		required_argument,	NULL,	'u' },
	{ "background",		no_argument,		NULL,	'b' },
    { "debug",   no_argument,        NULL,   'd' },
	{ GETOPT_NULL_LONGOPT },
};

static const char *const shortopts = "+hvlm:s:u:b:d";

static void print_help(void)
{
	printf("Usage: gpioset [OPTIONS] <chip name/number> <offset1>=<value1> <offset2>=<value2> ...\n");
	printf("Set GPIO line values of a GPIO chip\n");
	printf("\n");
	printf("Options:\n");
	printf("  -h, --help:\t\tdisplay this message and exit\n");
	printf("  -v, --version:\tdisplay the version and exit\n");
	printf("  -l, --active-low:\tset the line active state to low\n");
	printf("  -m, --mode=[exit|wait|time|signal] (defaults to 'exit'):\n");
	printf("		tell the program what to do after setting values\n");
	printf("  -s, --sec=SEC:\tspecify the number of seconds to wait (only valid for --mode=time)\n");
	printf("  -u, --usec=USEC:\tspecify the number of microseconds to wait (only valid for --mode=time)\n");
	printf("  -b, --background:\tafter setting values: detach from the controlling terminal\n");
    printf("  -d, --debug:\tlog all the things in logcat\n");
	printf("\n");
	printf("Modes:\n");
	printf("  exit:\t\tset values and exit immediately\n");
	printf("  wait:\t\tset values and wait for user to press ENTER\n");
	printf("  time:\t\tset values and sleep for a specified amount of time\n");
	printf("  signal:\tset values and wait for SIGINT or SIGTERM\n");
}

struct callback_data {
	/* Replace with a union once we have more modes using callback data. */
	struct timeval tv;
	bool daemonize;
};

static bool debug;

static void maybe_daemonize(bool daemonize)
{
	int status;

	if (daemonize) {
		status = daemon(0, 0);
		if (status < 0)
			die("unable to daemonize: %s", strerror(errno));
	}
}

static void wait_enter(void *data GPIOD_UNUSED)
{
	getchar();
}

static void wait_time(void *data)
{
	struct callback_data *cbdata = (callback_data *)data;

	maybe_daemonize(cbdata->daemonize);
	select(0, NULL, NULL, NULL, &cbdata->tv);
}

static void wait_signal(void *data)
{
	struct callback_data *cbdata = (callback_data *)data;
	int sigfd, status;
	struct pollfd pfd;
	sigset_t sigmask;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGINT);

	status = sigprocmask(SIG_BLOCK, &sigmask, NULL);
	if (status < 0)
		die("error blocking signals: %s", strerror(errno));

	sigfd = signalfd(-1, &sigmask, 0);
	if (sigfd < 0)
		die("error creating signalfd: %s", strerror(errno));

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = sigfd;
	pfd.events = POLLIN | POLLPRI;

	maybe_daemonize(cbdata->daemonize);

	for (;;) {
		status = poll(&pfd, 1, 1000 /* one second */);
		if (status < 0)
			die("error polling for signals: %s", strerror(errno));
		else if (status > 0)
			break;
	}

	/*
	 * Don't bother reading siginfo - it's enough to know that we
	 * received any signal.
	 */
	close(sigfd);
}

enum {
	MODE_EXIT = 0,
	MODE_WAIT,
	MODE_TIME,
	MODE_SIGNAL,
};

struct mode_mapping {
	int id;
	const char *name;
	gpiod_ctxless_set_value_cb callback;
};

static const struct mode_mapping modes[] = {
	[MODE_EXIT] = {
		.id		= MODE_EXIT,
		.name		= "exit",
		.callback	= NULL,
	},
	[MODE_WAIT] = {
		.id		= MODE_WAIT,
		.name		= "wait",
		.callback	= wait_enter,
	},
	[MODE_TIME] = {
		.id		= MODE_TIME,
		.name		= "time",
		.callback	= wait_time,
	},
	[MODE_SIGNAL] = {
		.id		= MODE_SIGNAL,
		.name		= "signal",
		.callback	= wait_signal,
	},
};

static const struct mode_mapping *parse_mode(const char *mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++)
		if (strcmp(mode, modes[i].name) == 0)
			return &modes[i];

	return NULL;
}

int main(int argc, char **argv)
{
	const struct mode_mapping *mode = &modes[MODE_EXIT];
	unsigned int *offsets, num_lines, i;
	int *values, status, optc, opti;
	struct callback_data cbdata;
	bool active_low = false;
	char *device, *end;
    std::vector<char *> argvList {};
    std::vector<std::string> argvStrings {};
    
    int argcList = 0;
    int argvIndex = 0;

	memset(&cbdata, 0, sizeof(cbdata));

    // split by space if needed when args are passed in quotes
    for (i = 0; i < argc; i++) {
        std::string buf;
        std::stringstream ss(argv[i]);

        while (ss >> buf) {
            argvStrings.push_back(buf);
        }
    }

    for (i = 0; i < argvStrings.size(); i++) {
        argvList.push_back(const_cast<char *>(argvStrings[i].c_str()));
    }
    argcList = argvList.size();

    // signal is default for us
    mode = &modes[3];

	for (;;) {
        optc = getopt_long(argcList, &argvList[0], shortopts, longopts, &opti);
		if (optc < 0)
			break;

		switch (optc) {
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'v':
			print_version();
			return EXIT_SUCCESS;
		case 'l':
			active_low = true;
			break;
		case 'm':
			mode = parse_mode(optarg);
			if (!mode)
				die("invalid mode: %s", optarg);
			break;
		case 's':
			cbdata.tv.tv_sec = strtoul(optarg, &end, 10);
			if (*end != '\0')
				die("invalid time value in seconds: %s", optarg);
			break;
		case 'u':
			cbdata.tv.tv_usec = strtoul(optarg, &end, 10);
			if (*end != '\0')
				die("invalid time value in microseconds: %s",
				    optarg);
			break;
		case 'b':
			cbdata.daemonize = true;
			break;
        case 'd':
            debug = true;
            break;
        case '?':
			die("try gpioset --help");
		default:
			abort();
		}
	}

    if (debug) {
        for (i = 0; i < argcList; i++) {
            ALOGI("argv[%d] = %s", i, argvList[i]);
        }
    }

    argvIndex += optind;

	if (mode->id != MODE_TIME && (cbdata.tv.tv_sec || cbdata.tv.tv_usec))
		die_logd("can't specify wait time in this mode");

	if (mode->id != MODE_SIGNAL &&
	    mode->id != MODE_TIME &&
	    cbdata.daemonize)
		die_logd("can't daemonize in this mode");

	if (argcList - argvIndex < 1)
		die_logd("gpiochip must be specified");

	if (argcList - argvIndex < 2)
		die_logd("at least one GPIO line offset to value mapping must be specified");

	device = argvList[argvIndex];

	num_lines = argcList - argvIndex - 1;

    if (debug) ALOGI("device = %s num_lines = %d", device, num_lines);

	offsets = (unsigned int *)malloc(sizeof(*offsets) * num_lines);
	values = (int *)malloc(sizeof(*values) * num_lines);
	if (!values || !offsets)
		die_logd("out of memory");

	for (i = 0; i < num_lines; i++) {
        int offsetsIndex = argvIndex + 1 + i;
		status = sscanf(argvList[offsetsIndex], "%u=%d", &offsets[i], &values[i]);
		if (status != 2)
			die_logd("invalid offset<->value mapping: %s", argvList[offsetsIndex]);

		if (values[i] != 0 && values[i] != 1)
			die_logd("value must be 0 or 1: %s", argvList[offsetsIndex]);

		if (offsets[i] > INT_MAX)
			die_logd("invalid offset: %s", argvList[offsetsIndex]);
	}

    if (debug) {
        for (i = 0; i < num_lines; i++) {
            ALOGI("offset %d  = value %d", offsets[i], values[i]);
        }
    }
    
	status = gpiod_ctxless_set_value_multiple(device, offsets, values,
						  num_lines, active_low,
						  "gpioset", mode->callback,
						  &cbdata);
	if (status < 0)
		die_perror("error setting the GPIO line values");

	free(offsets);
	free(values);

	return EXIT_SUCCESS;
}
