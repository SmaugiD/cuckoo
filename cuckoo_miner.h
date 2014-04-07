// Cuckoo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2014 John Tromp

#include "cuckoo.h"
#ifdef __APPLE__
#include "osx_barrier.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <vector>
#include <atomic>
#include <set>

// ok for size up to 2^32
#define MAXPATHLEN 8192

#ifdef TRIMEDGES
#include "trim_edge_data.h"
#else

#ifndef PRESIP
#define PRESIP 1024
#endif
#define CUCKOOSET(key,val)	cuckoo[key] = val
#define CUCKOOHASH      node_t *

class graph_data {
public:
  node_t *cuckoo;

  graph_data(nonce_t easiness, int nthreads) {
    assert(cuckoo = (node_t *)calloc(1+SIZE, sizeof(node_t)));
  }
  ~graph_data() {
    free(cuckoo);
  }
};
#endif

class cuckoo_ctx {
public:
  siphash_ctx sip_ctx;
  nonce_t easiness;
  graph_data data;
  nonce_t (*sols)[PROOFSIZE];
  unsigned maxsols;
  std::atomic<unsigned> nsols;
  int nthreads;
  int ntrims;
  pthread_barrier_t barry;

  cuckoo_ctx(const char* header, nonce_t easy_ness, int n_threads, int n_trims, int max_sols)
  : data(easy_ness, n_threads) {
    setheader(&sip_ctx, header);
    easiness = easy_ness;
    nthreads = n_threads;
    ntrims = n_trims;
    assert(pthread_barrier_init(&barry, NULL, nthreads) == 0);
    assert(sols = (nonce_t (*)[PROOFSIZE])calloc(maxsols = max_sols, PROOFSIZE*sizeof(nonce_t)));
    nsols = 0;
  }
};

typedef struct {
  int id;
  pthread_t thread;
  cuckoo_ctx *ctx;
} thread_ctx;

#ifdef TRIMEDGES
void barrier(pthread_barrier_t *barry) {
  int rc = pthread_barrier_wait(barry);
  if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
    printf("Could not wait on barrier\n");
    pthread_exit(NULL);
  }
}

#define LOGNBUCKETS	12
#define NBUCKETS	(1 << LOGNBUCKETS)
#define BUCKETSHIFT	(SIZESHIFT-1 - LOGNBUCKETS)
#define BUCKETSIZE	16

void trim_edges(thread_ctx *tp) {
  cuckoo_ctx &ctx = *tp->ctx;
  graph_data &data = ctx.data;
  shrinkingset &alive = *data.alive;
  twice_set &nonleaf = *data.nonleaf;
  u32 bucketsizes[NBUCKETS];
  node_t buckets[NBUCKETS][BUCKETSIZE];
  node_t nonces[NBUCKETS][BUCKETSIZE];

  for (unsigned part = 0; part <= PART_MASK; part++) {
    for (int uorv = 0; uorv < 2; uorv++) {
      if (tp->id == 0)
        nonleaf.reset();
      barrier(&ctx.barry);
      for (int qkill = 0; qkill < 2; qkill++) {
        for (int b=0; b < NBUCKETS; b++)
          bucketsizes[b] = 0;
        for (nonce_t block = tp->id*32; block < ctx.easiness; block += ctx.nthreads*32) {
          u32 alive32 = alive.block(block); // GLOBAL 1 SEQ
          for (nonce_t nonce = block; alive32; alive32>>=1, nonce++) {
            assert(alive.test(nonce) == (alive32&1));
            if (alive.test(nonce)) {
              node_t u = sipnode(&ctx.sip_ctx, nonce, uorv);
              if ((u & PART_MASK) == part) {
                u32 b = u >> BUCKETSHIFT;
                u32 *bsize = &bucketsizes[b];
                buckets[b][*bsize] = u >> PART_BITS;
                nonces[b][*bsize] = nonce;
                if (++*bsize == BUCKETSIZE) {
                  *bsize = 0;
                  for (int i=0; i<BUCKETSIZE; i++) {
                    node_t ui = buckets[b][i];
                    if (!qkill) {
                      nonleaf.set(ui); // GLOBAL 1 RND BUCKETSIZE-1 SEQ 
                    } else {
                      if (!nonleaf.test(ui)) // GLOBAL 1 RND BUCKETSIZE-1 SEQ 
// nonces[b][i] can be recoved from its last 16 or so bits and nonce, since they differ by at most 2^16
                        alive.reset(nonces[b][i], tp->id); // GLOBAL SEQ 
                    }
                  }
                }
              }
            }
          }
        }
        for (int b=0; b < NBUCKETS; b++) {
          int ni = bucketsizes[b];
          for (int i=0; i<ni ; i++) {
            node_t ui = buckets[b][i];
            if (!qkill) {
              nonleaf.set(ui);
            } else {
              if (!nonleaf.test(ui))
                alive.reset(nonces[b][i], tp->id);
            }
          }
        }
        barrier(&ctx.barry);
      }
    }
  }
}
#endif

