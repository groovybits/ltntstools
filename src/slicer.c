/* For a given TS file, drop N packets on PID X start at PID X packet Y. */
/* A tool that should take a clean video file, drop PTS packets for a specific pid a reproduce the TR101290 2.5 error on demand. */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include <libltntstools/ltntstools.h>

struct videotime_s
{
	int days;
	int hours;
	int mins;
	int secs;
	int msecs;
};

void pcr_to_videotime(int64_t pcr, struct videotime_s *vt)
{
	int seconds = pcr / 27000000;

	vt->days = seconds / (3600 * 24);
	seconds -= (vt->days * 3600 * 24);

	vt->hours = seconds / 3600;
	seconds -= (vt->hours * 3600);

	vt->mins = seconds / 60;
	seconds -= (vt->mins * 60);
	
	vt->secs = seconds;

	vt->msecs = (pcr - (pcr / 27000000)) % 27000;
	vt->msecs = 0;
}

int64_t videotime_to_pcr(struct videotime_s *vt)
{
	int64_t pcr = 0;

	pcr += vt->days * 3600 * 24;
	pcr += vt->hours * 3600;
	pcr += vt->mins * 60;
	pcr += vt->secs;
	pcr *= 27000000;
	pcr += vt->msecs * 27000;

	return pcr;
}

int str_to_videotime(char *t, struct videotime_s *vt)
{
	int ret = sscanf(t, "%d.%02d:%02d:%02d.%d",
		&vt->days,
		&vt->hours,
		&vt->mins,
		&vt->secs,
		&vt->msecs);
	if (ret != 5)
		return -1; /* Error */

	return 0;
}

char *videotime_to_str(struct videotime_s *vt)
{
	char *t = malloc(32);
	sprintf(t, "%d.%02d:%02d:%02d.%d",
		vt->days,
		vt->hours,
		vt->mins,
		vt->secs,
		vt->msecs);
	return t;
}

struct tool_context_s
{
	char *ifn;
	char *ofn;
	FILE *ifh;
	struct ltntstools_pcr_position_s *allPCRs;
	int allPCRLength;
	char *opt_e;
	char *opt_s;

	int64_t pcrMin, pcrMax, pcrDuration;

	struct videotime_s streamTime;

	struct videotime_s timeStartStream;
	struct videotime_s timeEndStream;
};

static int indexSave(struct tool_context_s *ctx)
{
	char ofn[256];
	sprintf(ofn, "%s.idx", ctx->ifn);

	printf("Writing index %s\n", ofn);
	FILE *ofh = fopen(ofn, "wb");
	fwrite(ctx->allPCRs, sizeof(struct ltntstools_pcr_position_s), ctx->allPCRLength, ofh);
	fclose(ofh);

	return 0;
}

static int indexLoad(struct tool_context_s *ctx)
{
	char ofn[256];
	sprintf(ofn, "%s.idx", ctx->ifn);

	printf("\nReading index %s\n", ofn);
	FILE *fh = fopen(ofn, "rb");
	if (!fh) {
		fprintf(stderr, "index not found\n");
		return -1;
	}

	fseeko(fh, 0, SEEK_END);
	off_t fileLength = ftell(fh);
	fseeko(fh, 0, SEEK_SET);

	ctx->allPCRs = malloc(fileLength);

	fread(ctx->allPCRs, 1, fileLength, fh);
	ctx->allPCRLength = fileLength / sizeof(struct ltntstools_pcr_position_s);
	fclose(fh);

	ctx->pcrMin = ctx->allPCRs->pcr;
	ctx->pcrMax = (ctx->allPCRs+ ctx->allPCRLength - 1)->pcr;
	ctx->pcrDuration = ctx->pcrMax - ctx->pcrMin;

	pcr_to_videotime(ctx->pcrDuration, &ctx->streamTime);

	char *streamtime = videotime_to_str(&ctx->streamTime);

	printf("PCRs from: %" PRIi64 " to %" PRIi64 ", duration %" PRIi64 ", %s\n",
		ctx->pcrMin,
		ctx->pcrMax,
		ctx->pcrDuration,
		streamtime
		);

	free(streamtime);

	return 0;
}

static void indexDumpEntry(struct tool_context_s *ctx, int id, struct ltntstools_pcr_position_s *p)
{
	struct videotime_s vt;
	pcr_to_videotime(p->pcr, &vt);

	char *streamtime = videotime_to_str(&vt);
	printf("%8d: 0x%04x %016" PRIx64 " %16" PRIi64 ", %s\n",
		id,
		p->pid,
		p->offset,
		p->pcr,
		streamtime);

	free(streamtime);
}

static void indexDump(struct tool_context_s *ctx)
{
	for (int i = 0; i < ctx->allPCRLength; i++) {
		indexDumpEntry(ctx, i, ctx->allPCRs + i);
	}
}

struct ltntstools_pcr_position_s *indexLookupPCR(struct tool_context_s *ctx, int64_t pcr)
{
	for (int i = 0; i < ctx->allPCRLength; i++) {
		if (pcr <= (ctx->allPCRs + i)->pcr)
			return ctx->allPCRs + i;
	}
	return NULL;
}

