/*
  SWARM

  Copyright (C) 2012-2014 Torbjorn Rognes and Frederic Mahe

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  Contact: Torbjorn Rognes <torognes@ifi.uio.no>,
  Department of Informatics, University of Oslo,
  PO Box 1080 Blindern, NO-0316 Oslo, Norway
*/

/*
  This version of the swarm algorithm uses Frederic's idea for d=1 to
  enumerate all of the maximum 7L+4 possible variants of a sequence with only
  one difference, where L is the length of the sequence.
*/

#include "swarm.h"

#define SEPCHAR ' '

#define HASH hash_cityhash64
#define HASHFILLFACTOR 0.5
#define POWEROFTWO
//#define HASHSTATS

struct ampinfo_s
{
  int swarmid;
  int generation;
  int swarms_next;
  int swarm_next;
};

struct ampinfo_s * ampinfo = 0;

int swarms_head;
int swarms_tail;
int current_swarm_tail;

unsigned long hash_tablesize = 0;

/* overall statistics */
static unsigned long maxgen = 0;
static unsigned long largest = 0;

/* per swarm statistics */
static unsigned long singletons = 0;
static unsigned long abundance_sum = 0;
static unsigned long swarmsize = 0;
static unsigned long swarm_maxgen = 0;
static unsigned long swarmed = 0;

pthread_attr_t attr;
pthread_mutex_t mutex_varmatch;
pthread_mutex_t mutex_stderr;

static struct thread_info_s
{
  pthread_t pthread;
  pthread_mutex_t workmutex;
  pthread_cond_t workcond;
  int work;
  unsigned char * varseq;
  int seed;
  unsigned long mut_start;
  unsigned long mut_length;
} * ti;

#ifdef HASHSTATS
unsigned long probes = 0;
unsigned long hits = 0;
unsigned long success = 0;
unsigned long tries  = 0;
unsigned long bingo = 0;
unsigned long collisions = 0;
#endif

int hash_shift;
unsigned long hash_mask;
unsigned char * hash_occupied = 0;
unsigned long * hash_values = 0;
int * hash_data = 0;

inline unsigned long hash_compute_hash(unsigned char * seq, int seqlen)
{
#if 1
  return HASH(seq, seqlen);
#else
  unsigned char compressed[200];
  unsigned char * q = compressed;
  unsigned char * s = seq;

  int chunk = seqlen & 0xffffffe0;
  int rest = seqlen-chunk;
  for (int i=0; i<chunk; i+= 32)
    {
      unsigned long * p = (unsigned long *)(s);
      unsigned long x0 = p[0] - 0x0101010101010101;
      unsigned long x1 = p[1] - 0x0101010101010101;
      unsigned long x2 = p[2] - 0x0101010101010101;
      unsigned long x3 = p[3] - 0x0101010101010101;
      *((unsigned long *)q) = x0 | (x1 << 2) | (x2 << 4) | (x3 << 6);
      s += 32;
      q += 8;
    }

  if (rest > 16)
    {
      unsigned int * p = (unsigned int *)(s);
      unsigned int x0 = p[0] - 0x01010101;
      unsigned int x1 = p[1] - 0x01010101;
      unsigned int x2 = p[2] - 0x01010101;
      unsigned int x3 = p[3] - 0x01010101;
      *((unsigned int *)q) = (x0) | (x1 << 2) | (x2 << 4) | (x3 << 6);
      s += 16;
      q += 4;
      rest -= 16;
    }

  chunk = rest & 0xfffffffc;
  rest = rest - chunk;
  for (int i=0; i<chunk; i+= 4)
    {
      unsigned int * p = (unsigned int *) (s);
      unsigned int x = p[0] - 0x01010101;
      *q++ = x | (x >> 6) | (x >> 12) | (x >> 18);
      s+=4;
    }
  if (rest == 1)
    *q++ = seq[chunk]-1;
  else if (rest == 2)
    *q++ = (seq[chunk]-1) | ((seq[chunk+1]-1) << 2);
  else if (rest == 3)
    *q++ = (seq[chunk]-1) | ((seq[chunk+1]-1) << 2) | ((seq[chunk+2]-1) << 4);
  return (HASH(compressed, q - compressed));
#endif
}

inline unsigned int hash_getindex(unsigned long hash)
{
#ifdef POWEROFTWO
  return hash & hash_mask;
#else
  return hash % hash_tablesize;
#endif
}

