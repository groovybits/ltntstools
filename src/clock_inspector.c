/*
 * The clock inspector extracts and plots differnt clocks from a MPEG-TS stream and
 * performs some lightweight math to measure distances, intervals, timeliness.
 *
 * In file input mode, measurements such as 'walltime drift' or Timestamp often make
 * no sense because the input stream is arriving faster than realtime.
 * 
 * In stream/udp input cases, values such ed 'filepos' make no real sense but instead
 * represents bytes received.
 * 
 * If you ignore small nuances like this, the tool is meaningfull in many ways.
 *
 * When using the -s mode to report PCR timing, it's important that the correct PCR
 * pid value is passed using -S. WIthout this, the PCR is assumed to be on a default pid
 * and some of the SCR reported data will be incorrect, even though most of it gets
 * autotected. **** make sure you have the -S option set of you care about reading
 * the SCR reports.
 * 
 * SCR (PCR) reporting
 * +SCR Timing         filepos ------------>                   SCR  <--- SCR-DIFF ------>  SCR             Walltime ----------------------------->  Drift
 * +SCR Timing             Hex           Dec   PID       27MHz VAL       TICKS         uS  Timecode        Now                      secs               ms
 * SCR #000000003 -- 000056790        354192  0031    959636022118      944813      34993  0.09:52:22.074  Fri Feb  9 09:13:52 2024 1707488033.067      0
 *                                                                       (since last PCR)                 
 */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>

#include "klbitstream_readwriter.h"
#include <libltntstools/ltntstools.h>
#include "xorg-list.h"
#include "ffmpeg-includes.h"
#include "kl-lineartrend.h"

#define DEFAULT_SCR_PID 0x31
#define DEFAULT_TREND_SIZE (60 * 60 * 60) /* 1hr */
#define DEFAULT_TREND_REPORT_PERIOD 15

struct ordered_clock_item_s {
	struct xorg_list list;

	uint64_t nr;
	int64_t clock;
	uint64_t filepos;
};

struct pid_s
{
	/* TS packets */
	uint64_t pkt_count;
	uint32_t cc;
	uint64_t cc_errors;

	/* PCR / SCR */
	uint64_t scr_first;
	time_t   scr_first_time;
	uint64_t scr;
	uint64_t scr_updateCount;

	/* Four vars that track when each TS packet arrives, and what SCR timestamp was during
	 * arrival. We use this to broadly measure the walltime an entire pess took to arrive,
	 * and the SCR ticket it took.
	 */
	uint64_t scr_at_pes_unit_header;
	uint64_t scr_last_seen; /* last scr when this pid was seen. Avoiding change 'scr' pid for now, risky? */
	struct timeval scr_at_pes_unit_header_ts;
	struct timeval scr_last_seen_ts;

	/* PTS */
	uint64_t pts_count;
	struct ltn_pes_packet_s pts_last;
	int64_t pts_diff_ticks;
	uint64_t pts_last_scr; /* When we captured the last packet, this reflects the SCR at the time. */
	struct ltntstools_clock_s clk_pts;
	struct {
		pthread_mutex_t trendLock; /* Lock the trend when we add or when we clone the struct */
		struct kllineartrend_context_s *clkToScrTicksDeltaTrend;
		time_t last_clkToScrTicksDeltaTrend;
		time_t last_clkToScrTicksDeltaTrendReport; /* Recall whenever we've output a trend report */
		double counter;
		int inserted_counter;
		double first_x;
		double first_y;
	} trend_pts, trend_dts;

	int clk_pts_initialized;

	/* DTS */
	uint64_t dts_count;
	struct ltn_pes_packet_s dts_last;
	int64_t dts_diff_ticks;
	uint64_t dts_last_scr; /* When we captured the last packet, this reflects the SCR at the time. */
	struct ltntstools_clock_s clk_dts;
	int clk_dts_initialized;

	/* Working data for PTS / DTS */
	struct ltn_pes_packet_s pes;

	struct xorg_list ordered_pts_list;
};

struct tool_context_s
{
	int enableNonTimingConformantMessages;
	int enableTrendReport;
	int enablePESDeliveryReport;
	int dumpHex;
	int trendSize;
	int reportPeriod;
	const char *iname;
	time_t initial_time;
	time_t current_stream_time;
	int64_t maxAllowablePTSDTSDrift;
//	uint32_t pid;
	struct pid_s pids[8192];
	pthread_t trendThreadId;
	int trendThreadComplete;

	int doPacketStatistics;
	int doSCRStatistics;
	int doPESStatistics;
	int pts_linenr;
	int scr_linenr;
	int ts_linenr;

	uint64_t ts_total_packets;

	int order_asc_pts_output;

	int scr_pid;

	struct ltntstools_stream_statistics_s *libstats;
};

static int gRunning = 1;
static void signal_handler(int signum)
{
	gRunning = 0;
}

/* Ordered PTS handling */
static void ordered_clock_init(struct xorg_list *list)
{
	xorg_list_init(list);
}

/* The clock is a PTS 90KHz counter */
static void ordered_clock_insert(struct xorg_list *list, struct ordered_clock_item_s *src)
{
	struct ordered_clock_item_s *e = calloc(1, sizeof(*e));
	memcpy(e, src, sizeof(*src));

	if (xorg_list_is_empty(list)) {
		xorg_list_append(&e->list, list);
		return;
	}

	/* Search the list backwards */
	struct ordered_clock_item_s *item = NULL;
	xorg_list_for_each_entry_reverse(item, list, list) {
		if (src->clock >= item->clock) {
			__xorg_list_add(&e->list, &item->list, item->list.next);
			return;
		}
	}
}