int indexFastQueryDuration(const char *fname,
	struct ltntstools_pcr_position_s *begin,
	struct ltntstools_pcr_position_s *end,
	int64_t *durationTicks, /* 27 MHz */
	struct videotime_s *streamTime,
	int64_t *fileSize)
{
	if (!fname || !begin || !end)
		return -1;

	/* If the file is less than 32MB, load the entire thing and index it */
	/* If the file is more than 32MB, load the first 16MB and the last 16MB */
	int singleRead = 0;
	struct {
		unsigned char *buf;
		int lengthBytes;
		struct ltntstools_pcr_position_s *arr;
		int arrlen;
		int addr;
	} segs[2];
	memset(&segs[0], 0, sizeof(segs));

	FILE *fh = fopen(fname, "rb");
	if (!fh) {
		return -1;
	}

	fseeko(fh, 0, SEEK_END);
	off_t lengthBytes = ftello(fh);
	fseek(fh, 0, SEEK_SET);

	if (lengthBytes < 32 * 1048576) {
		singleRead = 1;
		segs[0].lengthBytes = lengthBytes;
		segs[0].buf = malloc(segs[0].lengthBytes);
		segs[0].addr = 0;
		fread(segs[0].buf, 1, segs[0].lengthBytes, fh);
	} else {
		singleRead = 0;
		segs[0].lengthBytes = 16 * 1048576;
		segs[0].buf = malloc(segs[0].lengthBytes);
		segs[0].addr = 0;
		fread(segs[0].buf, 1, segs[0].lengthBytes, fh);

		segs[1].lengthBytes = 16 * 1048576;
		segs[1].buf = malloc(segs[1].lengthBytes);
		segs[1].addr = lengthBytes - (16 * 1048576);
		fseeko(fh, -(16 * 1048576), SEEK_END);
		fread(segs[1].buf, 1, segs[1].lengthBytes, fh);
	}
	fclose(fh);

	for (int i = 0; i < 2; i++) {
		if (segs[i].buf) {
			int ret = ltntstools_queryPCRs(segs[i].buf, segs[i].lengthBytes,
				segs[i].addr, &segs[i].arr, &segs[i].arrlen);
			if (ret == 0) {
				// indexDumpEntry(NULL, 0, segs[i].arr);
			}
		}
	}

	memcpy(begin, segs[0].arr, sizeof(struct ltntstools_pcr_position_s));

	if (singleRead) {
		memcpy(end, segs[0].arr + (segs[0].arrlen - 1), sizeof(struct ltntstools_pcr_position_s));
	} else {
		memcpy(end, segs[1].arr + (segs[1].arrlen - 1), sizeof(struct ltntstools_pcr_position_s));
	}
#if 0
	indexDumpEntry(NULL, 0, begin);
	indexDumpEntry(NULL, 0, end);
#endif
	for (int i = 0; i < 2; i++) {
		if (segs[i].buf)
			free(segs[i].buf);
	}

	if (durationTicks)
		*durationTicks = end->pcr - begin->pcr;
	if (fileSize)
		*fileSize = lengthBytes;

	if (streamTime)
		pcr_to_videotime(end->pcr - begin->pcr, streamTime);

	return 0; /* Success */
}

static void usage(const char *progname)
{
	printf("\nA tool to extract time periods from ISO13818 MPEGTS SPTS or MPTS files.\n");
	printf("Input file is assumed to be properly packet aligned.\n");
	printf("\nUsage:\n");
	printf("  -i <input.ts>\n");
	printf("  -o <output.ts>\n");
	printf("\nExamples:\n");
	printf("  # Create a timing index of your recording.ts file, 2hr recording can take 2-3 mins.\n");
	printf("  # This will create recording.ts.idx.\n");
	printf("  %s -i recording.ts\n", progname);
	printf("  # Show the contents of the timing index (automatically opens recording.ts.idx)\n");
	printf("  %s -i recording.ts -l\n", progname);
	printf("  # Extract the segment between two different timestamps, roughly 30 seconds long, to new file output.ts.\n");
	printf("  %s -i recording.ts -s 0.hh:mm:ss.0 -e 0.hh:mm:ss.0\n", progname);
	printf("  %s -i recording.ts -s 0.05:17:44.0 -e 0.05:18.14.0 -o output.ts\n", progname);
}