inline unsigned int hash_getnextindex(unsigned int j)
{
#ifdef POWEROFTWO
  return (j+1) & hash_mask;
#else
  return (j+1) % hash_tablesize;
#endif
}

void hash_alloc(unsigned long amplicons)
{
  hash_tablesize = 1;
  hash_shift = 0;
  while (amplicons > HASHFILLFACTOR * hash_tablesize)
    {
      hash_tablesize <<= 1;
      hash_shift++;
    }
  hash_mask = hash_tablesize - 1;
  
  hash_occupied =
    (unsigned char *) xmalloc((hash_tablesize + 63) / 8);
  memset(hash_occupied, 0, (hash_tablesize + 63) / 8);

  hash_values =
    (unsigned long *) xmalloc(hash_tablesize * sizeof(unsigned long));

  hash_data =
    (int *) xmalloc(hash_tablesize * sizeof(int));
}

void hash_free()
{
  free(hash_occupied);
  free(hash_values);
  free(hash_data);
}

inline void hash_set_occupied(unsigned int j)
{
  hash_occupied[j >> 3] |= (1 << (j & 7));
}

inline int hash_is_occupied(unsigned int j)
{
  return hash_occupied[j >> 3] & (1 << (j & 7));
}

inline void hash_set_value(unsigned int j, unsigned long hash)
{
  hash_values[j] = hash;
}

inline int hash_compare_value(unsigned int j, unsigned long hash)
{
  return (hash_values[j] == hash);
}

inline void hash_insert(int amp,
			unsigned char * key,
			unsigned long keylen)
{
  unsigned long hash = hash_compute_hash(key, keylen);
  unsigned int j = hash_getindex(hash);
  
  /* find the first empty bucket */
  while (hash_is_occupied(j))
    j = hash_getnextindex(j);
  
  hash_set_occupied(j);
  hash_set_value(j, hash);
  hash_data[j] = amp;
}

void find_variant_matches(unsigned long thread,
			  unsigned char * seq,
			  unsigned long seqlen,
			  int seed)
{
  /* compute hash and corresponding hash table index */

  unsigned long hash = hash_compute_hash(seq, seqlen);
  unsigned int j = hash_getindex(hash);

  /* find matching buckets */

#ifdef HASHSTATS
  tries++;
  probes++;
#endif

  while (hash_is_occupied(j))
    {
#ifdef HASHSTATS
      hits++;
#endif
      if (hash_compare_value(j, hash))
	{
#ifdef HASHSTATS
	  success++;
#endif
	  
	  /* check if not already swarmed */
	  int amp = hash_data[j];
	  struct ampinfo_s * bp = ampinfo + amp;
	  if (!bp->swarmid)
	    {
	      unsigned long ampseqlen = db_getsequencelen(amp);
	      unsigned char * ampseq = (unsigned char *) db_getsequence(amp);
	      
	      /* make sure sequences are identical even though hashes are */
	      if ((ampseqlen == seqlen) && (!memcmp(ampseq, seq, seqlen)))
		{
#ifdef HASHSTATS
		  bingo++;
#endif
		  
		  /* update info */
		  bp->swarmid = ampinfo[seed].swarmid;
		  bp->generation = ampinfo[seed].generation + 1;

		  /* lock mutex before adding this amplicon to swarm */
		  pthread_mutex_lock(&mutex_varmatch);
		  
		  /* add to swarm */
		  ampinfo[current_swarm_tail].swarm_next = amp;
		  current_swarm_tail = amp;
		  swarmed++;
		  
		  /* unlock mutex after adding this amplicon to swarm */
		  pthread_mutex_unlock(&mutex_varmatch);

		  /* output break_swarms info */
		  if (break_swarms)
		    {
		      pthread_mutex_lock(&mutex_stderr);
		      fprintf(stderr, "@@\t");
		      fprint_id_noabundance(stderr, seed);
		      fprintf(stderr, "\t");
		      fprint_id_noabundance(stderr, amp);
		      fprintf(stderr, "\t%d\n", 1);
		      pthread_mutex_unlock(&mutex_stderr);
		    }
		}
#ifdef HASHSTATS
	      else
		{
		  collisions++;
		  
		  fprintf(stderr, "Hash collision between ");
		  fprint_id_noabundance(stderr, seed);
		  fprintf(stderr, " and ");
		  fprint_id_noabundance(stderr, amp);
		  fprintf(stderr, ".\n");
		}
#endif
	    }
	}
      j = hash_getnextindex(j);
#ifdef HASHSTATS
      probes++;
#endif
    }
}