static void ordered_clock_dump(struct xorg_list *list, unsigned short pid)
{
	int64_t last = -1;
	uint64_t diffTicks = 0;

	int linenr = 0;

	struct ordered_clock_item_s *i = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(i, next, list, list) {
		if (last == -1) {
			diffTicks = 0;
		} else {
			diffTicks = ltntstools_pts_diff(last, i->clock);
		}

		if (linenr++ == 24) {
			linenr = 0;
			printf("+PTS/DTS (ordered) filepos ------------>               PTS/DTS  <------- DIFF ------>\n");
			printf("+PTS/DTS #             Hex           Dec   PID       90KHz VAL       TICKS         MS\n");
		}

		printf("PTS #%09" PRIi64 " -- %09" PRIx64 " %13" PRIu64 "  %04x  %14" PRIi64 "  %10" PRIi64 " %10.2f\n",
			i->nr,
			i->filepos,
			i->filepos,
			pid,
			i->clock,
			diffTicks,
			(double)diffTicks / 90);

		last = i->clock;
	}
}

/* End: Ordered PTS handling */

static void pidReport(struct tool_context_s *ctx)
{
	double total = ctx->ts_total_packets;
	for (int i = 0; i <= 0x1fff; i++) {
		if (ctx->pids[i].pkt_count) {
			printf("pid: 0x%04x pkts: %12" PRIu64 " discontinuities: %12" PRIu64 " using: %7.1f%%\n",
				i,
				ctx->pids[i].pkt_count,
				ctx->pids[i].cc_errors,
				((double)ctx->pids[i].pkt_count / total) * 100.0);
		}
	}
}

static void printTrend(struct tool_context_s *ctx, uint16_t pid, struct kllineartrend_context_s *trend, pthread_mutex_t *mutex)
{
	/* Lock the struct, briefly prevent additional adds */
	pthread_mutex_lock(mutex);
	struct kllineartrend_context_s *trendDup = kllineartrend_clone(trend);
	if (!trendDup) {
		pthread_mutex_unlock(mutex);
		return;
	}
	pthread_mutex_unlock(mutex);

	if (ctx->enableTrendReport >= 2) {
		/* If the caller passes -L twice or more, save data set on every print.
		 */
		/* Don't need the mutex */
		kllineartrend_save_csv(trendDup, trendDup->name);
	}
	if (ctx->enableTrendReport >= 3) {
		/* If the caller passes -L three times or more, print the entire data set on every print.
		 * expensive console processing. Choose wisely my friend.
		 */
		/* Don't need the mutex */
		kllineartrend_printf(trendDup);
	}

	struct timeval t1, t2, t3;
	double slope, intersect, deviation, r2;

	gettimeofday(&t1, NULL);
	kllineartrend_calculate(trendDup, &slope, &intersect, &deviation);
	gettimeofday(&t2, NULL);
	kllineartrend_calculate_r_squared(trendDup, slope, intersect, &r2);
	gettimeofday(&t3, NULL);

#if 0
	/* slope calculate takes 1ms for 216000 entries (LTN573), r2 calculation is twice as long */
	int a_diffus = ltn_timeval_subtract_us(&t2, &t1);
	int b_diffus = ltn_timeval_subtract_us(&t3, &t2);

	printf("Trend calculation for %d/%d elements took %dus, r2 took %dus.\n",
		trendDup->count, trendDup->maxCount, a_diffus, b_diffus);
#endif

	char t[64];
	time_t now = time(NULL);
	sprintf(t, "%s", ctime(&now));
	t[ strlen(t) - 1] = 0;

	printf("PID 0x%04x - Trend '%s', %8d entries, Slope %18.8f, Deviation is %12.2f, r2 is %12.8f @ %s\n",
		pid,
		trendDup->name,
		trendDup->count,
		slope, deviation, r2, t);

	kllineartrend_free(trendDup);
}

static void trendReportFree(struct tool_context_s *ctx)
{
	for (int i = 0; i <= 0x1fff; i++) {
		if (ctx->pids[i].trend_pts.clkToScrTicksDeltaTrend) {
			kllineartrend_free(ctx->pids[i].trend_pts.clkToScrTicksDeltaTrend);
		}
		if (ctx->pids[i].trend_dts.clkToScrTicksDeltaTrend) {
			kllineartrend_free(ctx->pids[i].trend_dts.clkToScrTicksDeltaTrend);
		}
	}
}

static void trendReport(struct tool_context_s *ctx)
{
	for (int i = 0; i <= 0x1fff; i++) {
		if (ctx->pids[i].trend_pts.clkToScrTicksDeltaTrend) {
			printTrend(ctx, i, ctx->pids[i].trend_pts.clkToScrTicksDeltaTrend, &ctx->pids[i].trend_pts.trendLock);
		}
		if (ctx->pids[i].trend_dts.clkToScrTicksDeltaTrend) {
			printTrend(ctx, i, ctx->pids[i].trend_dts.clkToScrTicksDeltaTrend, &ctx->pids[i].trend_dts.trendLock);
		}
	}
}

void *trend_report_thread(void *tool_context)
{
    struct tool_context_s *ctx = tool_context;
	pthread_detach(ctx->trendThreadId);

	time_t next = time(NULL) + ctx->reportPeriod;
    while (ctx->enableTrendReport && gRunning) {
		usleep(250 * 1000);
		if (time(NULL) < next)
			continue;

        printf("Dumping trend report(s)\n");
        trendReport(ctx);
		next = time(NULL) + ctx->reportPeriod;
    }
	ctx->trendThreadComplete = 1;

    return NULL;
}

