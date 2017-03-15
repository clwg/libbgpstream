/*
 * This file is part of bgpstream
 *
 * CAIDA, UC San Diego
 * bgpstream-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 * Authors: Alistair King, Chiara Orsini
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "bgpstream_log.h"
#include "bgpstream_reader_mgr.h"
#include "bgpdump/bgpdump_lib.h"
#include "utils.h"

#define BUFFER_LEN 1024

#define DUMP_OPEN_MAX_RETRIES 5
#define DUMP_OPEN_MIN_RETRY_WAIT 10

// TODO: clean up

typedef enum {
  BGPSTREAM_READER_STATUS_VALID_ENTRY,
  BGPSTREAM_READER_STATUS_FILTERED_DUMP,
  BGPSTREAM_READER_STATUS_EMPTY_DUMP,
  BGPSTREAM_READER_STATUS_CANT_OPEN_DUMP,
  BGPSTREAM_READER_STATUS_CORRUPTED_DUMP,
  BGPSTREAM_READER_STATUS_END_OF_DUMP
} bgpstream_reader_status_t;

typedef struct struct_bgpstream_reader_t bgpstream_reader_t;

typedef enum {
  BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR,
  BGPSTREAM_READER_MGR_STATUS_NON_EMPTY_READER_MGR
} bgpstream_reader_mgr_status_t;

struct bgpstream_reader_mgr {
  bgpstream_reader_t *reader_queue;
  bgpstream_filter_mgr_t *filter_mgr;
  bgpstream_reader_mgr_status_t status;
};


struct struct_bgpstream_reader_t {
  struct struct_bgpstream_reader_t *next;
  char dump_name[BGPSTREAM_DUMP_MAX_LEN];     // name of bgp dump
  char dump_project[BGPSTREAM_PAR_MAX_LEN];   // name of bgp project
  char dump_collector[BGPSTREAM_PAR_MAX_LEN]; // name of bgp collector
  bgpstream_record_dump_type_t dump_type;
  long dump_time;   // timestamp associated with the time the bgp data was
                    // aggregated
  long record_time; // timestamp associated with the current bd_entry
  BGPDUMP_ENTRY *bd_entry;
  int successful_read; // n. successful reads, i.e. entry != NULL
  int valid_read;      // n. reads successful and compatible with filters
  bgpstream_reader_status_t status;

  BGPDUMP *bd_mgr;
  /** The thread that opens the bgpdump */
  pthread_t producer;
  /* has the thread opened the dump? */
  int dump_ready;
  pthread_cond_t dump_ready_cond;
  pthread_mutex_t mutex;
  /* have we already checked that the dump is ready? */
  int skip_dump_check;
};

static void *thread_producer(void *user)
{
  bgpstream_reader_t *bsr = (bgpstream_reader_t *)user;
  int retries = 0;
  int delay = DUMP_OPEN_MIN_RETRY_WAIT;

  /* all we do is open the dump */
  /* but try a few times in case there is a transient failure */
  while (retries < DUMP_OPEN_MAX_RETRIES && bsr->bd_mgr == NULL) {
    if ((bsr->bd_mgr = bgpdump_open_dump(bsr->dump_name)) == NULL) {
      fprintf(stderr, "WARN: Could not open dumpfile (%s). Attempt %d of %d\n",
              bsr->dump_name, retries + 1, DUMP_OPEN_MAX_RETRIES);
      retries++;
      if (retries < DUMP_OPEN_MAX_RETRIES) {
        sleep(delay);
        delay *= 2;
      }
    }
  }

  pthread_mutex_lock(&bsr->mutex);
  if (bsr->bd_mgr == NULL) {
    fprintf(
      stderr,
      "ERROR: Could not open dumpfile (%s) after %d attempts. Giving up.\n",
      bsr->dump_name, DUMP_OPEN_MAX_RETRIES);
    bsr->status = BGPSTREAM_READER_STATUS_CANT_OPEN_DUMP;
  }
  bsr->dump_ready = 1;
  pthread_cond_signal(&bsr->dump_ready_cond);
  pthread_mutex_unlock(&bsr->mutex);

  return NULL;
}

