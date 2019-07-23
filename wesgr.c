/*
 * Copyright © 2014 Pekka Paalanen <pq@iki.fi>
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>

#include <json.h>

#include "wesgr.h"

struct bytebuf {
	uint8_t *data;
	size_t alloc;
	size_t len;
	size_t pos;
};

static void
bytebuf_init(struct bytebuf *bb)
{
	bb->data = NULL;
	bb->alloc = 0;
	bb->len = 0;
	bb->pos = 0;
}

static void
bytebuf_release(struct bytebuf *bb)
{
	free(bb->data);
	bytebuf_init(bb);
}

static int
bytebuf_ensure(struct bytebuf *bb, size_t sz)
{
	uint8_t *data;

	if (bb->alloc >= sz)
		return 0;

	data = realloc(bb->data, sz);
	if (!data)
		return ERROR;

	bb->data = data;
	bb->alloc = sz;

	return 0;
}

static int
bytebuf_read_from_file(struct bytebuf *bb, FILE *fp, size_t sz)
{
	size_t ret;

	if (bytebuf_ensure(bb, sz) < 0)
		return ERROR;

	ret = fread(bb->data, 1, sz, fp);
	if (ferror(fp))
		return ERROR;

	bb->len = ret;
	bb->pos = 0;

	return 0;
}

static int
parse_file(const char *name, struct parse_context *ctx)
{
	int ret = -1;
	struct bytebuf bb;
	FILE *fp;
	struct json_tokener *jtok;
	struct json_object *jobj;

	bytebuf_init(&bb);
	jtok = json_tokener_new();
	if (!jtok)
		return ERROR;

	fp = fopen(name, "r");
	if (!fp)
		goto out_release;

	while (1) {
		enum json_tokener_error jerr;
		int r;

		jobj = json_tokener_parse_ex(jtok,
					     (char *)(bb.data + bb.pos),
					     bb.len - bb.pos);
		jerr = json_tokener_get_error(jtok);
		if (!jobj && jerr == json_tokener_continue) {
			if (feof(fp)) {
				ret = 0;
				break;
			}

			if (bytebuf_read_from_file(&bb, fp, 8192) < 0)
				break;

			continue;
		}

		if (!jobj) {
			fprintf(stderr, "JSON parse failure: %d\n", jerr);
			break;
		}

		bb.pos += jtok->char_offset;

		r = parse_context_process_object(ctx, jobj);
		json_object_put(jobj);

		if (r < 0) {
			fprintf(stderr, "JSON interpretation error\n");
			break;
		}
	}

	fclose(fp);

	if (ret != -1)
		ret = graph_data_end(ctx->gdata);

out_release:
	bytebuf_release(&bb);
	json_tokener_free(jtok);

	if (ret == -1)
		return ERROR;
	return ret;
}

struct prog_args {
	int from_ms;
	int to_ms;
	const char *infile;
	const char *svgfile;
};

static void
print_usage(const char *prog)
{
	printf("Usage:\n  %s -i input.log -o output.svg [options]\n"
	"Arguments and options:\n"
	"  -h, --help                Print this help and exit.\n"
	"  -i, --input=FILE          Read FILE as the input data.\n"
	"  -o, --output=FILE         Write FILE as the output SVG.\n"
	"  -a, --from-ms=MS          Start the graph at MS milliseconds.\n"
	"  -b, --to-ms=MS            End the graph at MS milliseconds.\n",
	prog);
}

static int
parse_opts(struct prog_args *args, int argc, char *argv[])
{
	static const char short_opts[] = "hi:a:b:o:";
	static const struct option opts[] = {
		{ "help",              no_argument,       0, 'h' },
		{ "input",             required_argument, 0, 'i' },
		{ "from-ms",           required_argument, 0, 'a' },
		{ "to-ms",             required_argument, 0, 'b' },
		{ "output",            required_argument, 0, 'o' },
		{ NULL, 0, 0, 0 }
	};

	while (1) {
		int c;
		int longindex;

		c = getopt_long(argc, argv, short_opts, opts, &longindex);
		if (c == -1)
			break;

		switch (c) {
		case '?':
			return -1;
		case 'h':
			print_usage(argv[0]);
			return -1;
		case 'i':
			args->infile = optarg;
			break;
		case 'a':
			args->from_ms = atoi(optarg);
			break;
		case 'b':
			args->to_ms = atoi(optarg);
			break;
		case 'o':
			args->svgfile = optarg;
			break;
		default:
			break;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Error, extra command line arguments:");
		while (optind < argc)
			fprintf(stderr, " %s", argv[optind++]);
		fprintf(stderr, "\n");

		return -1;
	}

	return 0;
}

int
main(int argc, char *argv[])
{
	struct prog_args args = { -1, -1, NULL, NULL };
	struct graph_data gdata;
	struct parse_context ctx;

	if (parse_opts(&args, argc, argv) < 0)
		return 1;

	if (!args.infile) {
		fprintf(stderr, "Error: input file not specified.\n");
		return 1;
	}

	if (!args.svgfile) {
		fprintf(stderr, "Error: output file not specified.\n");
		return 1;
	}

	if (graph_data_init(&gdata) < 0)
		return 1;

	if (parse_context_init(&ctx, &gdata) < 0)
		return 1;

	if (parse_file(args.infile, &ctx) < 0)
		return 1;

	if (graph_data_to_svg(&gdata, args.from_ms, args.to_ms,
			      args.svgfile) < 0)
		return 1;

	parse_context_release(&ctx);
	graph_data_release(&gdata);

	return 0;
}

void
generic_error(const char *file, int line, const char *func)
{
	fprintf(stderr, "Error in %s(), %s:%d\n", func, file, line);
}