static ssize_t processPESHeader(uint8_t *buf, uint32_t lengthBytes, uint32_t pid, struct tool_context_s *ctx, uint64_t filepos, struct timeval ts,
	int64_t prior_pes_delivery_ticks,
	int64_t prior_pes_delivery_us)
{
	char time_str[64];

	time_t now = time(NULL);
	sprintf(time_str, "%s", ctime(&now));
	time_str[ strlen(time_str) - 1] = 0;

	struct pid_s *p = &ctx->pids[pid];

	if ((p->pes.PTS_DTS_flags == 2) || (p->pes.PTS_DTS_flags == 3)) {
		ltn_pes_packet_copy(&p->pts_last, &p->pes);

		if (p->clk_pts_initialized == 0) {
			p->clk_pts_initialized = 1;
			ltntstools_clock_initialize(&p->clk_pts);
			ltntstools_clock_establish_timebase(&p->clk_pts, 90000);
			ltntstools_clock_establish_wallclock(&p->clk_pts, p->pes.PTS);
		}
		ltntstools_clock_set_ticks(&p->clk_pts, p->pes.PTS);

		/* Initialize the trend if needed */
		if (p->trend_pts.clkToScrTicksDeltaTrend == NULL) {
			char label[64];
			sprintf(&label[0], "PTS 0x%04x to Wallclock delta", pid);
			pthread_mutex_init(&p->trend_pts.trendLock, NULL);
			p->trend_pts.clkToScrTicksDeltaTrend = kllineartrend_alloc(ctx->trendSize, label);
		}
	}
	if (p->pes.PTS_DTS_flags == 3) {
		ltn_pes_packet_copy(&p->dts_last, &p->pes);

		if (p->clk_dts_initialized == 0) {
			p->clk_dts_initialized = 1;
			ltntstools_clock_initialize(&p->clk_dts);
			ltntstools_clock_establish_timebase(&p->clk_dts, 90000);
			ltntstools_clock_establish_wallclock(&p->clk_dts, p->pes.DTS);
		}
		ltntstools_clock_set_ticks(&p->clk_dts, p->pes.DTS);

		if (p->trend_dts.clkToScrTicksDeltaTrend == NULL) {
			char label[64];
			sprintf(&label[0], "DTS 0x%04x to SCR tick delta", pid);
			pthread_mutex_init(&p->trend_dts.trendLock, NULL);
			p->trend_dts.clkToScrTicksDeltaTrend = kllineartrend_alloc(ctx->trendSize, label);
		}
	}

	struct klbs_context_s pbs, *bs = &pbs;
	klbs_init(bs);
	klbs_read_set_buffer(bs, buf, lengthBytes);

	ssize_t len = ltn_pes_packet_parse(&p->pes, bs, 1 /* SkipDataExtraction */);

	/* Track the difference in SCR clocks between this PTS header and the prior. */
	uint64_t pts_scr_diff_ms = 0;
	uint64_t dts_scr_diff_ms = 0;

	if ((p->pes.PTS_DTS_flags == 2) || (p->pes.PTS_DTS_flags == 3)) {
		p->pts_diff_ticks = ltntstools_pts_diff(p->pts_last.PTS, p->pes.PTS);
		if (p->pts_diff_ticks > (10 * 90000)) {
			p->pts_diff_ticks -= MAX_PTS_VALUE;
		}
		p->pts_count++;
		//p->scr = ctx->pids[ctx->scr_pid].scr;
		pts_scr_diff_ms = ltntstools_scr_diff(p->pts_last_scr, p->scr) / 27000;
		p->pts_last_scr = p->scr;
	}
	if (p->pes.PTS_DTS_flags == 3) {
		p->dts_diff_ticks = ltntstools_pts_diff(p->pts_last.DTS, p->pes.DTS);
		p->dts_count++;
		dts_scr_diff_ms = ltntstools_scr_diff(p->dts_last_scr, p->scr) / 27000;
		p->dts_last_scr = p->scr;
	}

	if (ctx->pts_linenr++ == 0) {
		printf("+PTS/DTS Timing       filepos ------------>               PTS/DTS  <------- DIFF ------> <---- SCR <--PTS*300--------->  Walltime ----------------------------->  Drift\n");
		printf("+PTS/DTS Timing           Hex           Dec   PID       90KHz VAL       TICKS         MS   Diff MS  minus SCR        ms  Now                      secs               ms\n");
	}
	if (ctx->pts_linenr > 24)
		ctx->pts_linenr = 0;

	/* Process a PTS if present. */
	if ((p->pes.PTS_DTS_flags == 2) || (p->pes.PTS_DTS_flags == 3)) {

		int64_t ptsWalltimeDriftMs = 0;
		if (p->clk_pts_initialized) {
			ptsWalltimeDriftMs = ltntstools_clock_get_drift_ms(&p->clk_pts);
		}

		/* Calculate the offset between the PTS and the last good SCR, assumed to be on pid DEFAULR_SCR_PID. */
		int64_t pts_minus_scr_ticks = (p->pes.PTS * 300) - ctx->pids[ctx->scr_pid].scr;
		double d_pts_minus_scr_ticks = pts_minus_scr_ticks;
		d_pts_minus_scr_ticks /= 27000.0;

		/* Update the PTS/SCR linear trends. */
		p->trend_pts.last_clkToScrTicksDeltaTrend = now;
		p->trend_pts.counter++;
		p->trend_pts.inserted_counter++;
		if (p->trend_pts.counter > 16) {
			/* allow the first few samples to flow through the model and be ignored.
			 */
#if 0
			pthread_mutex_lock(&p->trend_pts.trendLock);
			kllineartrend_add(p->trend_pts.clkToScrTicksDeltaTrend, p->trend_pts.counter, d_pts_minus_scr_ticks);
			pthread_mutex_unlock(&p->trend_pts.trendLock);
#else
			struct timeval t1;
			gettimeofday(&t1, NULL);
			double x, y;

			x = t1.tv_sec + t1.tv_usec / 1000000.0;
			y = p->pes.PTS / 90000.0;
			if (p->trend_pts.first_x == 0)
				p->trend_pts.first_x = x;
			if (p->trend_pts.first_y == 0)
				p->trend_pts.first_y = y;

			pthread_mutex_lock(&p->trend_pts.trendLock);
			kllineartrend_add(p->trend_pts.clkToScrTicksDeltaTrend, x - p->trend_pts.first_x, y - p->trend_pts.first_y);
			pthread_mutex_unlock(&p->trend_pts.trendLock);
#endif
		}

		if (d_pts_minus_scr_ticks < 0 && ctx->enableNonTimingConformantMessages) {
			char str[64];
			sprintf(str, "%s", ctime(&ctx->current_stream_time));
			str[ strlen(str) - 1] = 0;
			printf("!PTS #%09" PRIi64 " Error. The PTS is arriving BEHIND the PCR, the PTS is late. The stream is not timing conformant @ %s\n",
				p->pts_count,
				str);
		}

		if ((PTS_TICKS_TO_MS(p->pts_diff_ticks)) >= ctx->maxAllowablePTSDTSDrift) {
			char str[64];
			sprintf(str, "%s", ctime(&ctx->current_stream_time));
			str[ strlen(str) - 1] = 0;
			printf("!PTS #%09" PRIi64 " Error. Difference between previous and current 90KHz clock >= +-%" PRIi64 "ms (is %" PRIi64 ") @ %s\n",
				p->pts_count,
				ctx->maxAllowablePTSDTSDrift,
				PTS_TICKS_TO_MS(p->pts_diff_ticks),
				str);
		}

		if ((pts_scr_diff_ms) >= ctx->maxAllowablePTSDTSDrift) {
			char str[64];
			sprintf(str, "%s", ctime(&ctx->current_stream_time));
			str[ strlen(str) - 1] = 0;
			printf("!PTS #%09" PRIi64 " Error. Difference between previous and current PTS frame measured in SCR ticks >= +-%" PRIi64 "ms (is %" PRIi64 ") @ %s\n",
				p->pts_count,
				ctx->maxAllowablePTSDTSDrift,
				pts_scr_diff_ms,
				str);
		}

		if (!ctx->order_asc_pts_output) {
			printf("PTS #%09" PRIi64
				" -- %011" PRIx64
				" %13" PRIu64
				"  %04x  "
				"%14" PRIi64
				"  %10" PRIi64
				" %10.2f %9" PRIi64
				" %10" PRIi64
				" %9.2f  %s %08d.%03d %6" PRIi64 "\n",
				p->pts_count,
				filepos,
				filepos,
				pid,
				p->pes.PTS,
				p->pts_diff_ticks,
				(double)p->pts_diff_ticks / 90,
				pts_scr_diff_ms,
				pts_minus_scr_ticks,
				d_pts_minus_scr_ticks,
				time_str,
				(int)ts.tv_sec,
				(int)ts.tv_usec / 1000,
				ptsWalltimeDriftMs);

			if (ctx->enablePESDeliveryReport) {
				printf("!PTS #%09" PRIi64 "                              %04x took %10" PRIi64 " SCR ticks to arrive, or %9.03f ms, %9" PRIi64 " uS walltime %s\n",
					p->pts_count - 1,
					pid,
					prior_pes_delivery_ticks,
					(double)prior_pes_delivery_ticks / 27000.0,
					prior_pes_delivery_us,
					prior_pes_delivery_ticks == 0 ? "(probably delivered in a single SCR interval period, so basically no ticks measured)" : "");
			}
		}

		if (ctx->order_asc_pts_output) {
			if (p->pts_count == 1) {
				ordered_clock_init(&p->ordered_pts_list);
			}
			
			struct ordered_clock_item_s item;
			item.nr = p->pts_count;
			item.clock = p->pes.PTS;
			item.filepos = filepos;
			ordered_clock_insert(&p->ordered_pts_list, &item);
		}

	}
	/* Process a DTS if present. */
	if (p->pes.PTS_DTS_flags == 3) {

		/* Disabled for now, TODO */
		int64_t dtsWalltimeDriftMs = 0;
		if (p->clk_dts_initialized) {
			dtsWalltimeDriftMs = ltntstools_clock_get_drift_ms(&p->clk_dts);
		}

		/* Calculate the offset between the DTS and the last good SCR, assumed to be on pid DEFAULT_SCR_PID. */
		int64_t dts_minus_scr_ticks = (p->pes.DTS * 300) - ctx->pids[ctx->scr_pid].scr;
		double d_dts_minus_scr_ticks = dts_minus_scr_ticks;
		d_dts_minus_scr_ticks /= 27000.0;

		/* Update the DTS/SCR linear trends. */
		p->trend_dts.last_clkToScrTicksDeltaTrend = now;
		p->trend_dts.counter++;
		if (p->trend_dts.counter > 16) {
			/* allow the first few samples to flow through the model and be ignored.
			 */
#if 0
			pthread_mutex_lock(&p->trend_dts.trendLock);
			kllineartrend_add(p->trend_dts.clkToScrTicksDeltaTrend, p->trend_dts.counter, d_dts_minus_scr_ticks);
			pthread_mutex_unlock(&p->trend_dts.trendLock);
#else
			struct timeval t1;
			gettimeofday(&t1, NULL);
			double x, y;

			x = t1.tv_sec + t1.tv_usec / 1000000.0;
			y = p->pes.DTS / 90000.0;
			if (p->trend_dts.first_x == 0)
				p->trend_dts.first_x = x;
			if (p->trend_dts.first_y == 0)
				p->trend_dts.first_y = y;

			pthread_mutex_lock(&p->trend_dts.trendLock);
			kllineartrend_add(p->trend_dts.clkToScrTicksDeltaTrend, x - p->trend_dts.first_x, y - p->trend_dts.first_y);
			pthread_mutex_unlock(&p->trend_dts.trendLock);
#endif
		}

		if ((PTS_TICKS_TO_MS(p->dts_diff_ticks)) >= ctx->maxAllowablePTSDTSDrift) {
			char str[64];
			sprintf(str, "%s", ctime(&ctx->current_stream_time));
			str[ strlen(str) - 1] = 0;
			printf("!DTS #%09" PRIi64 " Error. Difference between previous and current 90KHz clock >= +-%" PRIi64 "ms (is %" PRIi64 ") @ %s\n",
				p->dts_count,
				ctx->maxAllowablePTSDTSDrift,
				PTS_TICKS_TO_MS(p->pts_diff_ticks),
				str);
		}

		if ((dts_scr_diff_ms) >= ctx->maxAllowablePTSDTSDrift) {
			char str[64];
			sprintf(str, "%s", ctime(&ctx->current_stream_time));
			str[ strlen(str) - 1] = 0;
			printf("!DTS #%09" PRIi64 " Error. Difference between previous and current DTS frame measured in SCR ticks >= +-%" PRIi64 "ms (is %" PRIi64 ") @ %s\n",
				p->dts_count,
				ctx->maxAllowablePTSDTSDrift,
				dts_scr_diff_ms,
				str);
		}

		printf("DTS #%09" PRIi64
			" -- %011" PRIx64
			" %13" PRIu64
			"  %04x  "
			"%14" PRIi64
			"  %10" PRIi64
			" %10.2f %9" PRIi64
			" %10" PRIi64
			" %9.2f  %s %08d.%03d %6" PRIi64 "\n",
			p->pts_count,
			filepos,
			filepos,
			pid,
			p->pes.DTS,
			p->dts_diff_ticks,
			(double)p->dts_diff_ticks / 90,
			dts_scr_diff_ms,
			dts_minus_scr_ticks,
			d_dts_minus_scr_ticks,
			time_str,
			(int)ts.tv_sec,
			(int)ts.tv_usec / 1000,
			dtsWalltimeDriftMs);
	}

	if (len > 0 && ctx->doPESStatistics > 1) {
		ltn_pes_packet_dump(&p->pes, "    ");
	}

	return len;
}