static BGPDUMP_ENTRY *get_next_entry(bgpstream_reader_t *bsr)
{
  if (bsr->skip_dump_check == 0) {
    pthread_mutex_lock(&bsr->mutex);
    while (bsr->dump_ready == 0) {
      pthread_cond_wait(&bsr->dump_ready_cond, &bsr->mutex);
    }
    pthread_mutex_unlock(&bsr->mutex);

    if (bsr->status == BGPSTREAM_READER_STATUS_CANT_OPEN_DUMP) {
      return NULL;
    }

    bsr->skip_dump_check = 1;
  }

  /* now, grab an entry from bgpdump */
  return bgpdump_read_next(bsr->bd_mgr);
}

/* -------------- Reader functions -------------- */

static bool
bgpstream_reader_filter_bd_entry(BGPDUMP_ENTRY *bd_entry,
                                 bgpstream_filter_mgr_t *filter_mgr)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: filter entry: start");
  bgpstream_interval_filter_t *tif;
  if (bd_entry != NULL && filter_mgr != NULL) {
    if (filter_mgr->time_intervals == NULL) {
      bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: filter entry: end");
      return true; // no time filtering,
    }
    int current_entry_time = bd_entry->time;
    tif = filter_mgr->time_intervals;
    while (tif != NULL) {
      if (current_entry_time >= tif->begin_time &&
          (tif->end_time == BGPSTREAM_FOREVER ||
           current_entry_time <= tif->end_time)) {
        bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: filter entry: OK");
        return true;
      }
      tif = tif->next;
    }
  }
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: filter entry: DISCARDED");
  return false;
}

static void
bgpstream_reader_read_new_data(bgpstream_reader_t *bs_reader,
                               bgpstream_filter_mgr_t *filter_mgr)
{
  bool significant_entry = false;
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: start");
  if (bs_reader == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: invalid reader provided");
    return;
  }
  if (bs_reader->status != BGPSTREAM_READER_STATUS_VALID_ENTRY) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: reader cannot read new data "
                    "(previous read was not successful)");
    return;
  }
  // if a valid record was read before (or it is the first time we read
  // something)
  // assert(bs_reader->status == BGPSTREAM_READER_STATUS_VALID_ENTRY)
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: bd_entry has to be set to NULL");
  // entry should not be destroyed, otherwise we could destroy
  // what is in the current record, the next export record will take
  // care of it
  bs_reader->bd_entry = NULL;
  // check if the end of the file was already reached before
  // reading a new value
  // if(bs_reader->bd_mgr->eof != 0) {
  //  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: end of file reached");
  //  bs_reader->status = BGPSTREAM_READER_STATUS_END_OF_DUMP;
  //  return;
  //}
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data (previous): %ld\t%ld\t%d\t%s\t%d",
                  bs_reader->record_time, bs_reader->dump_time,
                  bs_reader->dump_type, bs_reader->dump_collector,
                  bs_reader->status);
  bgpstream_log(BGPSTREAM_LOG_VFINE, 
    "\t\tBSR: read new data: reading new entry (or entries) in bgpdump");
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: from %s", bs_reader->dump_name);
  while (!significant_entry) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\t\tBSR: read new data: reading");
    // try and get the next entry
    // will block until dump is open (or fails)
    bs_reader->bd_entry = get_next_entry(bs_reader);
    // check if there was an error opening the dump
    if (bs_reader->status == BGPSTREAM_READER_STATUS_CANT_OPEN_DUMP) {
      return;
    }
    // check if an entry has been read
    if (bs_reader->bd_entry != NULL) {
      bs_reader->successful_read++;
      // check if entry is compatible with filters
      if (bgpstream_reader_filter_bd_entry(bs_reader->bd_entry, filter_mgr)) {
        // update reader fields
        bs_reader->valid_read++;
        bs_reader->status = BGPSTREAM_READER_STATUS_VALID_ENTRY;
        bs_reader->record_time = (long)bs_reader->bd_entry->time;
        significant_entry = true;
      }
      // if not compatible, destroy bgpdump entry
      else {
        bgpdump_free_mem(bs_reader->bd_entry);
        bs_reader->bd_entry = NULL;
        // significant_entry = false;
      }
    }
    // if an entry has not been read (i.e. b_e = NULL)
    else {
      // if the corrupted entry flag is
      // active, then dump is corrupted
      if (bs_reader->bd_mgr->corrupted_read) {
        bs_reader->status = BGPSTREAM_READER_STATUS_CORRUPTED_DUMP;
        significant_entry = true;
      }
      // empty read, not corrupted
      else {
        // end of file
        if (bs_reader->bd_mgr->eof != 0) {
          significant_entry = true;
          if (bs_reader->successful_read == 0) {
            // file was empty
            bs_reader->status = BGPSTREAM_READER_STATUS_EMPTY_DUMP;
          } else {
            if (bs_reader->valid_read == 0) {
              // valid contained entries, but
              // none of them was compatible with
              // filters
              bs_reader->status = BGPSTREAM_READER_STATUS_FILTERED_DUMP;
            } else {
              // something valid has been read before
              // reached the end of file
              bs_reader->status = BGPSTREAM_READER_STATUS_END_OF_DUMP;
            }
          }
        }
        // empty space at the *beginning* of file
        else {
          // do nothing - continue to read
          // significant_entry = false;
        }
      }
    }
  }
  // a significant entry has been found
  // and the reader has been updated accordingly
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: read new data: end");
  return;
}

