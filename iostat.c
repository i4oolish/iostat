/*
 * iostat.c v2.2
 * Linux I/O performance monitoring utility
 *
 * Special thanks to Stephen C. Tweedie's for his first version of
 * "sard" I/O counters for the Linux kernel. Without his work this
 * wouldn't be possible.
 *
 * Original iostat code by Greg Franks (Mar 10 1999)
 * 
 * Maintenance by Zlatko Calusic <zlatko@iskon.hr>
 *
 * v1.4 - Apr  7 2002, Zlatko Calusic <zlatko@iskon.hr>,
 *      - SMP compatibility, other bugfixes, cleanups...
 *
 * v1.5 - Apr 10 2002, Zlatko Calusic <zlatko@iskon.hr>
 *      - heavily modified & cleaned up, adapted to 2.5.8-pre3 sard patch
 *
 * v1.6 - Sep 24 2002, Rick Lindsley <ricklind@us.ibm.com>
 *      - modified to understand new disk stats (2.5.38)
 *
 * v2.0 - Jan  6 2004, Zlatko Calusic <zlatko@iskon.hr>
 *      - major release, support for both 2.4 & 2.6 stable kernels
 *
 * v2.1 - Nov 25 2004, Zlatko Calusic <zlatko@iskon.hr>
 *      - just added license info (GPL)
 *
 * v2.2 - Feb 14 2005, Arnaud Desitter <arnaud.desitter@ouce.ox.ac.uk>
 *                     Zlatko Calusic <zlatko@iskon.hr>
 *      - adapt to in kernel scan formats, fixes to avoid overflows
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/param.h>
#include <linux/major.h>

#ifndef IDE_DISK_MAJOR
#define IDE_DISK_MAJOR(M) ((M) == IDE0_MAJOR || (M) == IDE1_MAJOR || \
			   (M) == IDE2_MAJOR || (M) == IDE3_MAJOR || \
			   (M) == IDE4_MAJOR || (M) == IDE5_MAJOR || \
			   (M) == IDE6_MAJOR || (M) == IDE7_MAJOR || \
			   (M) == IDE8_MAJOR || (M) == IDE9_MAJOR)
#endif	/* !IDE_DISK_MAJOR */

#ifndef SCSI_DISK_MAJOR
#ifndef SCSI_DISK8_MAJOR
#define SCSI_DISK8_MAJOR 128
#endif
#ifndef SCSI_DISK15_MAJOR
#define SCSI_DISK15_MAJOR 135
#endif
#define SCSI_DISK_MAJOR(M) ((M) == SCSI_DISK0_MAJOR || \
			   ((M) >= SCSI_DISK1_MAJOR && \
			    (M) <= SCSI_DISK7_MAJOR) || \
			   ((M) >= SCSI_DISK8_MAJOR && \
			    (M) <= SCSI_DISK15_MAJOR))
#endif	/* !SCSI_DISK_MAJOR */

#define MAX_PARTITIONS 64

struct part_info {
	unsigned int major;	/* Device major number */
	unsigned int minor;	/* Device minor number */
	char name[32];
} partition[MAX_PARTITIONS];

struct blkio_info {
	unsigned int rd_ios;	/* Read I/O operations */
	unsigned int rd_merges;	/* Reads merged */
	unsigned long long rd_sectors; /* Sectors read */
	unsigned int rd_ticks;	/* Time in queue + service for read */
	unsigned int wr_ios;	/* Write I/O operations */
	unsigned int wr_merges;	/* Writes merged */
	unsigned long long wr_sectors; /* Sectors written */
	unsigned int wr_ticks;	/* Time in queue + service for write */
	unsigned int ticks;	/* Time of requests in queue */
	unsigned int aveq;	/* Average queue length */
} new_blkio[MAX_PARTITIONS], old_blkio[MAX_PARTITIONS];

struct cpu_info {
	unsigned long long user;
	unsigned long long system;
	unsigned long long idle;
	unsigned long long iowait;
} new_cpu, old_cpu;

FILE *iofp;			/* /proc/diskstats or /proc/partition */
FILE *cpufp;			/* /proc/stat */
char *opts = "cdDpPxh";		/* Options */
char buffer[256];		/* Temporary buffer for parsing */

int print_cpu = 0;
int print_disk_extended = 0;
int print_disk_util = 0;
int print_partition = 0;
int print_device = 1;

unsigned int n_partitions;	/* Number of partitions */
unsigned int ncpu;		/* Number of processors */
unsigned int kernel;		/* Kernel: 4 (2.4, /proc/partitions)
				        or 6 (2.6, /proc/diskstats) */

