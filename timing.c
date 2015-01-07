/* Copyright (C) Ulrich Drepper <drepper@redhat.com>, 2001-2005.  */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <getopt.h>
#include <gmp.h>
#include <limits.h>
#include <paths.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/wait.h>


static void get_time_diff (clock_t cl, bool clock_round,
			   const struct timespec *start, struct timespec *res,
			   unsigned long int *cntrp);
static void time_stats (const struct timespec *all_times,
			unsigned long int timecount, unsigned long int cut,
			struct timespec *avg, mpz_t *q, mpz_t *r);


static const struct option longopts[] =
{
  { "drop", required_argument, NULL, 'd' },
  { "help", no_argument, NULL, 'h' },
  { "ignore", no_argument, NULL, 'n' },
  { "output", required_argument, NULL, 'o' },
  { NULL, 0, NULL, 0 }
};

static unsigned long int count = 30;
static unsigned long int nparallel = 1;
static unsigned long int cut_percentage = 2;


static int
time_compare (const void *p1, const void *p2)
{
  const struct timespec *t1 = (const struct timespec *) p1;
  const struct timespec *t2 = (const struct timespec *) p2;

  if (t1->tv_sec < t2->tv_sec)
    return -1;
  if (t1->tv_sec > t2->tv_sec)
    return 1;
  if (t1->tv_nsec < t2->tv_nsec)
    return -1;
  if (t1->tv_nsec > t2->tv_nsec)
    return 1;
  return 0;
}