static void bgpstream_reader_destroy(bgpstream_reader_t *bs_reader)
{

  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: destroy reader start");
  if (bs_reader == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: destroy reader: null reader provided");
    return;
  }

  /* Ensure the thread is done */
  pthread_join(bs_reader->producer, NULL);
  pthread_mutex_destroy(&bs_reader->mutex);
  pthread_cond_destroy(&bs_reader->dump_ready_cond);

  // we do not deallocate memory for bd_entry
  // (the last entry may be still in use in
  // the current record)
  bs_reader->bd_entry = NULL;
  // close bgpdump
  bgpdump_close_dump(bs_reader->bd_mgr);
  bs_reader->bd_mgr = NULL;
  // deallocate all memory for reader
  free(bs_reader);
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: destroy reader end");
}

static bgpstream_reader_t *
bgpstream_reader_create(bgpstream_resource_t *resource,
                        bgpstream_filter_mgr_t *filter_mgr)
{
  assert(resource);
  // allocate memory for reader
  bgpstream_reader_t *bs_reader =
    (bgpstream_reader_t *)malloc(sizeof(bgpstream_reader_t));
  if (bs_reader == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: create reader: can't allocate memory for reader");
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: create reader end");
    return NULL; // can't allocate memory for reader
  }
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: create reader: initialize fields");
  // fields initialization
  bs_reader->next = NULL;
  bs_reader->bd_mgr = NULL;
  bs_reader->bd_entry = NULL;
  // init done
  strcpy(bs_reader->dump_name, resource->uri);
  strcpy(bs_reader->dump_project, resource->project);
  strcpy(bs_reader->dump_collector, resource->collector);
  bs_reader->dump_type = resource->record_type;
  bs_reader->dump_time = bs_reader->record_time = resource->initial_time;
  bs_reader->status = BGPSTREAM_READER_STATUS_VALID_ENTRY;
  bs_reader->valid_read = 0;
  bs_reader->successful_read = 0;

  pthread_mutex_init(&bs_reader->mutex, NULL);
  pthread_cond_init(&bs_reader->dump_ready_cond, NULL);
  bs_reader->dump_ready = 0;
  bs_reader->skip_dump_check = 0;

  // bgpdump is created in the thread
  pthread_create(&bs_reader->producer, NULL, thread_producer, bs_reader);

  /* // call bgpstream_reader_read_new_data */
  /* bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: create reader: read new data"); */
  /* bgpstream_reader_read_new_data(bs_reader, filter_mgr); */
  /* bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: create reader: end");   */
  // return reader
  return bs_reader;
}