static void processSCRStats(struct tool_context_s *ctx, uint8_t *pkt, uint64_t filepos, struct timeval ts)
{
	uint16_t pid = ltntstools_pid(pkt);

	uint64_t scr;
	if (ltntstools_scr(pkt, &scr) < 0)
		return;

	uint64_t scr_diff = 0;
	if (ctx->pids[pid].scr_updateCount > 0) {
		scr_diff = ltntstools_scr_diff(ctx->pids[pid].scr, scr);
	} else {
		ctx->pids[pid].scr_first = scr;
		ctx->pids[pid].scr_first_time = ctx->initial_time;
	}

	ctx->pids[pid].scr = scr;

	if (ctx->scr_linenr++ == 0) {
		printf("+SCR Timing           filepos ------------>                   SCR  <--- SCR-DIFF ------>  SCR             Walltime ----------------------------->  Drift\n");
		printf("+SCR Timing               Hex           Dec   PID       27MHz VAL       TICKS         uS  Timecode        Now                      secs               ms\n");
	}

	if (ctx->scr_linenr > 24)
		ctx->scr_linenr = 0;

	time_t dt = ctx->pids[pid].scr_first_time;
	dt += (ltntstools_scr_diff(ctx->pids[pid].scr_first, scr) / 27000000);

	ctx->current_stream_time = dt;

	char str[64];
	sprintf(str, "%s", ctime(&dt));
	str[ strlen(str) - 1] = 0;

	char *scr_ascii = NULL;
	ltntstools_pcr_to_ascii(&scr_ascii, scr);

	ctx->pids[pid].scr_updateCount++;

	char walltimePCRReport[32] = { 0 };
	int64_t PCRWalltimeDriftMs = 0;
	if (ltntstools_pid_stats_pid_get_pcr_walltime_driftms(ctx->libstats, pid, &PCRWalltimeDriftMs) == 0) {
		sprintf(walltimePCRReport, "%5" PRIi64, PCRWalltimeDriftMs);
	} else {
		sprintf(walltimePCRReport, "    NA");
	}

	time_t now = time(NULL);
	char time_str[64];
	sprintf(time_str, "%s", ctime(&now));
	time_str[ strlen(time_str) - 1] = 0;

	printf("SCR #%09" PRIu64 " -- %011" PRIx64 " %13" PRIu64 "  %04x  %14" PRIu64 "  %10" PRIu64 "  %9" PRIu64 "  %s  %s %08d.%03d %6s\n",
		ctx->pids[pid].scr_updateCount,
		filepos,
		filepos,
		pid,
		scr,
		scr_diff,
		scr_diff / 27,
		scr_ascii,
		time_str,
		(int)ts.tv_sec,
		(int)ts.tv_usec / 1000,
		walltimePCRReport);

	free(scr_ascii);
}