int
main (int argc, char *argv[])
{
  int opt;
  unsigned long int i;
  char *new_argv[2 + argc];
  char *new_env[5 + argc];
  int nenv = 0;
  bool verbose = false;
  bool relocations = false;
  bool ignore_status = false;
  const char *s;
  const char *outfname = NULL;
  int outfd = -1;

#define ADD_VAR(name) \
  if (strchr (name, '=') != NULL)					      \
    new_env[nenv++] = name;						      \
  else									      \
    {									      \
      s = getenv (name);						      \
      if (s != NULL)							      \
	{								      \
	  size_t l = strlen (s);					      \
	  new_env[nenv] = alloca (strlen (name) + 1 + l + 1);		      \
	  strcpy (stpcpy (stpcpy (new_env[nenv], name), "="), s);	      \
	  ++nenv;							      \
	}								      \
    }

  while ((opt = getopt_long (argc, argv, "+c:E:hno:p:rv", longopts, NULL))
	 != -1)
    switch (opt)
      {
      case 'c':
	{
	  char *endp;
	  count = strtoul (optarg, &endp, 0);
	  if (endp == optarg || *endp != '\0')
	    error (EXIT_FAILURE, 0, "invalid argument for -c parameter");
	}
	break;

      case 'd':
	{
	  char *endp;
	  unsigned long int newcut = strtoul (optarg, &endp, 0);
	  if (*endp == '\0')
	    cut_percentage = newcut;
	}
	break;

      case 'E':
 printf("optarg=\"%s\"\n", optarg);
	ADD_VAR (optarg);
	break;

      case 'h':
	fprintf (stderr, "%s [OPTION]... COMMAND [PARAMS]...\n\
\n\
  -c N         Repeat command N times\n\
  -d N         Drop the N percent best and worst results\n\
  -E VAR       Copy environment variable VAR\n\
  -o FNAME     Write output to FNAME\n\
  -r           Measure time used for relocations\n\
  -v           Be verbose\n\
  -n, --ignore Ignore exist status of the application\n",
		 argv[0]);
	exit (0);

      case 'o':
	outfname = optarg;
	break;

      case 'r':
	relocations = true;

      case 'v':
	verbose = true;
	break;

      case 'n':
	ignore_status = true;
	break;

      case 'p':
	{
	  char *endp;
	  nparallel = strtoul (optarg, &endp, 0);
	  if (endp == optarg || *endp != '\0')
	    error (EXIT_FAILURE, 0, "invalid argument for -p parameter");
	}
	break;

      case '?':
	break;

      default:
	abort ();
    }

  if (optind >= argc)
    error (EXIT_FAILURE, 0, "need at least one non-option parameter");

  /* Close all file descriptors except the 3 standard ones.  */
  DIR *dir = opendir ("/proc/self/fd");
  struct dirent *d;
  while ((d = readdir (dir)) != NULL)
    if (d->d_type == DT_UNKNOWN || d->d_type == DT_REG)
      {
	char *endp;
	long int num = strtol (d->d_name, &endp, 10);
	if (num > STDERR_FILENO && num != dirfd (dir) && *endp == '\0')
	  close (num);
      }
  closedir (dir);

  if (outfname != NULL)
    {
      outfd = open64 (outfname, O_RDWR | O_CREAT | O_APPEND | O_NOCTTY, 0666);
      if (outfd == -1)
	error (0, errno, "cannot open %s", outfname);
    }
  if (outfd == -1)
    {
      outfd = open (_PATH_TTY, O_RDWR | O_NOCTTY);
      if (outfd == -1)
	outfd = STDERR_FILENO;
    }
  if (outfd != STDERR_FILENO)
    fcntl (outfd, F_SETFD, FD_CLOEXEC);

  clockid_t cl;
  bool use_clock_cpu = false;
  struct timespec *all_times_rt = NULL;
  struct timespec *all_times_cpu = NULL;
  if (!relocations)
    {
      all_times_rt
	= (struct timespec *) malloc (count * sizeof (*all_times_rt));
      if (all_times_rt == NULL)
	error (EXIT_FAILURE, errno, "while allocating array for times");

      if (clock_getcpuclockid (0, &cl) == 0)
	{
	  use_clock_cpu = true;

	  all_times_cpu
	    = (struct timespec *) malloc (count * sizeof (*all_times_cpu));
	  if (all_times_cpu == NULL)
	    error (EXIT_FAILURE, errno, "while allocating array for times");
	}
      else if (verbose)
	puts ("no CPU clock found");
    }

  char *command = argv[optind];

  for (i = 0; optind + i < argc; ++i)
    new_argv[i] = argv[optind + i];
  new_argv[i] = NULL;

  ADD_VAR ("PATH");
  ADD_VAR ("LD_LIBRARY_PATH");

  if (relocations)
    {
      new_env[nenv++] = "LD_DEBUG=statistics";
      new_env[nenv++] = "LD_DEBUG_OUTPUT=/tmp/timing";
    }
  new_env[nenv] = NULL;

  unsigned long int timecount_rt = 0;
  unsigned long int timecount_cpu = 0;

  if (relocations)
    nparallel = 1;

  unsigned long long int min_total = ULONG_LONG_MAX;
  unsigned long long int min_relocs = 0;
  unsigned long long int min_load = 0;
  unsigned long long int max_total = 0;
  unsigned long long int max_relocs = 0;
  unsigned long long int max_load = 0;
  unsigned long long int sum_total = 0;
  unsigned long long int sum_relocs = 0;
  unsigned long long int sum_load = 0;
  for (i = 1; i <= count; ++i)
    {
      pid_t pid[nparallel];
      int status;
      struct timespec start_rt;
      struct timespec start_cpu;
      bool clock_round_rt = false;
      bool clock_round_cpu = false;

      if (!relocations)
	{
	  if (clock_gettime (CLOCK_REALTIME, &start_rt) == 0)
	    clock_round_rt = true;
	  else if (verbose && i == 1)
	    puts ("clock_gettime failed");

	  if (use_clock_cpu && clock_gettime (cl, &start_cpu) == 0)
	    clock_round_cpu = true;
	  else if (verbose && i == 1)
	    puts ("clock_gettime failed");
	}

      unsigned long int j;
      for (j = 0; j < nparallel; ++j)
	if (posix_spawnp (&pid[j], command, NULL,  NULL, new_argv, new_env)
	    != 0)
	  error (EXIT_FAILURE, errno, "cannot spawn");

      for (j = 0; j < nparallel; ++j)
	if (waitpid (pid[j], &status, 0) != pid[j])
	  error (EXIT_FAILURE, 0, "process other than child terminated");

      if (relocations)
	{
	  char fname[sizeof "/tmp/timing" + 24];
	  FILE *fp;

	  snprintf (fname, sizeof fname, "/tmp/timing.%ld", (long int) pid);
	  fp = fopen (fname, "r");
	  if (fp != NULL)
	    {
	      char *line = NULL;
	      size_t len = 0;
	      unsigned long long int total = 0;
	      unsigned long long int relocs = 0;
	      unsigned long long int load = 0;

	      __fsetlocking (fp, FSETLOCKING_BYCALLER);

	      while (! feof_unlocked (fp))
		{
		  char *cp;
		  ssize_t n = getline (&line, &len, fp);
		  if (n <= 0)
		    break;

		  if ((cp = strstr (line,
				    "total startup time in dynamic loader:"))
		      != NULL)
		    sscanf (cp, "total startup time in dynamic loader: %llu",
			    &total);
		  else if ((cp = strstr (line, "time needed for relocation:"))
			   != NULL)
		    sscanf (cp, "time needed for relocation: %llu",
			    &relocs);
		  else if ((cp = strstr (line, "time needed to load objects:"))
			   != NULL)
		    sscanf (cp, "time needed to load objects: %llu",
			    &load);
		}

	      free (line);
	      fclose (fp);
	      remove (fname);

	      if (total != 0 && relocs != 0 && load != 0)
		{
		  if (total < min_total)
		    {
		      min_total = total;
		      min_relocs = relocs;
		      min_load = load;
		    }

		  if (total > max_total)
		    {
		      max_total = total;
		      max_relocs = relocs;
		      max_load = load;
		    }

		  sum_total += total;
		  sum_relocs += relocs;
		  sum_load += load;

		  ++timecount_rt;
		}
	    }
	}
      else
	{
	  get_time_diff (CLOCK_REALTIME, clock_round_rt, &start_rt,
			 &all_times_rt[i - 1], &timecount_rt);

	  get_time_diff (cl, clock_round_cpu, &start_cpu,
			 &all_times_cpu[i - 1], &timecount_cpu);
	}

      if (WEXITSTATUS (status) != 0 && !ignore_status)
	error (EXIT_FAILURE, 0, "child terminated abnormally");
    }

  if (timecount_rt > 0 || timecount_cpu > 0)
    {
      if (relocations)
	dprintf (outfd,
		 "minimum: total=%llu cyc, relocs=%llu cyc, load=%llu cyc\n"
		 "maximum: total=%llu cyc, relocs=%llu cyc, load=%llu cyc\n"
		 "average: total=%llu cyc, relocs=%llu cyc, load=%llu cyc\n",
		 min_total, min_relocs, min_load,
		 max_total, max_relocs, max_load,
		 sum_total / timecount_rt, sum_relocs / timecount_rt,
		 sum_load / timecount_rt);
      else
	{
	  /* Sort the result.  */
	  qsort (all_times_rt, timecount_rt, sizeof (all_times_rt[0]),
		 time_compare);
	  qsort (all_times_cpu, timecount_cpu, sizeof (all_times_cpu[0]),
		 time_compare);

	  /* Remove the best and worst results.  */
	  unsigned long int cut1 = (timecount_rt * cut_percentage + 50) / 100;
	  unsigned long int cut2 = (timecount_cpu * cut_percentage + 50) / 100;
	  if (cut1 > 0 || cut2 > 1)
	    dprintf (outfd, MAX (cut1, cut2) == 1
		     ? "Strip out best and worst realtime result\n"
		     : "Strip out best and worst %lu realtime results\n",
		     MAX (cut1, cut2));

	  struct timespec avg_rt;
	  mpz_t q_rt;
	  mpz_t r_rt;
	  time_stats (all_times_rt, timecount_rt, cut1, &avg_rt, &q_rt, &r_rt);

	  struct timespec avg_cpu;
	  mpz_t q_cpu;
	  mpz_t r_cpu;
	  time_stats (all_times_cpu, timecount_cpu, cut2, &avg_cpu, &q_cpu,
		      &r_cpu);

	  dprintf (outfd,
		   "minimum: %lu.%09lu sec real / %lu.%09lu sec CPU\n"
		   "maximum: %lu.%09lu sec real / %lu.%09lu sec CPU\n"
		   "average: %lu.%09lu sec real / %lu.%09lu sec CPU\n"
		   "stdev  : %lu.%09lu sec real / %lu.%09lu sec CPU\n",
		   (unsigned long int) all_times_rt[0].tv_sec,
		   (unsigned long int) all_times_rt[0].tv_nsec,
		   (unsigned long int) all_times_cpu[0].tv_sec,
		   (unsigned long int) all_times_cpu[0].tv_nsec,
		   (unsigned long int) all_times_rt[timecount_rt - 1].tv_sec,
		   (unsigned long int) all_times_rt[timecount_rt - 1].tv_nsec,
		   (unsigned long int) all_times_cpu[timecount_cpu - 1].tv_sec,
		   (unsigned long int) all_times_cpu[timecount_cpu - 1].tv_nsec,
		   (unsigned long int) avg_rt.tv_sec,
		   (unsigned long int) avg_rt.tv_nsec,
		   (unsigned long int) avg_cpu.tv_sec,
		   (unsigned long int) avg_cpu.tv_nsec,
		   mpz_get_ui (q_rt), mpz_get_ui (r_rt),
		   mpz_get_ui (q_cpu), mpz_get_ui (r_cpu));
	}
    }

  return 0;
}


