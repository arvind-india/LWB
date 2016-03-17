/*
 * Copyright (c) 2016, Swiss Federal Institute of Technology (ETH Zurich).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the following disclaimer in the
 *  documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *  contributors may be used to endorse or promote products derived
 *  from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author:  Reto Da Forno
 */

/**
 * @addtogroup  lwb-scheduler
 * @{
 *
 * @defgroup    sched-ae scheduler used for the acoustic emission demo app
 * @{
 *
 * @brief
 * a simple scheduler implementation for the LWB
 * 
 * The scheduler has a static base period of LWB_CONF_SCHED_PERIOD_IDLE sec. 
 * There is no IPI (inter-packet interval) required for stream requests, the
 * scheduler will assign 1 slot to each stream in the next round. If the source
 * node wants to transmit more data, it needs to request another stream, i.e.
 * for each data packet, a stream request is necessary.
 * There are no more stream acknowledgements, but the SACK slot will be used
 * to acknowledge the data reception.
 * Schedule compression is not used
 * 
 * NOTE: 
 * - max. number of streams must not exceed the max. number of data slots
 * - the minimal idle period is 3s
 */
 
#include "lwb.h"

#ifdef LWB_SCHED_AE

/* parameter sanity check */

#if !defined(LWB_CONF_STREAM_EXTRA_DATA_LEN) || \
    (LWB_CONF_STREAM_EXTRA_DATA_LEN != 0)
#error "LWB_CONF_STREAM_EXTRA_DATA_LEN not set to 0!"
#endif

#if LWB_CONF_MAX_N_STREAMS > LWB_CONF_MAX_DATA_SLOTS
#error "max. # of streams mustn't be bigger than max. # of data slots"
#endif

#if LWB_CONF_SCHED_PERIOD_IDLE < 3
#error "LWB_CONF_SCHED_PERIOD_IDLE must be at least 3"
#endif

/* duration of the 'request round' in 10th of s, depends on the max. # nodes
 * (= LWB_CONF_MAX_N_STREAMS) */
#ifndef LWB_CONF_T_REQ_ROUND
#define LWB_CONF_T_REQ_ROUND    (uint16_t)((LWB_CONF_T_SCHED + \
                                 LWB_CONF_T_GAP + LWB_CONF_MAX_N_STREAMS * \
                                 (LWB_CONF_T_CONT + LWB_CONF_T_GAP) + \
                                 (RTIMER_SECOND_HF / 50)) / 325000 + 1)
#endif /* LWB_CONF_T_REQ_ROUND */

#ifndef MIN
#define MIN(x,y)        ((x) < (y) ? (x) : (y))
#endif /* MIN */

/*---------------------------------------------------------------------------*/
/**
 * @brief struct to store information about active streams on the host
 */