static void processPacketStats(struct tool_context_s *ctx, uint8_t *pkt, uint64_t filepos, struct timeval ts)
{
	uint16_t pid = ltntstools_pid(pkt);
	ctx->pids[pid].pkt_count++;

	uint32_t cc = ltntstools_continuity_counter(pkt);

	if (ctx->dumpHex) {
		if (ctx->ts_linenr++ == 0) {
			printf("+TS Packet         filepos ------------>\n");
			printf("+TS Packet             Hex           Dec   PID  Packet --------------------------------------------------------------------------------------->\n");
		}
		if (ctx->ts_linenr > 24)
			ctx->ts_linenr = 0;

		printf("TS  #%09" PRIu64 " -- %08" PRIx64 " %13" PRIu64 "  %04x  ",
			ctx->ts_total_packets,
			filepos,
			filepos,
			pid);
	}

	if (ctx->dumpHex == 1) {
		ltntstools_hexdump(pkt, 32, 32 + 1); /* +1 avoid additional trailing CR */
	} else
	if (ctx->dumpHex == 2) {
		ltntstools_hexdump(pkt, 188, 32);
	}

	uint32_t afc = ltntstools_adaption_field_control(pkt);
	if ((afc == 1) || (afc == 3)) {
		/* Every pid will be in error the first occurece. Check on second and subsequent pids. */
		if (ctx->pids[pid].pkt_count > 1) {
			if (((ctx->pids[pid].cc + 1) & 0x0f) != cc) {
				/* Don't CC check null pid. */
				if (pid != 0x1fff) {
					char str[64];
					sprintf(str, "%s", ctime(&ctx->current_stream_time));
					str[ strlen(str) - 1] = 0;
					printf("!CC Error. PID %04x expected %02x got %02x @ %s\n",
						pid, (ctx->pids[pid].cc + 1) & 0x0f, cc, str);
					ctx->pids[pid].cc_errors++;
				}
			}
		}
	}
	ctx->pids[pid].cc = cc;
}