void generate_variants(unsigned long thread,
		       int seed,
		       unsigned long start,
		       unsigned long len)
{
  /* 
     Generate all possible variants involving mutations from position start
     and extending len nucleotides. Insertions in front of those positions
     are included, but not those after. Positions are zero-based.
     The range may extend beyond the the length of the sequence indicating
     that inserts at the end of the sequence should be generated.

     The last thread will handle insertions at the end of the sequence,
     as well as identical sequences (no mutations).
  */

  unsigned char * varseq = ti[thread].varseq;
  
  unsigned char * seq = (unsigned char*) db_getsequence(seed);
  unsigned long seqlen = db_getsequencelen(seed);
  unsigned long end = MIN(seqlen,start+len);

  /* make an exact copy */
  memcpy(varseq, seq, seqlen);
  
#if 1
  /* identical non-variant */
  if (thread == threads -1)
    find_variant_matches(thread, varseq, seqlen, seed);
#endif

  /* substitutions */
  for(int i=start; i<end; i++)
    {
      for (int v=1; v<5; v++)
	if (v != seq[i])
	  {
	    varseq[i] = v;
	    find_variant_matches(thread, varseq, seqlen, seed);
	  }
      varseq[i] = seq[i];
    }

  /* deletions */
  memcpy(varseq, seq, start);
  if (start < seqlen-1)
    memcpy(varseq+start, seq+start+1, seqlen-start-1);
  for(int i=start; i<end; i++)
    {
      if ((i==0) || (seq[i] != seq[i-1]))
	{
	  find_variant_matches(thread, varseq, seqlen-1, seed);      
	}
      varseq[i] = seq[i];
    }
  
  /* insertions */
  memcpy(varseq, seq, start);
  memcpy(varseq+start+1, seq+start, seqlen-start);
  for(int i=start; i<start+len; i++)
    {
      for(int v=1; v<5; v++)
	{
	  if((i==seqlen) || (v != seq[i]))
	    {
	      varseq[i] = v;
	      find_variant_matches(thread, varseq, seqlen+1, seed);
	    }
	}
      if (i<seqlen)
	varseq[i] = seq[i];
    }
}

void * worker(void * vp)
{
  long t = (long) vp;
  struct thread_info_s * tip = ti + t;

  pthread_mutex_lock(&tip->workmutex);

  /* loop until signalled to quit */
  while (tip->work >= 0)
    {
      /* wait for work available */
      if (tip->work == 0)
	pthread_cond_wait(&tip->workcond, &tip->workmutex);
      if (tip->work > 0)
	{
	  generate_variants(t, tip->seed, tip->mut_start, tip->mut_length);
	  tip->work = 0;
	  pthread_cond_signal(&tip->workcond);
	}
    }

  pthread_mutex_unlock(&tip->workmutex);
  return 0;
}

void process_seed(int seed)
{
  unsigned long seqlen = db_getsequencelen(seed);

  unsigned long thr = threads;
  if (thr > seqlen + 1)
    thr = seqlen+1;

  /* prepare work for the threads */
  unsigned long start = 0;
  for(unsigned long t=0; t<thr; t++)
    {
      struct thread_info_s * tip = ti + t;
      unsigned long length = (seqlen - start + thr - t) / (thr - t);
      tip->seed = seed;
      tip->mut_start = start;
      tip->mut_length = length;
      start += length;
      
      pthread_mutex_lock(&tip->workmutex);
      tip->work = 1;
      pthread_cond_signal(&tip->workcond);
      pthread_mutex_unlock(&tip->workmutex);
    }

  /* wait for theads to finish their work */
  for(int t=0; t<thr; t++)
    {
      struct thread_info_s * tip = ti + t;
      pthread_mutex_lock(&tip->workmutex);
      while (tip->work > 0)
	pthread_cond_wait(&tip->workcond, &tip->workmutex);
      pthread_mutex_unlock(&tip->workmutex);
    }
}

void threads_init()
{
  pthread_mutex_init(&mutex_varmatch, NULL);
  pthread_mutex_init(&mutex_stderr, NULL);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	
  /* allocate memory for thread info, incl the variant sequences */
  unsigned long longestamplicon = db_getlongestsequence();
  ti = (struct thread_info_s *) xmalloc(threads * sizeof(struct thread_info_s));
  
  /* init and create worker threads */
  for(int t=0; t<threads; t++)
    {
      struct thread_info_s * tip = ti + t;
      tip->varseq = (unsigned char*) xmalloc(longestamplicon+1);
      tip->work = 0;
      pthread_mutex_init(&tip->workmutex, NULL);
      pthread_cond_init(&tip->workcond, NULL);
      if (pthread_create(&tip->pthread, &attr, worker, (void*)(long)t))
	fatal("Cannot create thread");
    }
}

