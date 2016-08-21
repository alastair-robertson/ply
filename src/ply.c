/*
 * Copyright 2015-2016 Tobias Waldekranz <tobias@waldekranz.com>
 *
 * This file is part of ply.
 *
 * ply is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, under the terms of version 2 of the
 * License.
 *
 * ply is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ply.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "ply.h"
#include "map.h"
#include "lang/ast.h"
#include "pvdr/pvdr.h"

FILE *scriptfp;

struct globals G;

static const char *sopts = "AcdDht:";
static struct option lopts[] = {
	{ "ascii",   no_argument,       0, 'A' },
	{ "command", no_argument,       0, 'c' },
	{ "debug",   no_argument,       0, 'd' },
	{ "dump",    no_argument,       0, 'D' },
	{ "help",    no_argument,       0, 'h' },
	{ "timeout", required_argument, 0, 't' },

	{ NULL }
};

void usage()
{
	printf("USAGE: ply [options] { scriptfile | -c 'program text' }\n\n");
	printf("       -A		# limit output to ASCII, no Unicode\n");
	printf("       -c 'program'	# execute specified program\n");
	printf("       -d		# include compilation debug info\n");
	printf("       -D		# dump BPF, and do not run\n");
	printf("       -h		# usage message (this)\n");
	printf("       -t timeout	# run duration (seconds)\n");
}

int parse_opts(int argc, char **argv, FILE **sfp)
{
	int cmd = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, sopts, lopts, NULL)) > 0) {
		switch (opt) {
		case 'A':
			G.ascii++;
			break;
		case 'c':
			cmd = 1;
			break;
		case 'd':
			G.debug++;
			break;
		case 'D':
			G.dump++;
			break;
		case 'h':
			usage();
			exit(0);
		case 't':
			G.timeout = strtol(optarg, NULL, 0);
			if (G.timeout <= 0) {
				_e("timeout must be a positive integer");
				return -EINVAL;
			}
			break;
		default:
			_e("unknown option '%c'. Try -h for usage.", opt);
			return -EINVAL;
		}
	}

	if (optind >= argc)
		return -EINVAL;

	if (cmd)
		*sfp = fmemopen(argv[optind], strlen(argv[optind]), "r");
	else
		*sfp = fopen(argv[optind], "r");

	if (!*sfp) {
		_pe("unable to read script");
		return -EIO;
	}

	return 0;
}

void sigint(int sigint)
{
	return;
}

int main(int argc, char **argv)
{
	FILE *sfp, *enable;
	node_t *probe, *script = NULL;
	prog_t *prog = NULL;
	pvdr_t *pvdr;
	int err = 0, num;

	scriptfp = stdin;
	err = parse_opts(argc, argv, &sfp);
	if (err)
		goto err;

	script = node_script_parse(sfp);
	if (!script) {
		err = -EINVAL;
		goto err;
	}

	err = pvdr_resolve(script);
	if (err)
		goto err;

	err = annotate_script(script);
	if (err)
		goto err;

	err = map_setup(script);
	if (err)
		goto err;
		
	if (G.dump)
		node_ast_dump(script);

	node_foreach(probe, script->script.probes) {
		err = -EINVAL;
		prog = compile_probe(probe);
		if (!prog)
			break;

		if (G.dump)
			continue;

		pvdr = node_get_pvdr(probe);
		num = pvdr->setup(probe, prog);
		if (num < 0) {
			err = num;
			goto err;
		}
	}

	_d("compilation ok");
	if (G.dump)
		goto done;
	
	if (G.timeout) {
		siginterrupt(SIGALRM, 1);
		signal(SIGALRM, sigint);
		alarm(G.timeout);
	}

	siginterrupt(SIGINT, 1);
	signal(SIGINT, sigint);
	
	enable = fopen("/sys/kernel/debug/tracing/events/kprobes/enable", "w");
	if (!enable) {
		perror("unable to enable probes");
		err = -errno;
		goto err;
	}

	fputs("1\n", enable);
	fflush(enable);
	rewind(enable);

	fprintf(stderr, "%d probe%s active\n", num, (num == 1) ? "" : "s");
	printf_drain(script);

	fprintf(stderr, "de-activating probes\n");
	fputs("0\n", enable);
	fflush(enable);
	fclose(enable);

	node_foreach(probe, script->script.probes) {
		pvdr = node_get_pvdr(probe);
		err = pvdr->teardown(probe);
		if (err)
			break;
	}

	fclose(fopen("/sys/kernel/debug/tracing/kprobe_events", "w"));

	map_teardown(script);
done:
err:
	if (prog)
		free(prog);
	if (script)
		node_free(script);

	return err;
}