void print_usage()
{
	fputs("iostat v2.2, (C) 1999-2005 by "
	      "Greg Franks, Zlatko Calusic, Rick Lindsley, Arnaud Desitter\n"
	      "Distributed under the terms of the GPL (see LICENSE file)\n"
	      "Usage: iostat [-cdDpPxh] [disks...] [interval [count]]\n"
	      "options:\n\n"
	      "\tc - print cpu usage info\n"
	      "\td - print basic disk info\n"
	      "\tD - print disk utilization info\n"
	      "\tp - print partition info also\n"
	      "\tP - print partition info only\n"
	      "\tx - print extended disk info\n"
	      "\th - this help\n\n", stderr);
	exit(EXIT_SUCCESS);
}

void handle_error(const char *string, int error)
{
	if (error) {
		fputs("iostat: ", stderr);
		if (errno)
			perror(string);
		else
			fprintf(stderr, "%s\n", string);
		exit(EXIT_FAILURE);
	}
}

void get_number_of_cpus()
{
	FILE *ncpufp = fopen("/proc/cpuinfo", "r");

	handle_error("Can't open /proc/cpuinfo", !ncpufp);
	while (fgets(buffer, sizeof(buffer), ncpufp)) {
		if (!strncmp(buffer, "processor\t:", 11))
			ncpu++;
	}
	fclose(ncpufp);
	handle_error("Error parsing /proc/cpuinfo", !ncpu);
}

int printable(unsigned int major, unsigned int minor)
{
	if (IDE_DISK_MAJOR(major)) {
		return (!(minor & 0x3F) && print_device)
			|| ((minor & 0x3F) && print_partition);
	} else if (SCSI_DISK_MAJOR(major)) {
		return (!(minor & 0x0F) && print_device)
			|| ((minor & 0x0F) && print_partition);
	} else {
		return 1;	/* if uncertain, print it */
	}
}

/* Get partition names.  Check against match list */
void initialize(char **match_list, int n_dev)
{
	const char *scan_fmt = NULL;

	switch (kernel) {
	case 4:
		scan_fmt = "%4d %4d %*d %31s %u";
		break;
	case 6:
		scan_fmt = "%4d %4d %31s %u";
		break;
	}
	handle_error("logic error in initialize()", !scan_fmt);

	while (fgets(buffer, sizeof(buffer), iofp)) {
		unsigned int reads = 0;
		struct part_info curr;

		if (sscanf(buffer, scan_fmt, &curr.major, &curr.minor,
			   curr.name, &reads) == 4) {
			unsigned int p;

			for (p = 0; p < n_partitions
				     && (partition[p].major != curr.major
					 || partition[p].minor != curr.minor);
			     p++);

			if (p == n_partitions && p < MAX_PARTITIONS) {
				if (n_dev) {
					unsigned int j;

					for (j = 0; j < n_dev && match_list[j];
					     j++) {
						if (!strcmp(curr.name,
							    match_list[j])) {
							partition[p] = curr;
							n_partitions = p + 1;
						}
					}
                                } else if (reads && printable(curr.major,
							      curr.minor)) {
                                        partition[p] = curr;
                                        n_partitions = p + 1;
                                }
                        }
		}
	}
}

void get_kernel_stats()
{
	const char *scan_fmt = NULL;

	switch (kernel) {
	case 4:
		scan_fmt = "%4d %4d %*d %*s %u %u %llu %u %u %u %llu %u %*u %u %u";
		break;
	case 6:
		scan_fmt = "%4d %4d %*s %u %u %llu %u %u %u %llu %u %*u %u %u";
		break;
	}
	handle_error("logic error in get_kernel_stats()", !scan_fmt);

	rewind(iofp);
	while (fgets(buffer, sizeof(buffer), iofp)) {
		int items;
		struct part_info curr;
		struct blkio_info blkio;

		items = sscanf(buffer, scan_fmt,
			       &curr.major, &curr.minor,
			       &blkio.rd_ios, &blkio.rd_merges,
			       &blkio.rd_sectors, &blkio.rd_ticks, 
			       &blkio.wr_ios, &blkio.wr_merges,
			       &blkio.wr_sectors, &blkio.wr_ticks,
			       &blkio.ticks, &blkio.aveq);

		/*
		 * Unfortunately, we can report only transfer rates
		 * for partitions in 2.6 kernels, all other I/O
		 * statistics are unavailable.
		 */
		if (items == 6) {
			blkio.rd_sectors = blkio.rd_merges;
			blkio.wr_sectors = blkio.rd_ticks;
			blkio.rd_ios = 0;
			blkio.rd_merges = 0;
			blkio.rd_ticks = 0;
			blkio.wr_ios = 0;
			blkio.wr_merges = 0;
			blkio.wr_ticks = 0;
			blkio.ticks = 0;
			blkio.aveq = 0;
			items = 12;
		}
			
		if (items == 12) {
			unsigned int p;

			/* Locate partition in data table */
			for (p = 0; p < n_partitions; p++) {
				if (partition[p].major == curr.major
				    && partition[p].minor == curr.minor) {
					new_blkio[p] = blkio;
					break;
				}
			}
		}
	}

	rewind(cpufp);
	while (fgets(buffer, sizeof(buffer), cpufp)) {
		if (!strncmp(buffer, "cpu ", 4)) {
			int items;
			unsigned long long nice, irq, softirq;

			items = sscanf(buffer,
				     "cpu %llu %llu %llu %llu %llu %llu %llu",
				       &new_cpu.user, &nice,
				       &new_cpu.system,
				       &new_cpu.idle,
				       &new_cpu.iowait,
				       &irq, &softirq);

			new_cpu.user += nice;
			if (items == 4)
				new_cpu.iowait = 0;
			if (items == 7)
				new_cpu.system += irq + softirq;

		}
	}
}