static void
bgpstream_reader_export_record(bgpstream_reader_t *bs_reader,
                               bgpstream_record_t *bs_record,
                               bgpstream_filter_mgr_t *filter_mgr)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: start");
  if (bs_reader == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: invalid reader provided");
    return;
  }
  if (bs_record == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: invalid record provided");
    return;
  }
  // if bs_reader status is BGPSTREAM_READER_STATUS_END_OF_DUMP we shouldn't
  // have called this
  // function
  if (bs_reader->status == BGPSTREAM_READER_STATUS_END_OF_DUMP) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: end of dump was reached");
    return;
  }
  // read bgpstream_reader field and copy them to a bs_record
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: copying bd_entry");
  bs_record->bd_entry = bs_reader->bd_entry;
  // disconnect reader from exported entry
  bs_reader->bd_entry = NULL;
  // memset(bs_record->attributes.dump_project, 0, BGPSTREAM_PAR_MAX_LEN);
  // memset(bs_record->attributes.dump_collector, 0, BGPSTREAM_PAR_MAX_LEN);
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: copying attributes");
  strcpy(bs_record->attributes.dump_project, bs_reader->dump_project);
  strcpy(bs_record->attributes.dump_collector, bs_reader->dump_collector);
  //   strcpy(bs_record->attributes.dump_type, bs_reader->dump_type);
  bs_record->attributes.dump_type = bs_reader->dump_type;
  bs_record->attributes.dump_time = bs_reader->dump_time;
  bs_record->attributes.record_time = bs_reader->record_time;
  // if this is the first significant record and no previous
  // valid record has been discarded because of time
  if (bs_reader->valid_read == 1 && bs_reader->successful_read == 1) {
    bs_record->dump_pos = BGPSTREAM_DUMP_START;
  } else {
    bs_record->dump_pos = BGPSTREAM_DUMP_MIDDLE;
  }
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: copying status");
  switch (bs_reader->status) {
  case BGPSTREAM_READER_STATUS_VALID_ENTRY:
    bs_record->status = BGPSTREAM_RECORD_STATUS_VALID_RECORD;
    break;
  case BGPSTREAM_READER_STATUS_FILTERED_DUMP:
    bs_record->status = BGPSTREAM_RECORD_STATUS_FILTERED_SOURCE;
    break;
  case BGPSTREAM_READER_STATUS_EMPTY_DUMP:
    bs_record->status = BGPSTREAM_RECORD_STATUS_EMPTY_SOURCE;
    break;
  case BGPSTREAM_READER_STATUS_CANT_OPEN_DUMP:
    bs_record->status = BGPSTREAM_RECORD_STATUS_CORRUPTED_SOURCE;
    break;
  case BGPSTREAM_READER_STATUS_CORRUPTED_DUMP:
    bs_record->status = BGPSTREAM_RECORD_STATUS_CORRUPTED_RECORD;
    break;
  default:
    bs_record->status = BGPSTREAM_RECORD_STATUS_EMPTY_SOURCE;
  }
  bgpstream_log(BGPSTREAM_LOG_VFINE, 
    "Exported: %ld\t%ld\t%d\t%s\t%d", bs_record->attributes.record_time,
    bs_record->attributes.dump_time, bs_record->attributes.dump_type,
    bs_record->attributes.dump_collector, bs_record->status);

  /** safe option for rib period filter*/
  char buffer[BUFFER_LEN];
  khiter_t k;
  int khret;
  if (filter_mgr->rib_period != 0 &&
      (bs_record->status == BGPSTREAM_RECORD_STATUS_CORRUPTED_SOURCE ||
       bs_record->status == BGPSTREAM_RECORD_STATUS_CORRUPTED_RECORD)) {
    snprintf(buffer, BUFFER_LEN, "%s.%s", bs_record->attributes.dump_project,
             bs_record->attributes.dump_collector);
    if ((k = kh_get(collector_ts, filter_mgr->last_processed_ts, buffer)) ==
        kh_end(filter_mgr->last_processed_ts)) {
      k = kh_put(collector_ts, filter_mgr->last_processed_ts, strdup(buffer),
                 &khret);
    }
    kh_value(filter_mgr->last_processed_ts, k) = 0;
  }

  bgpstream_log(BGPSTREAM_LOG_VFINE, "\t\tBSR: export record: end");
}