void threads_done()
{
  /* finish and clean up worker threads */
  for(int t=0; t<threads; t++)
    {
      struct thread_info_s * tip = ti + t;
      
      /* tell worker to quit */
      pthread_mutex_lock(&tip->workmutex);
      tip->work = -1;
      pthread_cond_signal(&tip->workcond);
      pthread_mutex_unlock(&tip->workmutex);

      /* wait for worker to quit */
      if (pthread_join(tip->pthread, NULL))
	fatal("Cannot join thread");

      pthread_cond_destroy(&tip->workcond);
      pthread_mutex_destroy(&tip->workmutex);
      free(tip->varseq);
    }

  free(ti);

  pthread_attr_destroy(&attr);
  pthread_mutex_destroy(&mutex_varmatch);
  pthread_mutex_destroy(&mutex_stderr);
}

void update_stats(int amp)
{
  /* update swarm stats */
  struct ampinfo_s * bp = ampinfo + amp;

  swarmsize++;
  if (bp->generation > swarm_maxgen)
    swarm_maxgen = bp->generation;
  unsigned long abundance = db_getabundance(amp);
  abundance_sum += abundance;
  if (abundance == 1)
    singletons++;
}

void algo_d1_run()
{
  unsigned long longestamplicon = db_getlongestsequence();
  unsigned long amplicons = db_getsequencecount();

  threads_init();

  ampinfo = (struct ampinfo_s *) xmalloc (amplicons * sizeof(struct ampinfo_s));

  /* compute hash for all amplicons and store them in a hash table */
  
  hash_alloc(amplicons);

  swarms_head = 0;
  swarms_tail = 0;

  progress_init("Hashing sequences:", amplicons);
  for(int i=0; i<amplicons; i++)
    {
      unsigned long seqlen = db_getsequencelen(i);
      unsigned char * seq = (unsigned char *) db_getsequence(i);
      struct ampinfo_s * bp = ampinfo + i;
      bp->generation = 0;
      bp->swarmid = 0;
      bp->swarms_next = -1;
      bp->swarm_next = -1;
      hash_insert(i, seq, seqlen);
      progress_update(i);
    }
  progress_done();

  unsigned char * dir = 0;
  unsigned long * hearray = 0;

  if (uclustfile)
    {
      dir = (unsigned char *) xmalloc(longestamplicon*longestamplicon);
      hearray = (unsigned long *) xmalloc(2 * longestamplicon * sizeof(unsigned long));
    }
  
  /* for each non-swarmed amplicon look for subseeds ... */
  unsigned long swarmid = 0;
  progress_init("Clustering:       ", amplicons);
  for(int seed = 0; seed < amplicons; seed++)
    {
      struct ampinfo_s * sp = ampinfo + seed;
      if (sp->swarmid == 0)
	{
	  /* start a new swarm with a new initial seed */
	  swarmid++;
	  swarmed++;
	  sp->swarmid = swarmid;
	  sp->generation = 0;
	  sp->swarm_next = -1;
	  sp->swarms_next = -1;

	  /* link up this initial seed in the list of swarms */
	  if (swarmid > 1)
	    ampinfo[swarms_tail].swarms_next = seed;
	  swarms_tail = seed;
	  current_swarm_tail = seed;
	  
	  /* initialize swarm stats */
	  swarmsize = 0;
	  swarm_maxgen = 0;
	  abundance_sum = 0;
	  singletons = 0;

	  update_stats(seed);
	  
	  /* find the first generation matches */
	  process_seed(seed);

	  /* find later generation matches */
	  int subseed = sp->swarm_next;
	  while(subseed >= 0)
	    {
	      process_seed(subseed);
	      subseed = ampinfo[subseed].swarm_next;
	    }

	  /* update statistics */
	  for (int a = ampinfo[seed].swarm_next; 
	       a >= 0;
	       a = ampinfo[a].swarm_next)
	    update_stats(a);

	  /* update overall statistics */
	  if (swarmsize > largest)
 	    largest = swarmsize;
	  if (swarm_maxgen > maxgen)
	    maxgen = swarm_maxgen;

	  /* output statistics to file */
	  if (statsfile)
	    {
	      fprintf(statsfile, "%lu\t%lu\t", swarmsize, abundance_sum);
	      fprint_id_noabundance(statsfile, seed);
	      fprintf(statsfile, "\t%lu\t%lu\t%lu\t%lu\n", 
		      db_getabundance(seed),
		      singletons, swarm_maxgen, swarm_maxgen);
	    }

	  /* output results for one swarm in native format */
	  if (!mothur)
	    {
	      for (int a = seed; 
		   a >= 0;
		   a = ampinfo[a].swarm_next)
		{
		  if (a != seed)
		    fputc(SEPCHAR, outfile);
		  fprint_id(outfile, a);
		}
	      fputc('\n', outfile);
	    }
      
	  /* output swarm in uclust format */
	  if (uclustfile)
	    {
	      fprintf(uclustfile, "C\t%u\t%lu\t*\t*\t*\t*\t*\t",
		      ampinfo[seed].swarmid - 1, 
		      swarmsize);
	      fprint_id(uclustfile, seed);
	      fprintf(uclustfile, "\t*\n");
          
	      fprintf(uclustfile, "S\t%u\t%lu\t*\t*\t*\t*\t*\t",
		      ampinfo[seed].swarmid-1,
		      db_getsequencelen(seed));
	      fprint_id(uclustfile, seed);
	      fprintf(uclustfile, "\t*\n");

	      for (int a = ampinfo[seed].swarm_next; 
		   a >= 0;
		   a = ampinfo[a].swarm_next)
		{
		  char * dseq = db_getsequence(a);
		  char * dend = dseq + db_getsequencelen(a);
		  char * qseq = db_getsequence(seed);
		  char * qend = qseq + db_getsequencelen(seed);

		  unsigned long nwscore = 0;
		  unsigned long nwdiff = 0;
		  char * nwalignment = NULL;
		  unsigned long nwalignmentlength = 0;

		  nw(dseq, dend, qseq, qend,
		     score_matrix_63, gapopen, gapextend,
		     & nwscore, & nwdiff, & nwalignmentlength, & nwalignment,
		     dir, hearray, 0, 0);
              
		  double percentid = 100.0 * (nwalignmentlength - nwdiff) /
		    nwalignmentlength;
              
		  fprintf(uclustfile,
			  "H\t%u\t%lu\t%.1f\t+\t0\t0\t%s\t",
			  ampinfo[seed].swarmid-1,
			  db_getsequencelen(a),
			  percentid, 
			  nwdiff > 0 ? nwalignment : "=");
              
		  fprint_id(uclustfile, a);
		  fprintf(uclustfile, "\t");
		  fprint_id(uclustfile, seed);
		  fprintf(uclustfile, "\n");
		  
		  if (nwalignment)
		    free(nwalignment);
		}
	    }
	}
      progress_update(swarmed);
    }
  progress_done();

  unsigned long swarmcount = swarmid;

  /* dump swarms in mothur format */
  /* cannot do it earlier because we need to know the number of swarms */
  if (mothur)
    {
      fprintf(outfile, "swarm_%ld\t%lu", resolution, swarmcount);

      for (int seed = 0;
	   seed >= 0;
	   seed = ampinfo[seed].swarms_next)
	
	for (int a = seed; 
	     a >= 0;
	     a = ampinfo[a].swarm_next)
	  {
	    if (a == seed)
	      fputc('\t', outfile);
	    else
	      fputc(',', outfile);
	    fprint_id(outfile, a);
	  }
      fputc('\n', outfile);
    }
  
  fprintf(stderr, "\n");
  fprintf(stderr, "Number of swarms:  %lu\n", swarmid);
  fprintf(stderr, "Largest swarm:     %lu\n", largest);
  fprintf(stderr, "Max generations:   %lu\n", maxgen);

  threads_done();

  hash_free();

  free(ampinfo);

  if (uclustfile)
    {
      free(dir);
      free(hearray);
    }

#ifdef HASHSTATS
  fprintf(stderr, "Tries: %ld\n", tries);
  fprintf(stderr, "Probes: %ld\n", probes);
  fprintf(stderr, "Hits: %ld\n", hits);
  fprintf(stderr, "Success: %ld\n", success);
  fprintf(stderr, "Bingo: %ld\n", bingo);
  fprintf(stderr, "Collisions: %ld\n", collisions);
#endif
}