static void processPESStats(struct tool_context_s *ctx, uint8_t *pkt, uint64_t filepos, struct timeval ts)
{
	uint16_t pid = ltntstools_pid(pkt);
	struct pid_s *p = &ctx->pids[pid];
	int64_t prior_pes_delivery_ticks;
	int64_t prior_pes_delivery_us;

	int peshdr = ltntstools_payload_unit_start_indicator(pkt);

	int pesoffset = 0;
	if (peshdr) {
		pesoffset = ltntstools_contains_pes_header(pkt + 4, 188 - 4);

		/* Calculate how long the PREVIOUS PES took to arrive in SCR ticks. */
		prior_pes_delivery_ticks = p->scr_last_seen - p->scr_at_pes_unit_header;
		prior_pes_delivery_us = ltn_timeval_subtract_us(&p->scr_last_seen_ts, &p->scr_at_pes_unit_header_ts);

		p->scr_at_pes_unit_header = ctx->pids[ctx->scr_pid].scr;
		p->scr_at_pes_unit_header_ts = ts;
	} else {
		/* make a note of the last user SCR for this packet on this pid */
		p->scr_last_seen = ctx->pids[ctx->scr_pid].scr;
		p->scr_last_seen_ts = ts;
	}

	if (peshdr && pesoffset >= 0 && pid > 0) {
		processPESHeader(pkt + 4 + pesoffset, 188 - 4 - pesoffset, pid, ctx, filepos, ts, prior_pes_delivery_ticks, prior_pes_delivery_us);
	}
}

static int validateLinearTrend()
{
	//double vals[10] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 1.0, 1.0 }; /* rsq = 1, slope = 1 */
	double vals[10] = { 2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0, 1.0, 2.0 }; /* rsq = 1, slope = 2 */
	//double vals[10] = { 0.01, 0.02, 0.03, 0.04, 0.05, 0.06, 0.07, 0.08, 1.0, 0.01 };
	//double vals[10] = { 1.0, 0.0, 1.0, 0.2, 1.0, 0.4, 1.0, 0.6, 0.985348, -0.04761905 }; /// ?

	struct kllineartrend_context_s *tc = kllineartrend_alloc(128, "linear trend test");

	double counter = 0;
	for (int i = 0; i < 8; i++) {
		kllineartrend_add(tc, ++counter, vals[i]);
	}

	kllineartrend_printf(tc);

	double slope, intersect, deviation, r2;
	kllineartrend_calculate(tc, &slope, &intersect, &deviation);
	kllineartrend_calculate_r_squared(tc, slope, intersect, &r2);

	printf("Slope %17.8f Deviation is %12.2f, r is %f\n", slope, deviation, r2);
	if (r2 != vals[8]) {
		printf("Rsquared calculation doesn't match excel\n");
	}
	if (slope != vals[9]) {
		printf("slope calculation doesn't match excel\n");
	}

	kllineartrend_free(tc);

	return -1;
}

static int validateClockMath()
{
	/* Setup a PCR measurement unit as a 27MHz clock.
	 * We're going to simulate it moving forward in time and
	 * observe how we measure it as ir naturally wraps around
	 * it's upper value limit.
	 * */
	struct ltntstools_clock_s pcrclk;
	ltntstools_clock_initialize(&pcrclk);
	ltntstools_clock_establish_timebase(&pcrclk, 27 * 1e6);

	int64_t pcr_increment = 27 * 1e6; /* 1 second in a 27MHz clock */
	int64_t pcr = MAX_SCR_VALUE - (pcr_increment * 6); /* Start the PCR N frames behind the wrap */
	int64_t elapsed_us = 0;
	struct timeval t1, t2;

	while (1) {
		gettimeofday(&t1, NULL);

		if (ltntstools_clock_is_established_wallclock(&pcrclk) == 0) {
			/* Associate the current walltime to this PCR time (1 * 27), minus 10 frames of 59.94 */
			ltntstools_clock_establish_wallclock(&pcrclk, pcr);
		}

		/* PCR wraps across maximum value */
		ltntstools_clock_set_ticks(&pcrclk, pcr);

		int64_t us = ltntstools_clock_get_drift_us(&pcrclk);

		/* Negative drift indicates PCR falling behind walltime */
		char *s = NULL;
		ltntstools_pcr_to_ascii(&s, pcr);
		printf("pcr %13" PRIi64 " '%s', drift us: %5" PRIi64 ", sleep processing elapsed %7" PRIi64 "\n",
			pcr,
			s,
			us, elapsed_us);
		free(s);

		if (pcr >= MAX_SCR_VALUE) {
			printf("PCR has wrapped\n");
			pcr -= MAX_SCR_VALUE;
		}

		sleep(1);
		gettimeofday(&t2, NULL);

		elapsed_us = ltn_timeval_subtract_us(&t2, &t1);
		pcr += (elapsed_us * 27); /* one second in 27MHz clock */

		/* The PCR willnaturally fall behind wall time by 1 us every few seconds, 
		 * because all of this non-timed processing isn't accounted for, such as 
		 * subtarction, getting the time itself etc.
		 */

	}

	return 0;
}