// function used for debug
static void print_reader_queue(bgpstream_reader_t *reader_queue)
{
#ifdef NDEBUG
  const bgpstream_reader_t *iterator = reader_queue;
  bgpstream_log(BGPSTREAM_LOG_VFINE, "READER QUEUE: start");
  int i = 1;
  while (iterator != NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\t%d %s %s %ld %ld %d", i, iterator->dump_collector,
                    iterator->dump_type, iterator->dump_time,
                    iterator->record_time, iterator->status);
    iterator = iterator->next;
    i++;
  }
  iterator = NULL;
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\nREADER QUEUE: end");
#endif
}

/* -------------- Reader mgr functions -------------- */

bgpstream_reader_mgr_t *
bgpstream_reader_mgr_create(bgpstream_filter_mgr_t *filter_mgr)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: create reader mgr: start");
  // allocate memory and initialize fields
  bgpstream_reader_mgr_t *bs_reader_mgr =
    (bgpstream_reader_mgr_t *)malloc(sizeof(bgpstream_reader_mgr_t));
  if (bs_reader_mgr == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, 
      "\tBSR_MGR: create reader mgr: can't allocate memory for reader mgr");
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: create reader mgr end");
    return NULL; // can't allocate memory for reader
  }
  // mgr initialization
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: create reader mgr: initialization");
  bs_reader_mgr->reader_queue = NULL;
  bs_reader_mgr->filter_mgr = filter_mgr;
  bs_reader_mgr->status = BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR;
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: create reader mgr: end");
  return bs_reader_mgr;
}

int bgpstream_reader_mgr_is_empty(bgpstream_reader_mgr_t *bs_reader_mgr)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: is_empty start");
  if (bs_reader_mgr == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: is_empty end: empty!");
    return true;
  }
  if (bs_reader_mgr->status == BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: is_empty end: empty!");
    return true;
  } else {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: is_empty end: non-empty!");
    return false;
  }
}

static void
bgpstream_reader_mgr_sorted_insert(bgpstream_reader_mgr_t *bs_reader_mgr,
                                   bgpstream_reader_t *bs_reader)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: sorted insert:start");
  if (bs_reader_mgr == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: sorted insert: null reader mgr provided");
    return;
  }
  if (bs_reader == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: sorted insert: null reader provided");
    return;
  }

  bgpstream_reader_t *iterator = bs_reader_mgr->reader_queue;
  bgpstream_reader_t *previous_iterator = bs_reader_mgr->reader_queue;
  bool inserted = false;
  // insert new reader in priority queue
  while (!inserted) {
    // case 1: insertion in empty queue (reader_queue == NULL)
    if (bs_reader_mgr->status == BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR) {
      bs_reader_mgr->reader_queue = bs_reader;
      bs_reader->next = NULL;
      bs_reader_mgr->status = BGPSTREAM_READER_MGR_STATUS_NON_EMPTY_READER_MGR;
      inserted = true;
      continue;
    }
    // case 2: insertion in non-empty queue
    else {
      // reached the end of the queue
      if (iterator == NULL) {
        previous_iterator->next = bs_reader;
        bs_reader->next = NULL;
        inserted = true;
        continue;
      }
      // still in the middle of the queue
      else {
        // if time is the same
        if (bs_reader->record_time == iterator->record_time) {
          // if type is the same -> continue
          if (iterator->dump_type == bs_reader->dump_type) {
            previous_iterator = iterator;
            iterator = previous_iterator->next;
            continue;
          }
          // if the queue contains ribs, and the current reader
          // is an update -> continue
          if (iterator->dump_type == BGPSTREAM_RIB &&
              bs_reader->dump_type == BGPSTREAM_UPDATE) {
            previous_iterator = iterator;
            iterator = previous_iterator->next;
            continue;
          }
          // if the queue contains updates, and the current reader
          // is a rib -> insert
          if (iterator->dump_type == BGPSTREAM_UPDATE &&
              bs_reader->dump_type == BGPSTREAM_RIB) {
            // insertion at the beginning of the queue
            if (previous_iterator == bs_reader_mgr->reader_queue &&
                iterator == bs_reader_mgr->reader_queue) {
              bs_reader->next = bs_reader_mgr->reader_queue;
              bs_reader_mgr->reader_queue = bs_reader;
            }
            // insertion in the middle of the queue
            else {
              previous_iterator->next = bs_reader;
              bs_reader->next = iterator;
            }
            inserted = true;
            continue;
          }
        }
        // if time is different
        else {
          // if reader time is lower than iterator time
          // then insert
          if (bs_reader->record_time < iterator->record_time) {
            // insertion at the beginning of the queue
            if (previous_iterator == bs_reader_mgr->reader_queue &&
                iterator == bs_reader_mgr->reader_queue) {
              bs_reader->next = bs_reader_mgr->reader_queue;
              bs_reader_mgr->reader_queue = bs_reader;
            }
            // insertion in the middle of the queue
            else {
              previous_iterator->next = bs_reader;
              bs_reader->next = iterator;
            }
            inserted = true;
            continue;
          }
        }
        // otherwise update the iterators
        previous_iterator = iterator;
        iterator = previous_iterator->next;
      }
    }
  }
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: sorted insert: end");
}