void print_cpu_stats()
{
	double total;
	struct cpu_info cpu;

	cpu.user = new_cpu.user - old_cpu.user;
	cpu.system = new_cpu.system - old_cpu.system;
	cpu.idle = new_cpu.idle - old_cpu.idle;
	cpu.iowait = new_cpu.iowait - old_cpu.iowait;
	total = (cpu.user + cpu.system + cpu.idle + cpu.iowait) / 100.0;
	printf("%3.0f %3.0f ", cpu.user / total, cpu.system / total);
	if (kernel == 6)
		printf("%3.0f ", cpu.iowait / total);
	printf("%3.0f", cpu.idle / total);
}

/*
 * Print out statistics.
 * extended form is:
 *   read merges
 *   write merges
 *   read io requests
 *   write io requests
 *   kilobytes read
 *   kilobytes written
 *   average queue length
 *   average waiting time (queue + service)
 *   average service time at disk
 *   average disk utilization.
 */

#define PER_SEC(x) (1000.0 * (x) / deltams)

void print_partition_stats()
{
	unsigned int p;
	double deltams = 1000.0 *
		((new_cpu.user + new_cpu.system +
		  new_cpu.idle + new_cpu.iowait) -
		 (old_cpu.user + old_cpu.system +
		  old_cpu.idle + old_cpu.iowait)) / ncpu / HZ;

	for (p = 0; p < n_partitions; p++) {
		struct blkio_info blkio;
		double n_ios;	 /* Number of requests */
		double n_ticks;	 /* Total service time */
		double n_kbytes; /* Total kbytes transferred */
		double busy;	 /* Utilization at disk	(percent) */
		double svc_t;	 /* Average disk service time */
		double wait;	 /* Average wait */
		double size;	 /* Average request size */
		double queue;	 /* Average queue */

		blkio.rd_ios = new_blkio[p].rd_ios
			- old_blkio[p].rd_ios;
		blkio.rd_merges = new_blkio[p].rd_merges
			- old_blkio[p].rd_merges;
		blkio.rd_sectors = new_blkio[p].rd_sectors
			- old_blkio[p].rd_sectors;
		blkio.rd_ticks = new_blkio[p].rd_ticks
			- old_blkio[p].rd_ticks;
		blkio.wr_ios = new_blkio[p].wr_ios
			- old_blkio[p].wr_ios;
 		blkio.wr_merges = new_blkio[p].wr_merges
			- old_blkio[p].wr_merges; 
		blkio.wr_sectors = new_blkio[p].wr_sectors
			- old_blkio[p].wr_sectors;
		blkio.wr_ticks = new_blkio[p].wr_ticks
			- old_blkio[p].wr_ticks;
		blkio.ticks = new_blkio[p].ticks
			- old_blkio[p].ticks;
		blkio.aveq = new_blkio[p].aveq
			- old_blkio[p].aveq;

		n_ios  = blkio.rd_ios + blkio.wr_ios;
		n_ticks = blkio.rd_ticks + blkio.wr_ticks;
		n_kbytes = (blkio.rd_sectors + blkio.wr_sectors) / 2.0;

		queue = blkio.aveq / deltams;
		size = n_ios ? n_kbytes / n_ios : 0.0;
		wait = n_ios ? n_ticks / n_ios : 0.0;
		svc_t = n_ios ? blkio.ticks / n_ios : 0.0;
		busy = 100.0 * blkio.ticks / deltams; /* percentage! */
		if (busy > 100.0)
			busy = 100.0;

		if (print_disk_extended) {
			printf("%-6s %5.0f %5.0f %6.1f %6.1f %7.1f "
			       "%7.1f %6.1f %5.1f %6.1f %5.1f %3.0f ",
			       partition[p].name,
			       PER_SEC(blkio.rd_merges),
			       PER_SEC(blkio.wr_merges),
			       PER_SEC(blkio.rd_ios),
			       PER_SEC(blkio.wr_ios),
			       PER_SEC(blkio.rd_sectors) / 2.0,
			       PER_SEC(blkio.wr_sectors) / 2.0,
			       size, queue, wait, svc_t, busy);
			if (!p && print_cpu) {
				print_cpu_stats();
			}
			putchar('\n');
		} else if (print_disk_util) {
			printf("%4.0f %4.0f %4.0f  ",
			       PER_SEC(blkio.rd_ios),
			       PER_SEC(blkio.wr_ios),
			       busy);
		} else {
			printf("%5.0f %3.0f %5.1f ",
			       PER_SEC(n_kbytes),
			       PER_SEC(n_ios),
			       svc_t);
		}
	}
}