static void kernel_check_socket_sizes(AVIOContext *i)
{
	printf("Kernel configured default/max socket buffer sizes:\n");

	char line[256];
	int val;
	FILE *fh = fopen("/proc/sys/net/core/rmem_default", "r");
	if (fh) {
		fread(&line[0], 1, sizeof(line), fh);
		val = atoi(line);
		printf("/proc/sys/net/core/rmem_default = %d\n", val);
		fclose(fh);
	}

	fh = fopen("/proc/sys/net/core/rmem_max", "r");
	if (fh) {
		fread(&line[0], 1, sizeof(line), fh);
		val = atoi(line);
		printf("/proc/sys/net/core/rmem_max = %d\n", val);
		if (i->buffer_size > val) {
			fprintf(stderr, "buffer_size %d exceeds rmem_max %d, aborting\n", i->buffer_size, val);
			exit(1);
		}
		fclose(fh);
	}

}

static void usage(const char *progname)
{
	printf("A tool to extract PCR/SCR PTS/DTS clocks from all pids in a MPEGTS file, or stream.\n");
	printf("Usage:\n");
	printf("  -i <url> Eg: udp://227.1.20.45:4001?localaddr=192.168.20.45\n"
		"           192.168.20.45 is the IP addr where we'll issue a IGMP join\n");
	printf("  -T YYYYMMDDHHMMSS [def: current time]\n");
	printf("     Time is only relevant when running -s SCR mode. The tool will adjust\n");
	printf("     the initial SCR to match walltime, then any other SCR it reports will\n");
	printf("     be reported as initial walltime plus SCR difference. Useful when\n");
	printf("     trying to match TS files to other logging mechanisms based on datetime\n");
	printf("  -d Dump every ts packet header in hex to console (use additional -d for more detail)\n");
	printf("  -s Dump SCR/PCR time, adjusting for -T initial time if necessary\n");
	printf("  -S <0xpid> Use SCR on this pid. [def: 0x%04x]\n", DEFAULT_SCR_PID);
	printf("  -p Dump PTS/DTS (use additional -p to show PES header on console)\n");
	printf("  -D Max allowable PTS/DTS clock drift value in ms. [def: 700]\n");
	printf("  -R Reorder the PTS display output to be in ascending PTS order [def: disabled]\n");
	printf("     In this case we'll calculate the PTS intervals reliably based on picture frame display order [def: disabled]\n");
	printf("     This mode casuses all PES headers to be cached (growing memory usage over time), it's memory expensive.\n");
	printf("  -P Show progress indicator as a percentage when processing large files [def: disabled]\n");
	printf("  -Z Suppress any warnings relating to non-conformant stream timing issues [def: warnings are output]\n");
	printf("  -L Enable printing of PTS to SCR linear trend report [def: no]\n");
	printf("  -Y Enable printing of 'PES took x ms' walltime and tick delivery times within a stream [def: no]\n");
	printf("  -t <#seconds>. Stop after N seconds [def: 0 - unlimited]\n");
	printf("  -A <number> default trend size [def: %d]\n", DEFAULT_TREND_SIZE);
	printf("      108000 is 1hr of 30fps, 216000 is 1hr of 60fps, 5184000 is 24hrs of 60fps\n");
	printf("  -B <seconds> trend report output period [def: %d]\n", DEFAULT_TREND_REPORT_PERIOD);

	printf("\n  Example UDP or RTP:\n");
	printf("    tstools_clock_inspector -i 'udp://227.1.20.80:4002?localaddr=192.168.20.45&buffer_size=2500000&overrun_nonfatal=1&fifo_size=50000000' -S 0x31 -p\n");
}