static int
bgpstream_reader_period_check(bgpstream_resource_t *res,
                              bgpstream_filter_mgr_t *filter_mgr)
{
  /* consider making this buffer static at the beginning of the file */
  char buffer[BUFFER_LEN];
  khiter_t k;
  int khret;

  if (filter_mgr->rib_period != 0 && res->record_type == BGPSTREAM_RIB) {
    snprintf(buffer, BUFFER_LEN, "%s.%s", res->project, res->collector);
    /* first instance */
    if ((k = kh_get(collector_ts, filter_mgr->last_processed_ts, buffer)) ==
        kh_end(filter_mgr->last_processed_ts)) {
      k = kh_put(collector_ts, filter_mgr->last_processed_ts, strdup(buffer),
                 &khret);
      kh_value(filter_mgr->last_processed_ts, k) = res->initial_time;
      return 1;
    }

    /* we have to check the frequency only if we get a new filetime */
    if (res->initial_time != kh_value(filter_mgr->last_processed_ts, k)) {
      if (res->initial_time <
          kh_value(filter_mgr->last_processed_ts, k) + filter_mgr->rib_period) {
        return 0;
      }
      kh_value(filter_mgr->last_processed_ts, k) = res->initial_time;
    }
  }

  return 1;
}

int bgpstream_reader_mgr_add(bgpstream_reader_mgr_t *bs_reader_mgr,
                             bgpstream_resource_t **res_batch,
                             int res_batch_cnt)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: add input: start");
  bgpstream_reader_t *bs_reader = NULL;

  /* tmp structure to hold reader pointers */
  bgpstream_reader_t **tmp_reader_queue = NULL;
  int i = 0;
  int max_readers = res_batch_cnt;
  int max = 0;
  tmp_reader_queue =
    (bgpstream_reader_t **)malloc(sizeof(bgpstream_reader_t *) * max_readers);

  /* foreach resource, add it to the queue and create a reader */
  int r;
  for (r=0; r<res_batch_cnt; r++) {
    if (bgpstream_reader_period_check(res_batch[r], bs_reader_mgr->filter_mgr)) {
      bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: add input: i");
      // a) create a new reader (create includes the first read)
      bs_reader = bgpstream_reader_create(res_batch[r], bs_reader_mgr->filter_mgr);
      // if it creates correctly then add it to the temporary queue
      if (bs_reader != NULL) {
        tmp_reader_queue[i] = bs_reader;
        i++;
      } else {
        bgpstream_log(BGPSTREAM_LOG_ERR, "ERROR: could not create reader\n");
        return -1;
      }
    }
  }

  /* TODO: use the resources internally */
  bgpstream_resource_destroy_batch(res_batch, res_batch_cnt, 1);

  /* then for each reader read the first record and add it to the reader queue
   */
  max = i;
  for (i = 0; i < max; i++) {
    bs_reader = tmp_reader_queue[i];
    bgpstream_reader_read_new_data(bs_reader, bs_reader_mgr->filter_mgr);
    bgpstream_reader_mgr_sorted_insert(bs_reader_mgr, bs_reader);
  }
  free(tmp_reader_queue);
  tmp_reader_queue = NULL;
  print_reader_queue(bs_reader_mgr->reader_queue);
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: add input: end");

  return 0;
}