int path(CUCKOOHASH cuckoo, node_t u, node_t *us) {
  int nu;
  for (nu = 0; u; u = cuckoo[u]) {
    if (++nu >= MAXPATHLEN) {
      while (nu-- && us[nu] != u) ;
      if (nu < 0)
        printf("maximum path length exceeded\n");
      else printf("illegal % 4d-cycle\n", MAXPATHLEN-nu);
      pthread_exit(NULL);
    }
    us[nu] = u;
  }
  return nu;
}

typedef std::pair<node_t,node_t> edge;

void solution(cuckoo_ctx &ctx, node_t *us, int nu, node_t *vs, int nv) {
  std::set<edge> cycle;
  unsigned n;
  cycle.insert(edge(*us, *vs));
  while (nu--)
    cycle.insert(edge(us[(nu+1)&~1], us[nu|1])); // u's in even position; v's in odd
  while (nv--)
    cycle.insert(edge(vs[nv|1], vs[(nv+1)&~1])); // u's in odd position; v's in even
  unsigned soli = std::atomic_fetch_add_explicit(&ctx.nsols, 1U, std::memory_order_relaxed);
  for (nonce_t nonce = n = 0; nonce < ctx.easiness; nonce++)
#ifdef TRIMEDGES
    if (ctx.data.alive->test(nonce))
#endif
    {
      edge e(1+sipnode(&ctx.sip_ctx, nonce, 0), 1+HALFSIZE+sipnode(&ctx.sip_ctx, nonce, 1));
      if (cycle.find(e) != cycle.end()) {
        ctx.sols[soli][n++] = nonce;
        cycle.erase(e);
      }
    }
}

void *worker(void *vp) {
  thread_ctx *tp = (thread_ctx *)vp;
  cuckoo_ctx &ctx = *tp->ctx;
  graph_data &data = ctx.data;

#ifdef TRIMEDGES
  shrinkingset &alive = *data.alive;
  int load = 100 * ctx.easiness / CUCKOO_SIZE;
  if (tp->id == 0)
    printf("initial load %d%%\n", load);
  for (int round=1; round <= ctx.ntrims; round++) {
    trim_edges(tp);
    if (tp->id == 0) {
      load = (int)(100 * alive.count() / CUCKOO_SIZE);
      printf("%d trims: load %d%%\n", round, load);
    }
  }
  if (tp->id == 0) {
    if (load >= 90) {
      printf("overloaded! exiting...");
      exit(0);
    }
    delete data.nonleaf;
    data.nonleaf = 0;
    data.cuckoo = new cuckoo_hash();
  }
  barrier(&ctx.barry);
  cuckoo_hash &cuckoo = *data.cuckoo;
  node_t us[MAXPATHLEN], vs[MAXPATHLEN];
  for (nonce_t block = tp->id*32; block < ctx.easiness; block += ctx.nthreads*32) {
    for (nonce_t nonce = block; nonce < block+32 && nonce < ctx.easiness; nonce++) {
      if (alive.test(nonce)) {
        node_t u0, v0;
        sipedge(&ctx.sip_ctx, nonce, &u0, &v0);
#else
  node_t *cuckoo = data.cuckoo;
  node_t us[MAXPATHLEN], vs[MAXPATHLEN], uvpre[2*PRESIP], npre = 0;
  for (node_t nonce = tp->id; nonce < ctx.easiness; nonce += ctx.nthreads) {{{
    node_t u0, v0;
    if (!npre) {
      for (unsigned n = nonce; npre < PRESIP; npre++, n += ctx.nthreads)
        sipedge(&ctx.sip_ctx, n, &uvpre[2*npre], &uvpre[2*npre+1]);
    }
    unsigned i = PRESIP - npre--;
    u0 = uvpre[2*i];
    v0 = uvpre[2*i+1];
#endif
    u0 += 1        ;  // make non-zero
    v0 += 1 + HALFSIZE;  // make v's different from u's
    node_t u = cuckoo[u0], v = cuckoo[v0];
    if (u == v0 || v == u0)
      continue; // ignore duplicate edges
    us[0] = u0;
    vs[0] = v0;
    int nu = path(cuckoo, u, us), nv = path(cuckoo, v, vs);
    if (us[nu] == vs[nv]) {
      int min = nu < nv ? nu : nv;
      for (nu -= min, nv -= min; us[nu] != vs[nv]; nu++, nv++) ;
      int len = nu + nv + 1;
      printf("% 4d-cycle found at %d:%d%%\n", len, tp->id, (int)(nonce*100L/ctx.easiness));
      if (len == PROOFSIZE && ctx.nsols < ctx.maxsols)
        solution(ctx, us, nu, vs, nv);
      continue;
    }
    if (nu < nv) {
      while (nu--)
        CUCKOOSET(us[nu+1], us[nu]);
      CUCKOOSET(u0, v0);
    } else {
      while (nv--)
        CUCKOOSET(vs[nv+1], vs[nv]);
      CUCKOOSET(v0, u0);
    }
  }}}
  pthread_exit(NULL);
}