int clock_inspector(int argc, char *argv[])
{
	int ch;

	struct tool_context_s *ctx = calloc(1, sizeof(*ctx));
	ctx->doPacketStatistics = 1;
	ctx->doSCRStatistics = 0;
	ctx->doPESStatistics = 0;
	ctx->maxAllowablePTSDTSDrift = 700;
	ctx->scr_pid = DEFAULT_SCR_PID;
	ctx->enableNonTimingConformantMessages = 1;
	ctx->enableTrendReport = 0;
	ctx->trendSize = DEFAULT_TREND_SIZE;
	ctx->reportPeriod = DEFAULT_TREND_REPORT_PERIOD;
	int progressReport = 0;
	int stopSeconds = 0;

	/* We use this specifically for tracking PCR walltime drift */
	ltntstools_pid_stats_alloc(&ctx->libstats);

    while ((ch = getopt(argc, argv, "?dhi:spt:A:B:T:D:LPRS:X:YZ")) != -1) {
		switch (ch) {
		case 'A':
			ctx->trendSize = atoi(optarg);
			if (ctx->trendSize < 60) {
				ctx->trendSize = 60;
			}
			break;
		case 'B':
			ctx->reportPeriod = atoi(optarg);
			if (ctx->reportPeriod < 5) {
				ctx->reportPeriod = 5;
			}
			break;
		case 'd':
			ctx->dumpHex++;
			break;
		case 'i':
			ctx->iname = optarg;
			break;
		case 'p':
			ctx->doSCRStatistics = 1; /* We need SCR stats also, because some of the PES stats make reference to the SCR */
			ctx->doPESStatistics++;
			break;
		case 'L':
			ctx->enableTrendReport++;
			break;
		case 'P':
			progressReport = 1;
			break;
		case 's':
			ctx->doSCRStatistics = 1;
			break;
		case 'S':
			if ((sscanf(optarg, "0x%x", &ctx->scr_pid) != 1) || (ctx->scr_pid > 0x1fff)) {
				usage(argv[0]);
				exit(1);
			}
			ltntstools_pid_stats_pid_set_contains_pcr(ctx->libstats, ctx->scr_pid);
			break;
		case 'D':
			ctx->maxAllowablePTSDTSDrift = atoi(optarg);
			break;
		case 'R':
			ctx->order_asc_pts_output = 1;
			break;
		case 'T':
			{
				//time_t mktime(struct tm *tm);
				struct tm tm = { 0 };
				if (sscanf(optarg, "%04d%02d%02d%02d%02d%02d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
					&tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
				{
					usage(argv[0]);
					fprintf(stderr, "-T invalid datetime\n");
					exit(1);
				}
				tm.tm_year -= 1900;
				tm.tm_mon -= 1;
				ctx->initial_time = mktime(&tm);
			}
			break;
		case 'Y':
			ctx->enablePESDeliveryReport = 1;
			break;
		case 't':
			stopSeconds = atoi(optarg);
			break;
		case 'X':
			/* Keep valgrind happy */
			ltntstools_pid_stats_free(ctx->libstats);
			ctx->libstats = NULL;

			if (atoi(optarg) == 1) {
				return validateClockMath();
			} else
			if (atoi(optarg) == 2) {
				return validateLinearTrend();
			}
		case 'Z':
			ctx->enableNonTimingConformantMessages = 0;
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (ctx->initial_time == 0) {
		time(&ctx->initial_time);
	}

	if (ctx->iname == 0) {
		usage(argv[0]);
		fprintf(stderr, "\nError, -i is mandatory, aborting\n\n");
		exit(1);
	}

	int blen = 188 * 1024;
	uint8_t *buf = malloc(blen);
	if (!buf) {
		fprintf(stderr, "Unable to allocate buffer\n");
		exit(1);
	}

	uint64_t fileLengthBytes = 0;
	FILE *fh = fopen(ctx->iname, "rb");
	if (fh) {
		fseeko(fh, 0, SEEK_END);
		fileLengthBytes = ftello(fh);
		fclose(fh);
	} else {
		progressReport = 0;
	}

	pthread_create(&ctx->trendThreadId, NULL, trend_report_thread, ctx);

	/* TODO: Replace this with avio so we can support streams. */
	avformat_network_init();
	AVIOContext *puc;
	int ret = avio_open2(&puc, ctx->iname, AVIO_FLAG_READ | AVIO_FLAG_NONBLOCK | AVIO_FLAG_DIRECT, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, "-i error, unable to open file or url\n");
		return 1;
	}

	kernel_check_socket_sizes(puc);
	if (ctx->enableTrendReport) {
		printf("Enabled Linear Trend reporting for PTS to SCR deltas\n");
	}

	signal(SIGINT, signal_handler);

	time_t stopTime = time(NULL) + stopSeconds;

	/* TODO: Migrate this to use the source-avio.[ch] framework */
	uint64_t filepos = 0;
	uint64_t streamPosition = 0;
	while (gRunning) {

		if (stopSeconds) {
			time_t now = time(NULL);
			if (now > stopTime) {
				signal_handler(1);
			}
		}

		int rlen = avio_read(puc, buf, blen);
		if (rlen == -EAGAIN) {
			usleep(1 * 1000);
			continue;
		}
		if (rlen < 0)
			break;

		streamPosition += rlen;

		/* Push the entire stream into the stats layer - so we can compyte walltime */
		ltntstools_pid_stats_update(ctx->libstats, buf, rlen / 188);

		for (int i = 0; i < rlen; i += 188) {

			filepos = (streamPosition - rlen) + i;

			uint8_t *p = (buf + i);

			struct timeval ts;
			gettimeofday(&ts, NULL);

			if (ctx->doPacketStatistics) {
				processPacketStats(ctx, p, filepos, ts);
			}

			if (ctx->doSCRStatistics) {
				processSCRStats(ctx, p, filepos, ts);
			}

			if (ctx->doPESStatistics) {
				/* Big caveat here: We expect the PES header to be contained
				 * somewhere (anywhere) in this single packet, and we only parse
				 * enough bytes to extract PTS and DTS.
				 */
				processPESStats(ctx, p, filepos, ts);
			}

			ctx->ts_total_packets++;

		}
		if (progressReport) {
			fprintf(stderr, "\rprocessing ... %.02f%%",
				(double)(((double)filepos / (double)fileLengthBytes) * 100.0));
		}
	}
	avio_close(puc);
	while (ctx->trendThreadComplete != 1) {
		usleep(50 * 1000);
	}

	if (progressReport) {
		fprintf(stderr, "\ndone\n");
	}

	printf("\n");
	pidReport(ctx);
	if (ctx->enableTrendReport) {
		trendReport(ctx);
		trendReportFree(ctx);
	}

	if (ctx->libstats) {
		ltntstools_pid_stats_free(ctx->libstats);
		ctx->libstats = NULL;
	}

	free(buf);

	if (ctx->order_asc_pts_output) {
		for (int i = 0; i <= 0x1fff; i++) {
			if (ctx->pids[i].pts_count > 0) {
				ordered_clock_dump(&ctx->pids[i].ordered_pts_list, i);
			}
		}
	}

	free(ctx);
	return 0;
}