static bgpstream_reader_t *
bs_reader_mgr_pop_head(bgpstream_reader_mgr_t *bs_reader_mgr)
{
  bgpstream_reader_t *bs_reader = bs_reader_mgr->reader_queue;
  // disconnect reader from the queue
  bs_reader_mgr->reader_queue = bs_reader_mgr->reader_queue->next;
  if (bs_reader_mgr->reader_queue == NULL) { // check if last reader
    bs_reader_mgr->status = BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR;
  }
  bs_reader->next = NULL;
  return bs_reader;
}

int bgpstream_reader_mgr_get_next_record(
  bgpstream_reader_mgr_t *bs_reader_mgr,
  bgpstream_record_t *bs_record)
{

  long previous_record_time = 0;

  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: get_next_record: start");
  if (bs_reader_mgr == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: get_next_record: null reader mgr provided");
    return -1;
  }
  if (bs_record == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "BSR_MGR: get_next_record: null record provided");
    return -1;
  }
  if (bs_reader_mgr->status == BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: get_next_record: empty reader mgr");
    return 0;
  }
  // get head from reader queue (without disconnecting it)
  bgpstream_reader_t *bs_reader = bs_reader_mgr->reader_queue;

  // bgpstream_reader_export
  bgpstream_reader_export_record(bs_reader, bs_record, bs_reader_mgr->filter_mgr);
  // we save the difference between successful read
  // and valid read, so we can check if some valid data
  // have been discarded during the last read_new_data

  int read_diff = bs_reader->successful_read - bs_reader->valid_read;
  // if previous read was successful, we read next
  // entry from same reader

  if (bs_reader->status == BGPSTREAM_READER_STATUS_VALID_ENTRY) {

    previous_record_time = bs_record->attributes.record_time;
    bgpstream_reader_read_new_data(bs_reader, bs_reader_mgr->filter_mgr);

    // if end of dump is reached after a successful read (already exported)
    // we destroy the reader
    if (bs_reader->status == BGPSTREAM_READER_STATUS_END_OF_DUMP) {

      if ((bs_reader->successful_read - bs_reader->valid_read) == read_diff) {
        bs_record->dump_pos = BGPSTREAM_DUMP_END;
      }
      // otherwise we maintain the dump_pos already assigned
      bs_reader_mgr_pop_head(bs_reader_mgr);
      bgpstream_reader_destroy(bs_reader);
    }
    // otherwise we insert the reader in the queue again
    else {
      if (bs_reader->record_time != previous_record_time) {
        bs_reader_mgr_pop_head(bs_reader_mgr);
        bgpstream_reader_mgr_sorted_insert(bs_reader_mgr, bs_reader);
      }
      // if the time is the same we do not disconnect the
      // reader, we just leave it where it is (head)
    }
  }
  // otherwise we destroy the reader
  else {
    bs_record->dump_pos = BGPSTREAM_DUMP_END;
    bs_reader_mgr_pop_head(bs_reader_mgr);
    bgpstream_reader_destroy(bs_reader);
  }

  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: get_next_record: end");
  return 1;
}

void bgpstream_reader_mgr_destroy(bgpstream_reader_mgr_t *bs_reader_mgr)
{
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: destroy reader mgr: start");
  if (bs_reader_mgr == NULL) {
    bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: destroy reader mgr:  null reader mgr provided");
    return;
  }
  bs_reader_mgr->filter_mgr = NULL;
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: destroy reader mgr:  destroying reader queue");
  // foreach reader in queue: destroy reader
  bgpstream_reader_t *iterator;
  while (bs_reader_mgr->reader_queue != NULL) {
    // at every step we destroy the reader referenced
    // by the reader_queue
    iterator = bs_reader_mgr->reader_queue;
    bs_reader_mgr->reader_queue = iterator->next;
    bgpstream_reader_destroy(iterator);
  }
  bs_reader_mgr->status = BGPSTREAM_READER_MGR_STATUS_EMPTY_READER_MGR;
  free(bs_reader_mgr);
  bgpstream_log(BGPSTREAM_LOG_VFINE, "\tBSR_MGR: destroy reader mgr: end");
}