void print_header_lines()
{
	unsigned int p;

	/* Line 1 */
	if (print_disk_extended) {
		printf("%78s",
		       "extended device statistics                       ");
	} else {
		for (p = 0; p < n_partitions; p++) {
			printf("%9s       ", partition[p].name);
		}
	}

	if (print_cpu)
		printf("      cpu");
	putchar('\n');

	/* Line 2 */
	if (print_disk_extended) {
		printf("device mgr/s mgw/s    r/s    w/s    kr/s    "
		       "kw/s   size queue   wait svc_t  %%b ");
	} else {
		for (p = 0; p < n_partitions; p++) {
			if (print_disk_util)
				printf(" r/s  w/s   %%b  ");
			else
				printf("  kps tps svc_t ");
		}
	}
	if (print_cpu) {
		switch (kernel) {
		case 4:
			printf(" us  sy  id");
			break;
		case 6:
			printf(" us  sy  wt  id");
			break;
		}
	}
	putchar('\n');
}

void process(int lineno)
{
	unsigned int p;

	get_kernel_stats();

	if (!lineno || print_disk_extended)
		print_header_lines();

	print_partition_stats();

	if (!print_disk_extended) {
		if (print_cpu)
			print_cpu_stats();
		putchar('\n');
	}

	/* Save old stats */
	for (p = 0; p < n_partitions; p++)
		old_blkio[p] = new_blkio[p];
	old_cpu = new_cpu;
}

int main(int argc, char **argv)
{
	int c, n_dev, lineno;
	int interval = 1;
	int count = -1;

	setlinebuf(stdout);
	get_number_of_cpus();

	iofp = fopen("/proc/diskstats", "r");
	if (iofp) {
		kernel = 6;
	} else {
		iofp = fopen("/proc/partitions", "r");
		if (iofp)
			kernel = 4;
	}
	handle_error("Can't get I/O statistics on this system", !iofp);

	cpufp = fopen("/proc/stat", "r");
	handle_error("Can't open /proc/stat", !cpufp);

	while ((c = getopt(argc, argv, opts)) != EOF) {
		switch (c) {
		case 'c':
			print_cpu = 1;
			break;
		case 'd':
			print_disk_util = 0;
			break;
		case 'D':
			print_disk_util = 1;
			break;
		case 'P':
			print_device = 0;
			/* falldown */
		case 'p':
			print_partition = 1;
			break;
		case 'x':
			print_disk_extended = 1;
			break;
		case 'h':
		default:
			print_usage();
		}
	}

	/* No options.  Set defaults. */
	if (optind == 1)
		print_cpu = 1;

	/* List of disks/devices [delay [count]]. */
	for (n_dev = 0; optind + n_dev < argc
		     && !isdigit(argv[optind + n_dev][0]); n_dev++);

	initialize(&argv[optind], n_dev);
	optind += n_dev;

	/* Figure out [delay [count]].  Default is one display only */
	switch (argc - optind) {
	case 2:
		count = atoi(argv[optind + 1]);
		/* drop down */
	case 1:
		interval = atoi(argv[optind]);
		break;
	case 0:
		count = 0;
		break;
	default:
		print_usage();
	}

	/* Main loop */
	for (lineno = 0;; lineno = (++lineno) % 21) {
		process(lineno);
		if (count > 0)
			count--;
		if (!count)
			break;
		sleep(interval);
	}
	exit(EXIT_SUCCESS);
}