typedef struct stream_info {
  struct stream_info *next;
  uint16_t            node_id;
  uint8_t             state;
} lwb_stream_list_t;
/*---------------------------------------------------------------------------*/
static uint32_t time;                                         /* global time */
static uint16_t n_streams;                               /* # active streams */
static uint8_t  sched_state;                              /* scheduler state */
static uint8_t  n_pending_sack;
static uint16_t pending_sack[LWB_CONF_MAX_N_STREAMS];
LIST(streams_list);                    /* -> lists only work for data in RAM */
/* data structures to hold the stream info */
MEMB(streams_memb, lwb_stream_list_t, LWB_CONF_MAX_N_STREAMS);
/*---------------------------------------------------------------------------*/
uint8_t 
lwb_sched_prepare_sack(void *payload) 
{
  if(n_pending_sack) {
    DEBUG_PRINT_VERBOSE("%u S-ACKs pending", n_pending_sack);
    memcpy(payload, pending_sack, n_pending_sack * 2);
    return n_pending_sack * 2;
  }
  return 0;         /* return the length of the packet */
}
/*---------------------------------------------------------------------------*/
void 
lwb_sched_proc_srq(const lwb_stream_req_t* req) 
{
  lwb_stream_list_t *s = 0;
  uint8_t exists = 0;
  
  if(n_streams >= LWB_CONF_MAX_N_STREAMS) {
    DEBUG_PRINT_WARNING("stream request from node %u dropped, max #streams "
                        "reached", req->node_id);
    return;
  }  
  /* check if stream already exists */
  for(s = list_head(streams_list); s != 0; s = s->next) {
    if(req->node_id == s->node_id) {
      if(!s->state) {
        n_streams++;
        s->state = 1;
        DEBUG_PRINT_INFO("stream of node %u reactivated", req->node_id);
      }
      exists = 1;                    /* already exists */
      break;
    }
  }   
  if (!exists)
  {
    /* does not exist: add the new stream */
    s = memb_alloc(&streams_memb);
    if(s == 0) {
      DEBUG_PRINT_ERROR("out of memory: stream request dropped");
      return;
    }
    s->node_id       = req->node_id;
    s->state         = 1;
    /* insert the stream into the list, ordered by node id */
    lwb_stream_list_t *prev;
    for(prev = list_head(streams_list); prev != NULL; prev = prev->next) {
      if((req->node_id >= prev->node_id) && ((prev->next == NULL) || 
         (req->node_id < prev->next->node_id))) {
        break;
      }
    }
    list_insert(streams_list, prev, s);   
    n_streams++;
    DEBUG_PRINT_INFO("stream of node %u added", req->node_id);         
  }
  /* no need to send a stream acknowledgement */
}
/*---------------------------------------------------------------------------*/
uint16_t 
lwb_sched_compute(lwb_schedule_t * const sched, 
                  const uint8_t * const streams_to_update, 
                  uint8_t reserve_slot_host) 
{ 
  uint8_t n_slots_assigned = 0;  
  
  /* increment the timestamp */
  time += sched->period;
  
  /* scheduler states:
   * 0: idle
   * 1: contention detected, request round is next
   * 2: data round is next
   */
  if(sched_state == 0) {
    if(glossy_get_n_rx_started()) {
      DEBUG_PRINT_INFO("initiating a request round");
      /* clear the content of the schedule */
      memset(sched->slot, 0, sizeof(sched->slot)); 
      /* every node gets one slot (a chance to request a stream) */
      lwb_stream_list_t *curr_stream = list_head(streams_list);
      while(curr_stream != NULL) {
        sched->slot[n_slots_assigned] = curr_stream->node_id;
        n_slots_assigned++;
        /* go to the next stream in the list */
        curr_stream = curr_stream->next;
      }
      sched->period = 1;      /* set period to the smallest value */
      sched->n_slots = n_slots_assigned;
      sched_state++;
    } else {
      sched->n_slots = 0;
      sched->period  = LWB_CONF_SCHED_PERIOD_IDLE * 10;
      /* no data slots, just a contention slot */
      LWB_SCHED_SET_CONT_SLOT(sched);
    }
  } else if(sched_state == 1) {
    /* calculate the schedule based on the received stream requests */
    /* clear the content of the schedule */
    memset(sched->slot, 0, sizeof(sched->slot)); 
    /* are there any streams? */
    if(n_streams) {  
      /* go through the list of streams */
      lwb_stream_list_t *curr_stream = list_head(streams_list);
      while(curr_stream != NULL && 
            (n_slots_assigned < LWB_CONF_MAX_DATA_SLOTS)) {
        if(curr_stream->state) {
          sched->slot[n_slots_assigned] = curr_stream->node_id;
          n_slots_assigned++;
        }    
        /* go to the next stream in the list */
        curr_stream = curr_stream->next;
      }
    }
    sched->n_slots = n_slots_assigned;
    if(n_slots_assigned) {
      sched->period = LWB_CONF_T_REQ_ROUND;
      sched_state++;
      /* the next round will include an acknowledgement slot */
      LWB_SCHED_SET_SACK_SLOT(sched);
    } else {
      sched->period = LWB_CONF_SCHED_PERIOD_IDLE * 10 - 1;
      LWB_SCHED_SET_CONT_SLOT(sched);
      sched_state = 0;
    }
  } else if(sched_state == 2) {        
    n_pending_sack = 0;
    /* deactivate a stream if we received data */
    uint16_t i = 0;
    while (i < LWB_SCHED_N_SLOTS(sched)) {
      /* the node IDs will be in order */
      /* find the stream with this node ID in the list */
      lwb_stream_list_t *curr_stream = list_head(streams_list);
      while(curr_stream != NULL) {
        if(sched->slot[i] == curr_stream->node_id) {
          break;
        }
        curr_stream = curr_stream->next;
      }
      /* ignore inactive streams */
      if(curr_stream != NULL && curr_stream->state) {
        /* did we receive data from this node? */
        if(streams_to_update[i] != LWB_INVALID_STREAM_ID) {
          /* deactivate the stream */
          pending_sack[n_pending_sack] = curr_stream->node_id;
          n_pending_sack++;
          curr_stream->state = 0;
          if(n_streams == 0) {
            DEBUG_PRINT_WARNING("invalid stream cound");
          } else {
            n_streams--;
          }
          DEBUG_PRINT_INFO("data received, stream removed");
        }
      } else {
        /* this is not supposed to happen */
        DEBUG_PRINT_WARNING("data received from unknown stream");
      }
      i++;
    }
    sched->n_slots = 0;    
    sched->period = LWB_CONF_SCHED_PERIOD_IDLE * 10 - 
                    LWB_CONF_T_REQ_ROUND - 1;
    LWB_SCHED_SET_CONT_SLOT(sched);
    sched_state = 0;    /* back to idle state */
  }
  
  sched->time = time / 10;  
     
  /* log the parameters of the new schedule */
  DEBUG_PRINT_VERBOSE("schedule updated (s=%u T=%u n=%u|%u len=%u)", 
                      n_streams, sched->period / 10, n_slots_assigned, 
                      sched->n_slots >> 14, n_slots_assigned * 2);
  
  return (n_slots_assigned * 2) + LWB_SCHED_PKT_HEADER_LEN;
}
/*---------------------------------------------------------------------------*/
uint16_t 
lwb_sched_init(lwb_schedule_t* sched) 
{
  /* initialize streams member and list */
  memb_init(&streams_memb);
  list_init(streams_list);
  n_streams = 0;
  sched_state = 0;
  n_pending_sack = 0;
  time = 0;                             /* global time starts now */
  sched->n_slots = 0;
  LWB_SCHED_SET_CONT_SLOT(sched);       /* include a contention slot */
  sched->time = time;
  sched->period = LWB_CONF_SCHED_PERIOD_IDLE * 10;
  
  DEBUG_PRINT_INFO("AE scheduler initialized (max_streams: %u, t_req: %u)", 
                   LWB_CONF_MAX_N_STREAMS, LWB_CONF_T_REQ_ROUND);
  
  return LWB_SCHED_PKT_HEADER_LEN; /* empty schedule, no slots allocated yet */
}
/*---------------------------------------------------------------------------*/

#endif /* LWB_SCHED_AE */

/**
 * @}
 * @}
 */