static void
get_time_diff (clock_t cl, bool clock_round, const struct timespec *start,
	       struct timespec *res, unsigned long int *cntrp)
{
  if (clock_round && clock_gettime (cl, res) == 0)
    {
      if (res->tv_nsec < start->tv_nsec)
	{
	  res->tv_nsec = 1000000000 + res->tv_nsec - start->tv_nsec;
	  res->tv_sec -= 1 + start->tv_sec;
	}
      else
	{
	  res->tv_nsec -= start->tv_nsec;
	  res->tv_sec -= start->tv_sec;
	}

      if (res->tv_sec != 0 || res->tv_nsec != 0)
	++*cntrp;
    }
  else
    {
      res->tv_nsec -= start->tv_nsec;
      res->tv_sec -= start->tv_sec;
    }
}


static void
time_stats (const struct timespec *all_times, unsigned long int timecount,
	    unsigned long int cut, struct timespec *avg, mpz_t *q, mpz_t *r)
{
  if (cut > 0)
    {
      all_times += cut;
      timecount -= 2 * cut;
    }

  /* Some up the remaining times.  */
  unsigned long long int temp = 0;
  for (unsigned long int i = 0; i < timecount; ++i)
    temp += all_times[i].tv_sec * 1000000000ull + all_times[i].tv_nsec;
  /* Compute the average.  */
  temp /= timecount;
  /* Convert to the usual format.  */
  avg->tv_nsec = temp % 1000000000ull;
  avg->tv_sec = temp / 1000000000ull;

  /* Compute the standard unbiased deviation.  */
  mpz_t sumsqdif;
  mpz_init (sumsqdif);

  for (unsigned long int i = 0; i < timecount; ++i)
    if (all_times[i].tv_sec != 0 || all_times[i].tv_nsec != 0)
      {
	struct timespec dif;

	dif.tv_sec = all_times[i].tv_sec - avg->tv_sec;
	dif.tv_nsec = all_times[i].tv_nsec - avg->tv_nsec;
	if (dif.tv_nsec < 0)
	  {
	    dif.tv_nsec += 1000000000;
	    --dif.tv_sec;
	  }

	mpz_t tmp;
	mpz_init (tmp);
	mpz_set_si (tmp, dif.tv_sec);
	mpz_mul_ui (tmp, tmp, 1000000000);
	mpz_add_ui (tmp, tmp, dif.tv_nsec);
	mpz_mul (tmp, tmp, tmp);

	mpz_add (sumsqdif, sumsqdif, tmp);

	mpz_clear (tmp);
      }

  /* Note: the unbiased standard deviation.  Otherwise the sum would
     be divided by COUNT.  */
  if (timecount > 1)
    {
      mpz_add_ui (sumsqdif, sumsqdif, (timecount - 1) / 2);
      mpz_tdiv_q_ui (sumsqdif, sumsqdif, timecount - 1);
    }
  mpz_sqrt (sumsqdif, sumsqdif);

  mpz_init (*q);
  mpz_init (*r);

  mpz_tdiv_qr_ui (*q, *r, sumsqdif, 1000000000);
}
