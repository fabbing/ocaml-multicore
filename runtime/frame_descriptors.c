/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                   Tom Kelly, OCaml Labs Consultancy                    */
/*                 Stephen Dolan, University of Cambridge                 */
/*                                                                        */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2021 OCaml Labs Consultancy Ltd                            */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include "caml/platform.h"
#include "caml/frame_descriptors.h"
#include "caml/major_gc.h" /* for caml_major_cycles_completed */
#include "caml/memory.h"
#include <stddef.h>

/* Defined in code generated by ocamlopt */
extern intnat * caml_frametable[];

typedef struct link {
  intnat* frametable;
  struct link *next;
} link;

#define iter_list(list,lnk) \
  for (lnk = list; lnk != NULL; lnk = lnk->next)

static frame_descr * next_frame_descr(frame_descr * d) {
  unsigned char num_allocs = 0, *p;
  CAMLassert(d->retaddr >= 4096);
  /* Skip to end of live_ofs */
  p = (unsigned char*)&d->live_ofs[d->num_live];
  /* Skip alloc_lengths if present */
  if (d->frame_size & 2) {
    num_allocs = *p;
    p += num_allocs + 1;
  }
  /* Skip debug info if present */
  if (d->frame_size & 1 &&
      d->frame_size != (unsigned short)-1) {
    /* Align to 32 bits */
    p = Align_to(p, uint32_t);
    p += sizeof(uint32_t) * (d->frame_size & 2 ? num_allocs : 1);
  }
  /* Align to word size */
  p = Align_to(p, void*);
  return ((frame_descr*) p);
}

static caml_frame_descrs build_frame_descriptors(link* frametables)
{
  intnat num_descr, tblsize, i, j, len;
  intnat * tbl;
  frame_descr * d;
  uintnat h;
  link *lnk;
  caml_frame_descrs table;

  /* Count the frame descriptors */
  num_descr = 0;
  iter_list(frametables,lnk) {
    num_descr += *lnk->frametable;
  }

  /* The size of the hashtable is a power of 2 greater or equal to
     2 times the number of descriptors */
  tblsize = 4;
  while (tblsize < 2 * num_descr) tblsize *= 2;

  /* Allocate the hash table */
  table.descriptors = caml_stat_alloc(tblsize * sizeof(frame_descr*));
  table.mask = tblsize - 1;
  for (i = 0; i < tblsize; i++) table.descriptors[i] = NULL;

  /* Fill the hash table */
  iter_list(frametables,lnk) {
    tbl = lnk->frametable;
    len = *tbl;
    d = (frame_descr *)(tbl + 1);
    for (j = 0; j < len; j++) {
      h = Hash_retaddr(d->retaddr, tblsize - 1);
      while (table.descriptors[h] != NULL) {
        h = (h+1) & table.mask;
      }
      table.descriptors[h] = d;
      d = next_frame_descr(d);
    }
  }
  return table;
}

static caml_plat_mutex descr_mutex;
static link* frametables;

/* Memory used by frametables is only freed once a GC cycle has
   completed, because other threads access the frametable at
   unpredictable times. */
struct frametable_version {
  caml_frame_descrs table;

  /* after this cycle has completed,
     the previous table should be deallocated.
     Set to No_need_to_free after prev is freed */
  atomic_uintnat free_prev_after_cycle;
  struct frametable_version* prev;
};
#define No_need_to_free ((uintnat)(-1))

/* Only modified when holding descr_mutex, but read without locking */
static atomic_uintnat current_frametable = ATOMIC_UINTNAT_INIT(0);

static link *cons(intnat *frametable, link *tl) {
  link *lnk = caml_stat_alloc(sizeof(link));
  lnk->frametable = frametable;
  lnk->next = tl;
  return lnk;
}

void caml_init_frame_descriptors(void)
{
  int i;
  struct frametable_version *ft;

  caml_plat_mutex_init(&descr_mutex);

  caml_plat_lock(&descr_mutex);
  for (i = 0; caml_frametable[i] != 0; i++)
    frametables = cons(caml_frametable[i], frametables);

  ft = caml_stat_alloc(sizeof(*ft));
  ft->table = build_frame_descriptors(frametables);
  atomic_store_rel(&ft->free_prev_after_cycle, No_need_to_free);
  ft->prev = 0;
  atomic_store_rel(&current_frametable, (uintnat)ft);
  caml_plat_unlock(&descr_mutex);
}

void caml_register_frametable(intnat *table)
{
  struct frametable_version *ft, *old;

  caml_plat_lock(&descr_mutex);

  frametables = cons(table, frametables);
  old = (struct frametable_version*)atomic_load_acq(&current_frametable);
  CAMLassert(old != NULL);
  ft = caml_stat_alloc(sizeof(*ft));
  ft->table = build_frame_descriptors(frametables);
  atomic_store_rel(&ft->free_prev_after_cycle, caml_major_cycles_completed);
  ft->prev = old;
  atomic_store_rel(&current_frametable, (uintnat)ft);

  caml_plat_unlock(&descr_mutex);
}

caml_frame_descrs caml_get_frame_descrs()
{
  struct frametable_version *ft =
    (struct frametable_version*)atomic_load_acq(&current_frametable);
  CAMLassert(ft);
  if (atomic_load_acq(&ft->free_prev_after_cycle) < caml_major_cycles_completed)
  {
    /* it's now safe to free the old table */
    caml_plat_lock(&descr_mutex);
    if (ft->prev != NULL) {
      caml_stat_free(ft->prev->table.descriptors);
      caml_stat_free(ft->prev);
      ft->prev = NULL;
      atomic_store_rel(&ft->free_prev_after_cycle, No_need_to_free);
    }
    caml_plat_unlock(&descr_mutex);
  }
  return ft->table;
}

frame_descr* caml_find_frame_descr(caml_frame_descrs fds, uintnat pc)
{
  frame_descr * d;
  uintnat h;

  h = Hash_retaddr(pc, fds.mask);
  while (1) {
    d = fds.descriptors[h];
    if (d == 0) return NULL; /* can happen if some code compiled without -g */
    if (d->retaddr == pc) break;
    h = (h+1) & fds.mask;
  }
  return d;
}