int slicer(int argc, char *argv[])
{
	int ch;

	struct tool_context_s tctx, *ctx;
	ctx = &tctx;
	memset(ctx, 0, sizeof(*ctx));

        while ((ch = getopt(argc, argv, "?hi:ls:e:o:q:")) != -1) {
		switch (ch) {
		case '?':
		case 'h':
			usage(argv[0]);
			exit(1);
		case 'e':
			if (str_to_videotime(optarg, &ctx->timeEndStream) < 0) {
				fprintf(stderr, "-e syntax error\n");
				exit(1);
			}
			ctx->opt_e = strdup(optarg);
			break;
		case 'i':
			ctx->ifn = strdup(optarg);
			break;
		case 'l':
			{
			int ret = indexLoad(ctx);
			if (ret < 0) {
			}
			indexDump(ctx);
			break;
			}
		case 'o':
			ctx->ofn = strdup(optarg);
			break;
		case 'q':
			{
			struct ltntstools_pcr_position_s begin, end;
			struct videotime_s streamTime;
			int64_t durationTicks;
			int64_t fileLengthBytes;

			int ret = indexFastQueryDuration(optarg, &begin, &end, &durationTicks,
					&streamTime, &fileLengthBytes);
			if (ret < 0) {
				fprintf(stderr, "Unable to query file details\n");
				exit(1);
			}

			struct videotime_s bvt, evt;
			pcr_to_videotime(begin.pcr, &bvt);
			pcr_to_videotime(end.pcr, &evt);

			char *bstr = videotime_to_str(&bvt);
			char *estr = videotime_to_str(&evt);
			char *dstr = videotime_to_str(&streamTime);

			printf("\n");
			printf("file: %s\n", optarg);
			printf("      from %s\n", bstr);
			printf("        to %s\n", estr);
			printf("  duration %s\n", dstr);
			printf("\n");

			free(bstr);
			free(estr);
			free(dstr);

			exit(0);
			}
			break;
		case 's':
			if (str_to_videotime(optarg, &ctx->timeStartStream) < 0) {
				fprintf(stderr, "-s syntax error\n");
				exit(1);
			}
			ctx->opt_s = strdup(optarg);
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (ctx->ifn == 0) {
		usage(argv[0]);
		fprintf(stderr, "\n-i is mandatory\n\n");
		exit(1);
	}

	/* Read the index if it exists */
	int ret = indexLoad(ctx);
	if (ret < 0) {
		/* File is assumed to have properly aligned packets. */
		ctx->ifh = fopen(ctx->ifn, "rb");
		if (!ctx->ifh) {
			fprintf(stderr, "Unable to open input file '%s'\n", ctx->ifn);
			exit(1);
		}
	
		fseeko(ctx->ifh, 0, SEEK_END);
		off_t fileLength = ftell(ctx->ifh);
		fseeko(ctx->ifh, 0, SEEK_SET);


		int blen = ((16 * 1048576) / 188) * 188;
		unsigned char *buf = malloc(blen);
		double pct;
		while (!feof(ctx->ifh)) {
			off_t pos = ftello(ctx->ifh);
			int rlen = fread(buf, 1, blen, ctx->ifh);
			if (rlen <= 0)
				break;

			pct = ((double)pos / (double)fileLength) * 100.0;

			//printf("%" PRIu64 ", %8d items, %.2f%%\r", pos, ctx->allPCRLength, pct);
			printf("Creating index ... %.2f%%\r", pct);
			fflush(stdout);
			struct ltntstools_pcr_position_s *arr = NULL;
			int arrlen = 0;
			int ret = ltntstools_queryPCRs(buf, rlen, pos, &arr, &arrlen);
			if (ret == 0) {
				ctx->allPCRs = realloc(ctx->allPCRs,
					(ctx->allPCRLength * sizeof(struct ltntstools_pcr_position_s)) +
						(arrlen * sizeof(struct ltntstools_pcr_position_s)));
	
				memcpy(ctx->allPCRs + ctx->allPCRLength, arr,
					arrlen * sizeof(struct ltntstools_pcr_position_s));
				ctx->allPCRLength += arrlen;

				free(arr);

			}
		}
		fclose(ctx->ifh);
		free(buf);

		indexSave(ctx);

		printf("\rdone.\n");
		exit(0);
	}

	{
	}

	/* Display all SCRs between start and end */
	int64_t pcrStart = ctx->pcrMin;
	int64_t pcrEnd = ctx->pcrMax;

	if (ctx->opt_s) {
		pcrStart = videotime_to_pcr(&ctx->timeStartStream);
	}
	if (ctx->opt_e) {
		pcrEnd = videotime_to_pcr(&ctx->timeEndStream);
	}

	struct ltntstools_pcr_position_s *s = indexLookupPCR(ctx, pcrStart);
	struct ltntstools_pcr_position_s *e = indexLookupPCR(ctx, pcrEnd);
	indexDumpEntry(ctx, 0, s);
	indexDumpEntry(ctx, 1, e);

	if (ctx->ofn) {
		FILE *ifh = fopen(ctx->ifn, "rb");
		if (ifh) {
			FILE *ofh = fopen(ctx->ofn, "wb");
			if (ofh) {
				fseeko(ifh, s->offset, SEEK_SET);
				int blen = 188 * 64;
				char *pkts = malloc(blen);
				for (uint64_t begin = s->offset; begin < e->offset; begin += blen) {
					int rlen = fread(pkts, 1, blen, ifh);
					fwrite(pkts, 1, rlen, ofh);
					printf("Writing ... %.02f%%\r", ((double)(begin - s->offset) / (double)(e->offset - s->offset)) * 100.0);
					fflush(stdout);
				}
				printf("\ndone.\n");

				fclose(ofh);
			}
			fclose(ifh);
		}
	}

	if (ctx->ifn)
		free(ctx->ifn);
	if (ctx->ofn)
		free(ctx->ofn);

	return 0;
}