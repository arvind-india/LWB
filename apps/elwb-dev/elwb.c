/*
 * Copyright (c) 2017, Swiss Federal Institute of Technology (ETH Zurich).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
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
 *          Felix Sutton
 * 
 * Version: 2.1
 */

/**
 * @file
 *
 * a modified implementation of the Low-Power Wireless Bus called e-LWB
 * (event-based/triggered LWB)
 * it is a many-to-one protocol for fast data dissemination under rapidly
 * chanding loads
 * 
 * header length is 0, neither recipient node ID nor stream ID are required 
 * since all the data flows to the sinks
 */
 
#include "contiki.h"


#if LWB_VERSION == 0

/* parameter checks */
#if !defined(LWB_SCHED_ELWB_DYN)
#error "eLWB only supports the ELWB_DYN scheduler!"
#endif

/*---------------------------------------------------------------------------*/
#define LWB_PERIOD_SCALE                100   /* also change in sched-elwb.c */

#if LWB_CONF_HEADER_LEN != 0
#error "LWB_CONF_HEADER_LEN must be set to 0!"
#endif /* LWB_CONF_HEADER_LEN */

/* expected packet length of a slot request */         
#ifndef LWB_CONF_SRQ_PKT_LEN
#define LWB_CONF_SRQ_PKT_LEN        2 
#endif /* LWB_CONF_SRQ_PKT_LEN */

#if LWB_CONF_SRQ_PKT_LEN != 2
#warning "LWB_CONF_SRQ_PKT_LEN should be set to 1!"
#endif /* LWB_CONF_SRQ_PKT_LEN */

/* set to 1 to directly forward all received messages to BOLT */
#ifndef LWB_CONF_WRITE_TO_BOLT
#define LWB_CONF_WRITE_TO_BOLT      0
#endif /* LWB_CONF_WRITE_TO_BOLT */

/* by default, forward all received packets to the app task on the source nodes
 * that originated from the host (or ID 0) */
#ifndef LWB_CONF_SRC_PKT_FILTER
/* if expression evaluates to 'true', the packet is forwarded/kept */
#define LWB_CONF_SRC_PKT_FILTER(data)  \
                         (schedule.slot[i] == 0 || schedule.slot[i] == HOST_ID)
#endif /* LWB_CONF_SRC_PKT_FILTER */
/*---------------------------------------------------------------------------*/
/* use a bit to indicate whether it is a contention or a data round */
#define IS_DATA_ROUND(s)            LWB_SCHED_HAS_SACK_SLOT(s)
/* use the DACK bit to indicate an idle state */
#define IS_STATE_IDLE(s)            LWB_SCHED_HAS_DACK_SLOT(s)
/* schedule that contains a contention slot is the first schedule of a round */
#define IS_FIRST_SCHEDULE(s)        LWB_SCHED_HAS_CONT_SLOT(s)
/*---------------------------------------------------------------------------*/
/* internal sync state of the LWB */
typedef enum {
  BOOTSTRAP = 0,
  SYNCED,
  UNSYNCED,
  UNSYNCED2,
  NUM_OF_SYNC_STATES
} lwb_sync_state_t;
/*---------------------------------------------------------------------------*/
typedef enum {
  EVT_SCHED_RCVD = 0,
  EVT_SCHED_MISSED,
  NUM_OF_SYNC_EVENTS
} sync_event_t;
/*---------------------------------------------------------------------------*/
typedef struct {
  uint8_t   op;          /* pending operation: 0 = none, 1 = read, 2 = write */
  uint8_t   len;                            /* for write op: number of bytes */
  uint8_t*  notify;  /* pointer to notification byte, length will be written */
  uint8_t*  sram_ptr;                       /* local buffer (16-bit address) */
  uint32_t  xmem_addr;              /* 32-bit address in the external memory */
  /* note: 'op' and 'notify' are RW for the xmem task, whereas the other 
   *       fields are read-only! */
} xmem_task_t;
/*---------------------------------------------------------------------------*/
/**
 * @brief the finite state machine for the time synchronization on a source 
 * node the next state can be retrieved from the current state (column) and
 * the latest event (row)
 * @note  undefined transitions force the SM to go back into bootstrap
 */
static const 
lwb_sync_state_t next_state[NUM_OF_SYNC_EVENTS][NUM_OF_SYNC_STATES] = 
{/* STATES:                                         EVENTS:         */
 /* BOOTSTRAP, SYNCED,   UNSYNCED,  UNSYNCED2                       */
  { SYNCED,    SYNCED,   SYNCED,    SYNCED    }, /* schedule rcvd   */
  { BOOTSTRAP, UNSYNCED, UNSYNCED2, BOOTSTRAP }  /* schedule missed */
};
static const char* lwb_sync_state_to_string[NUM_OF_SYNC_STATES] = {
  "BOOTSTRAP", "SYN", "USYN", "USYN2"
};
static const uint32_t guard_time[NUM_OF_SYNC_STATES] = {
/*BOOTSTRAP,        SYNCED,           UNSYNCED,           UNSYNCED2 */
  LWB_CONF_T_GUARD, LWB_CONF_T_GUARD, LWB_CONF_T_GUARD_1, LWB_CONF_T_GUARD_2
};
/*---------------------------------------------------------------------------*/
#ifdef LWB_CONF_TASK_ACT_PIN
  #define LWB_TASK_RESUMED        PIN_CLR(LWB_CONF_TASK_ACT_PIN); \
                                  PIN_SET(LWB_CONF_TASK_ACT_PIN)
  #define LWB_TASK_SUSPENDED      PIN_CLR(LWB_CONF_TASK_ACT_PIN)
#else
  #define LWB_TASK_RESUMED     
  #define LWB_TASK_SUSPENDED  
#endif
/*---------------------------------------------------------------------------*/
#define LWB_DATA_RCVD             (glossy_get_n_rx() > 0) 
#define GET_EVENT                 (glossy_is_t_ref_updated() ? \
                                   EVT_SCHED_RCVD : EVT_SCHED_MISSED)
/*---------------------------------------------------------------------------*/
#define LWB_SEND_SCHED() \
{\
  glossy_start(node_id, (uint8_t *)&schedule, schedule_len, \
               LWB_CONF_TX_CNT_SCHED, GLOSSY_WITH_SYNC, GLOSSY_WITH_RF_CAL);\
  LWB_WAIT_UNTIL(rt->time + LWB_CONF_T_SCHED);\
  glossy_stop();\
}   
#define LWB_RCV_SCHED() \
{\
  glossy_start(GLOSSY_UNKNOWN_INITIATOR, (uint8_t *)&schedule, payload_len, \
               LWB_CONF_TX_CNT_SCHED, GLOSSY_WITH_SYNC, GLOSSY_WITH_RF_CAL);\
  LWB_WAIT_UNTIL(rt->time + LWB_CONF_T_SCHED + t_guard);\
  glossy_stop();\
}   
#define LWB_SEND_PACKET() \
{\
  glossy_start(node_id, (uint8_t*)glossy_payload, payload_len, \
               LWB_CONF_TX_CNT_DATA, GLOSSY_WITHOUT_SYNC, \
               GLOSSY_WITHOUT_RF_CAL);\
  LWB_WAIT_UNTIL(rt->time + t_slot);\
  glossy_stop();\
}
#define LWB_RCV_PACKET() \
{\
  glossy_start(GLOSSY_UNKNOWN_INITIATOR, (uint8_t*)glossy_payload, \
               payload_len, \
               LWB_CONF_TX_CNT_DATA, GLOSSY_WITHOUT_SYNC, \
               GLOSSY_WITHOUT_RF_CAL);\
  LWB_WAIT_UNTIL(rt->time + t_slot + t_guard);\
  glossy_stop();\
}
/*---------------------------------------------------------------------------*/
/* suspend the LWB proto-thread until the rtimer reaches the specified time */
#define LWB_WAIT_UNTIL(time) \
{\
  rtimer_schedule(LWB_CONF_RTIMER_ID, time, 0, callback_func);\
  LWB_TASK_SUSPENDED;\
  PT_YIELD(&lwb_pt);\
  LWB_TASK_RESUMED;\
}
/* same as LWB_WAIT_UNTIL, but use the LF timer to schedule the wake-up */
#define LWB_LF_WAIT_UNTIL(time) \
{\
  rtimer_schedule(LWB_CONF_LF_RTIMER_ID, time, 0, callback_func);\
  LWB_TASK_SUSPENDED;\
  PT_YIELD(&lwb_pt);\
  LWB_TASK_RESUMED;\
}
/*---------------------------------------------------------------------------*/
static struct pt        lwb_pt;
static struct process*  post_proc;
static struct process*  pre_proc;
static lwb_sync_state_t sync_state;
static rtimer_clock_t   rx_timestamp;
static uint32_t         global_time;
static lwb_schedule_t   schedule;
static uint8_t          schedule_len;
static uint16_t         glossy_payload[(LWB_CONF_MAX_PKT_LEN + 1) / 2];
static uint8_t          payload_len;
static uint32_t         t_preprocess;
static uint32_t         t_slot;
static uint32_t         t_slot_ofs;
static uint32_t         t_guard;
static const void*      callback_func;
static lwb_statistics_t stats = { 0 };
static rtimer_clock_t   last_synced_lf;
#if !LWB_CONF_USE_XMEM
/* allocate memory in the SRAM (+1 to store the message length) */
static uint8_t          in_buffer_mem[LWB_CONF_IN_BUFFER_SIZE * 
                                      (LWB_CONF_MAX_DATA_PKT_LEN + 1)];  
static uint8_t          out_buffer_mem[LWB_CONF_OUT_BUFFER_SIZE * 
                                       (LWB_CONF_MAX_DATA_PKT_LEN + 1)]; 
#else /* LWB_CONF_USE_XMEM */
static uint8_t          xmem_buffer[LWB_CONF_MAX_DATA_PKT_LEN + 1];
/* shared memory for process-to-protothread communication (xmem access) */
static xmem_task_t      xmem_task = { 0 };
#endif /* LWB_CONF_USE_XMEM */
FIFO(in_buffer, LWB_CONF_MAX_DATA_PKT_LEN + 1, LWB_CONF_IN_BUFFER_SIZE);
FIFO(out_buffer, LWB_CONF_MAX_DATA_PKT_LEN + 1, LWB_CONF_OUT_BUFFER_SIZE);
/*---------------------------------------------------------------------------*/
PROCESS(lwb_process, "Com Task (eLWB)");/* process ctrl block def. */
/*---------------------------------------------------------------------------*/
/* store a received message in the incoming queue, returns 1 if successful, 
 * 0 otherwise */
uint8_t 
lwb_in_buffer_put(uint8_t* data, uint8_t len)
{  
  if(!len || len > LWB_CONF_MAX_DATA_PKT_LEN) {
    DEBUG_PRINT_WARNING("lwb: invalid packet received");
    return 0;
  }
  /* received messages will have the max. length LWB_CONF_MAX_DATA_PKT_LEN */
  uint32_t pkt_addr = fifo_put(&in_buffer);
  if(FIFO_ERROR != pkt_addr) {
#if !LWB_CONF_USE_XMEM
    /* copy the data into the queue */
    uint8_t* next_msg = (uint8_t*)((uint16_t)pkt_addr);
    memcpy(next_msg, data, len);
    /* last byte holds the payload length */
    *(next_msg + LWB_CONF_MAX_DATA_PKT_LEN) = len;    
#else /* LWB_CONF_USE_XMEM */
    if(xmem_task.op) {
      DEBUG_PRINT_ERROR("xmem task busy, operation skipped");
      return 0;
    }
    /* schedule a write operation from the external memory */
    xmem_task.op        = 2;
    xmem_task.len       = len;
    xmem_task.xmem_addr = pkt_addr;
    xmem_task.sram_ptr  = data;
    process_poll(&lwb_process);    
#endif /* LWB_CONF_USE_XMEM */
    return 1;
  }
  stats.rxbuf_drop++;
  DEBUG_PRINT_WARNING("lwb rx queue full");
  return 0;
}
/*---------------------------------------------------------------------------*/
/* fetch the next 'ready-to-send' message from the outgoing queue
 * returns 1 if successful, 0 otherwise */
uint8_t 
lwb_out_buffer_get(uint8_t* out_data, uint8_t* out_len)
{   
  /* messages have the max. length LWB_CONF_MAX_DATA_PKT_LEN and are already
   * formatted */
  uint32_t pkt_addr = fifo_get(&out_buffer);
  if(FIFO_ERROR != pkt_addr) {
#if !LWB_CONF_USE_XMEM
    /* assume pointers are always 16-bit */
    uint8_t* next_msg = (uint8_t*)((uint16_t)pkt_addr);  
    memcpy(out_data, next_msg, *(next_msg + LWB_CONF_MAX_DATA_PKT_LEN));
    *out_len = *(next_msg + LWB_CONF_MAX_DATA_PKT_LEN);
#else /* LWB_CONF_USE_XMEM */
    if(xmem_task.op) {
      DEBUG_PRINT_ERROR("xmem task busy, operation skipped");
      return 0;
    }
    /* schedule a read operation from the external memory */
    xmem_task.op        = 1;
    xmem_task.notify    = out_len;
    xmem_task.xmem_addr = pkt_addr;
    xmem_task.sram_ptr  = out_data;
    process_poll(&lwb_process);
#endif /* LWB_CONF_USE_XMEM */
    return 1;
  }
  DEBUG_PRINT_VERBOSE("lwb tx queue empty");
  return 0;
}
/*---------------------------------------------------------------------------*/
/* puts a message into the outgoing queue, returns 1 if successful, 
 * 0 otherwise;
 * needs to have all 4 parameters to be compatible with lwb.h */
uint8_t
lwb_send_pkt(uint16_t recipient,
             uint8_t stream_id,
             const uint8_t* const data,
             uint8_t len)
{
  /* data has the max. length LWB_CONF_MAX_DATA_PKT_LEN, lwb header needs 
   * to be added before the data is inserted into the queue */
  if(len > LWB_CONF_MAX_DATA_PKT_LEN || !data) {
    return 0;
  }
  uint32_t pkt_addr = fifo_put(&out_buffer);
  if(FIFO_ERROR != pkt_addr) {
#if !LWB_CONF_USE_XMEM
    /* assume pointers are 16-bit */
    uint8_t* next_msg = (uint8_t*)((uint16_t)pkt_addr);
    *(next_msg + LWB_CONF_MAX_DATA_PKT_LEN) = len;
    memcpy(next_msg, data, len);
#else /* LWB_CONF_USE_XMEM */
    memcpy(xmem_buffer, data, len);
    *(xmem_buffer + LWB_CONF_MAX_DATA_PKT_LEN) = len;
    xmem_wait_until_ready();
    xmem_write(pkt_addr, LWB_CONF_MAX_DATA_PKT_LEN + 1, xmem_buffer);
#endif /* LWB_CONF_USE_XMEM */
    DEBUG_PRINT_VERBOSE("msg added to lwb tx queue");
    return 1;
  }
  stats.txbuf_drop++;
  DEBUG_PRINT_VERBOSE("lwb tx queue full");
  return 0;
}
/*---------------------------------------------------------------------------*/
/* copies the oldest received message in the queue into out_data and returns 
 * the message size (in bytes); needs to have all 3 parameters to be 
 * compatible with lwb.h */
uint8_t
lwb_rcv_pkt(uint8_t* out_data,
            uint16_t * const out_node_id,
            uint8_t * const out_stream_id)
{ 
  if(!out_data) { return 0; }
  /* messages in the queue have the max. length LWB_CONF_MAX_DATA_PKT_LEN, 
   * lwb header needs to be stripped off; payload has max. length
   * LWB_CONF_MAX_DATA_PKT_LEN */
  uint32_t pkt_addr = fifo_get(&in_buffer);
  if(FIFO_ERROR != pkt_addr) {
#if !LWB_CONF_USE_XMEM
    /* assume pointers are 16-bit */
    uint8_t* next_msg = (uint8_t*)((uint16_t)pkt_addr); 
    memcpy(out_data, next_msg, *(next_msg + LWB_CONF_MAX_DATA_PKT_LEN));
    return *(next_msg + LWB_CONF_MAX_DATA_PKT_LEN);
#else /* LWB_CONF_USE_XMEM */
    if(!xmem_read(pkt_addr, LWB_CONF_MAX_DATA_PKT_LEN + 1, xmem_buffer)) {
      DEBUG_PRINT_ERROR("xmem_read() failed");
      return 0;
    }
    xmem_wait_until_ready(); /* wait for the data transfer to complete */
    uint8_t msg_len = *(xmem_buffer + LWB_CONF_MAX_DATA_PKT_LEN);
    memcpy(out_data, xmem_buffer, msg_len);
    return msg_len;
#endif /* LWB_CONF_USE_XMEM */
  }
  DEBUG_PRINT_VERBOSE("lwb rx queue empty");
  return 0;
}
/*---------------------------------------------------------------------------*/
uint8_t
lwb_get_rcv_buffer_state(void)
{
  return FIFO_CNT(&in_buffer);
}
/*---------------------------------------------------------------------------*/
uint8_t
lwb_get_send_buffer_state(void)
{
  return FIFO_CNT(&out_buffer);
}
/*---------------------------------------------------------------------------*/
const lwb_statistics_t * const
lwb_get_stats(void)
{
  return &stats;
}
/*---------------------------------------------------------------------------*/
uint32_t 
lwb_get_time(rtimer_clock_t* reception_time)
{
  if(reception_time) {
    *reception_time = rx_timestamp;
  }
  return global_time;
}
/*---------------------------------------------------------------------------*/
uint64_t
lwb_get_timestamp(void)
{
  /* convert to microseconds */
  uint64_t timestamp = (uint64_t)global_time * 1000000;
  if(sync_state <= SYNCED) {
    return timestamp + /* convert to microseconds */
           (rtimer_now_hf() - rx_timestamp) * 1000000 / RTIMER_SECOND_HF;
  }
  /* not synced! */
  return timestamp +
         (rtimer_now_lf() - last_synced_lf) * 1000000 / RTIMER_SECOND_LF;
}
/*---------------------------------------------------------------------------*/
/**
 * @brief thread of the host node
 */
PT_THREAD(lwb_thread_host(rtimer_t *rt)) 
{  
  /* variables specific to the host (all must be static) */
  static rtimer_clock_t t_start;
  static rtimer_clock_t t_start_lf;
  static uint16_t curr_period = 0;
  static uint16_t srq_cnt = 0;
  static int8_t glossy_rssi = 0;
  
  PT_BEGIN(&lwb_pt);

  callback_func = lwb_thread_host;
  t_guard       = LWB_CONF_T_GUARD;      /* constant guard time for the host */
   
  while(1) {
  
  #if LWB_CONF_T_PREPROCESS
    if(t_preprocess) {
      if(pre_proc) {
        process_poll(pre_proc);
      }
      
      /* update the schedule in case there is data to send!
      * note: this will delay the sending of the schedule by some time! */
      if(LWB_SCHED_HAS_CONT_SLOT(&schedule) && !FIFO_EMPTY(&out_buffer)) {
        schedule_len = lwb_sched_compute(&schedule, 0, 
                                        lwb_get_send_buffer_state());
        DEBUG_PRINT_VERBOSE("schedule recomputed");
      }
      LWB_LF_WAIT_UNTIL(rt->time + LWB_CONF_T_PREPROCESS);
      t_preprocess = 0; /* reset value */
    }    
  #endif /* LWB_CONF_T_PREPROCESS */
      
    /* --- COMMUNICATION ROUND STARTS --- */
        
    t_start_lf = rt->time; 
    rt->time = rtimer_now_hf();    
    t_start = rt->time;

    /* --- SEND SCHEDULE --- */    
    LWB_SEND_SCHED();
   
    glossy_rssi     = glossy_get_rssi(0);
    stats.relay_cnt = glossy_get_relay_cnt_first_rx();
    t_slot_ofs      = (LWB_CONF_T_SCHED + LWB_CONF_T_GAP);
    global_time     = schedule.time;
    rx_timestamp    = t_start;
        
  #if LWB_CONF_USE_XMEM
    /* put the external memory back into active mode (takes ~500us) */
    xmem_wakeup();    
  #endif /* LWB_CONF_USE_XMEM */

    /* --- DATA SLOTS --- */
    
    if(LWB_SCHED_HAS_DATA_SLOT(&schedule)) {

    #if LWB_CONF_SCHED_COMPRESS
      lwb_sched_uncompress((uint8_t*)schedule.slot, 
                           LWB_SCHED_N_SLOTS(&schedule));
    #endif /* LWB_CONF_SCHED_COMPRESS */

      /* adjust T_DATA (modification to original LWB) */
      if(IS_DATA_ROUND(&schedule)) {
        /* this is a data round */
        t_slot = LWB_CONF_T_DATA;
      } else {
        t_slot = LWB_CONF_T_CONT;
      }
      static uint16_t i;
      for(i = 0; i < LWB_SCHED_N_SLOTS(&schedule); i++) {
        /* is this our slot? Note: slots assigned to node ID 0 always belong 
         * to the host */
        if(schedule.slot[i] == 0 || schedule.slot[i] == node_id) {
          /* send a data packet (if there is any) */
          lwb_out_buffer_get((uint8_t*)glossy_payload, &payload_len);
          if(payload_len) { 
            /* note: stream ID is irrelevant here */
            /* wait until the data slot starts */
            LWB_WAIT_UNTIL(t_start + t_slot_ofs);  
            LWB_SEND_PACKET();
            DEBUG_PRINT_VERBOSE("data packet sent (%ub)", payload_len);
          }
        } else {
          if(IS_DATA_ROUND(&schedule)) {
            payload_len = GLOSSY_UNKNOWN_PAYLOAD_LEN;
          } else {
            /* it's a request round */
            payload_len = LWB_CONF_SRQ_PKT_LEN;
          }
          /* wait until the data slot starts */
          LWB_WAIT_UNTIL(t_start + t_slot_ofs - t_guard);
          LWB_RCV_PACKET();  /* receive a data packet */
          payload_len = glossy_get_payload_len();
          if(LWB_DATA_RCVD) {
            if(!IS_DATA_ROUND(&schedule)) {
              /* stream request! */
              uint16_t srq[2] = { schedule.slot[i], glossy_payload[0] };
              lwb_sched_proc_srq((lwb_stream_req_t*)srq);
            } else {
              DEBUG_PRINT_VERBOSE("data received from node %u (%ub)", 
                                  schedule.slot[i], payload_len);
  #if LWB_CONF_WRITE_TO_BOLT
              bolt_write((uint8_t*)glossy_payload, payload_len);
  #else /* LWB_CONF_WRITE_TO_BOLT */
              lwb_in_buffer_put((uint8_t*)glossy_payload, payload_len);
  #endif /* LWB_CONF_WRITE_TO_BOLT */
              /* update statistics */
              stats.rx_total += payload_len;
              stats.pck_cnt++;
            }
          } else {
            DEBUG_PRINT_VERBOSE("no data received from node %u", 
                                schedule.slot[i]);
          }
        }
        t_slot_ofs += (t_slot + LWB_CONF_T_GAP);
      }      
  #if LWB_CONF_WRITE_TO_BOLT
      if(IS_DATA_ROUND(&schedule)) {
        static uint16_t pkt_cnt_prev;
        uint16_t diff = stats.pck_cnt - pkt_cnt_prev;
        if(diff) {
          DEBUG_PRINT_INFO("%u msg forwarded to BOLT", diff);
          pkt_cnt_prev = stats.pck_cnt;
        }
      }
  #endif /* LWB_CONF_WRITE_TO_BOLT */
    }
    
    /* --- CONTENTION SLOT --- */
    
    if(LWB_SCHED_HAS_CONT_SLOT(&schedule)) {
      t_slot = LWB_CONF_T_CONT;
      payload_len = LWB_CONF_SRQ_PKT_LEN;
      glossy_payload[0] = 0;
      /* wait until the slot starts, then receive the packet */
      LWB_WAIT_UNTIL(t_start + t_slot_ofs - t_guard);
      LWB_RCV_PACKET();
      if(LWB_DATA_RCVD && glossy_payload[0] != 0) {
        /* process the request only if there is a valid node ID */
        //DEBUG_PRINT_INFO("request received from node %u", glossy_payload[0]);
        lwb_sched_proc_srq((lwb_stream_req_t*)glossy_payload);
      }
      if(glossy_get_n_rx_started()) {
        /* set the period to 0 to notify the scheduler that at 
         * least one nodes wants to request a stream (has data to send) */
        schedule.period = 0;
        srq_cnt++;
        
        /* compute 2nd schedule */
        lwb_sched_compute(&schedule, 0, 0);
        glossy_payload[0] = schedule.period;
      } else {
        /* else: no update to schedule needed; set period to 0 to indicate to source
         * nodes that there is no change in period (and no request round 
         * following) */
        glossy_payload[0] = 0;
      }
      t_slot_ofs += LWB_CONF_T_CONT + LWB_CONF_T_GAP;
  
      /* --- SEND 2ND SCHEDULE --- */
      
      /* (just a 2-byte packet to indicate a change in the round period) */    
      /* send the 2nd schedule only if there was a contention slot */
      payload_len = 2;
      LWB_WAIT_UNTIL(t_start + t_slot_ofs);
      LWB_SEND_PACKET();    /* send as normal packet! saves energy */
    }
    
    /* --- COMMUNICATION ROUND ENDS --- */
    /* time for other computations */
                
    /* poll the other processes to allow them to run after the LWB task was 
     * suspended (note: the polled processes will be executed in the inverse
     * order they were started/created) */
    if(IS_STATE_IDLE(&schedule)) {
      /* print out some stats */
      DEBUG_PRINT_INFO("t=%lu T=%u n=%u srq=%u p=%u per=%d "
                       "rssi=%ddBm", 
                       schedule.time,
                       curr_period * (1000 / LWB_PERIOD_SCALE),
                       LWB_SCHED_N_SLOTS(&schedule),
                       srq_cnt, 
                       stats.pck_cnt,
                       glossy_get_per(),
                       glossy_rssi);
      
    #if LWB_CONF_USE_XMEM
      /* make sure the xmem task has a chance to run, yield for T_GAP */
      rtimer_schedule(LWB_CONF_RTIMER_ID, rtimer_now_hf() + LWB_CONF_T_GAP,
                      0, callback_func);
      LWB_TASK_SUSPENDED;
      PT_YIELD(&lwb_pt);
      LWB_TASK_RESUMED;
    #endif /* LWB_CONF_USE_XMEM */
    
      if(post_proc) {
        process_poll(post_proc);    
      }
    #if LWB_CONF_T_PREPROCESS
      t_preprocess = LWB_CONF_T_PREPROCESS;     
    #endif /* LWB_CONF_T_PREPROCESS */
    }
    
    /* --- COMPUTE NEW SCHEDULE (for the next round) --- */
    curr_period  = schedule.period; /* required to schedule the next wake-up */
    schedule_len = lwb_sched_compute(&schedule, 0, 
                                     lwb_get_send_buffer_state());
    if(!schedule_len) {
      DEBUG_PRINT_ERROR("invalid schedule (0 bytes)");
    }
    
    /* suspend this task and wait for the next round */
    LWB_LF_WAIT_UNTIL(t_start_lf + curr_period * RTIMER_SECOND_LF / 
                      LWB_PERIOD_SCALE - t_preprocess);
  }
  
  PT_END(&lwb_pt);
}
/*---------------------------------------------------------------------------*/
/**
 * @brief declaration of the protothread (source node)
 */
PT_THREAD(lwb_thread_src(rtimer_t *rt)) 
{  
  /* variables specific to the source node (all must be static) */
  static rtimer_clock_t t_ref;
  static rtimer_clock_t t_ref_lf;
  static uint8_t  node_registered = 0;
  static uint16_t period_idle;        /* last base period */
  
  PT_BEGIN(&lwb_pt);
  
  memset(&schedule, 0, sizeof(schedule));
  sync_state    = BOOTSTRAP;
  callback_func = lwb_thread_src;
  
  while(1) {
        
  #if LWB_CONF_T_PREPROCESS
    if(t_preprocess) {
      if(pre_proc) {
        process_poll(pre_proc);
      }
      LWB_LF_WAIT_UNTIL(rt->time + LWB_CONF_T_PREPROCESS);
      t_preprocess = 0;
    }
  #endif /* LWB_CONF_T_PREPROCESS */
    
    /* --- COMMUNICATION ROUND STARTS --- */
    
    rt->time = rtimer_now_hf();            /* overwrite LF with HF timestamp */
            
    /* --- RECEIVE SCHEDULE --- */
    
    payload_len = GLOSSY_UNKNOWN_PAYLOAD_LEN;
    if(sync_state == BOOTSTRAP) {
      while (1) {
        schedule.n_slots = 0;   /* reset */
        DEBUG_PRINT_MSG_NOW("BOOTSTRAP");
        stats.bootstrap_cnt++;
        static rtimer_clock_t bootstrap_started;
        bootstrap_started = rtimer_now_hf();
        /* synchronize first! wait for the first schedule... */
        do {
          /* turn radio on */
          glossy_start(GLOSSY_UNKNOWN_INITIATOR, (uint8_t *)&schedule,
                      payload_len, LWB_CONF_TX_CNT_SCHED, GLOSSY_WITH_SYNC,
                      GLOSSY_WITH_RF_CAL);
          LWB_WAIT_UNTIL(rt->time + LWB_CONF_T_SCHED);
          glossy_stop();
        } while(!glossy_is_t_ref_updated() && ((rtimer_now_hf() -
                bootstrap_started) < LWB_CONF_T_SILENT));        
        if (glossy_is_t_ref_updated()) {
          break;  /* schedule received, exit bootstrap state */
        }
        /* go to sleep for LWB_CONF_T_DEEPSLEEP ticks */
        stats.sleep_cnt++;
        DEBUG_PRINT_MSG_NOW("timeout, entering sleep mode");
        LWB_BEFORE_DEEPSLEEP();
        LWB_LF_WAIT_UNTIL(rtimer_now_lf() + LWB_CONF_T_DEEPSLEEP);
        LWB_AFTER_DEEPSLEEP();
        rt->time = rtimer_now_hf();
      }
    } else {
      LWB_RCV_SCHED();
    }
    stats.glossy_snr = glossy_get_snr();
                   
  #if LWB_CONF_USE_XMEM
    /* put the external memory back into active mode (takes ~500us) */
    xmem_wakeup();    
  #endif /* LWB_CONF_USE_XMEM */

    /* --- SYNC --- */
    
    /* update the sync state machine (compute new sync state and update 
     * t_guard) */
    sync_state = next_state[GET_EVENT][sync_state];
    t_guard = guard_time[sync_state];               /* adjust the guard time */
    if(sync_state == UNSYNCED) {
      stats.unsynced_cnt++;
    } else if(sync_state == BOOTSTRAP) {
      t_preprocess = 0;
      continue;
    }
     
    if(glossy_is_t_ref_updated()) {
      /* HF timestamp of 1st RX, subtract const offset to align src and host */
      t_ref = glossy_get_t_ref() - LWB_CONF_T_REF_OFS;      
      /* calculate t_ref_lf by subtracting the elapsed time since t_ref: */
      rtimer_clock_t hf_now;
      rtimer_now(&hf_now, &t_ref_lf);
      t_ref_lf -= (uint32_t)(hf_now - t_ref) / (uint32_t)RTIMER_HF_LF_RATIO;
      if(IS_FIRST_SCHEDULE(&schedule)) {        
        /* do some basic drift estimation: measured elapsed time minus
         * effective elapsed time (given by host) */
        int16_t drift = (int16_t)((int32_t)(t_ref_lf - last_synced_lf) - 
                    (int32_t)(schedule.time - global_time) * RTIMER_SECOND_LF);
        if(drift < 100 && drift > -100) {
          stats.drift = (stats.drift + drift) / 2;
        }
        /* only update the timestamp during the idle period */
        period_idle = schedule.period;
        global_time = schedule.time;
        last_synced_lf = t_ref_lf;
        rx_timestamp = t_ref;
      }
      stats.relay_cnt = glossy_get_relay_cnt_first_rx();
      
    } else {
      DEBUG_PRINT_WARNING("schedule missed");
      /* we can only estimate t_ref and t_ref_lf */
      if(!IS_STATE_IDLE(&schedule)) {
        /* missed schedule was during a contention/data round -> reset t_ref */
        t_ref_lf = last_synced_lf;      /* restore the last known sync point */
        if(IS_DATA_ROUND(&schedule)) {
          /* last round was a data round? -> add one period */
          t_ref_lf += period_idle * RTIMER_SECOND_LF / LWB_PERIOD_SCALE;
        }
        schedule.period = period_idle;
      } else {
        t_ref_lf += period_idle * RTIMER_SECOND_LF / LWB_PERIOD_SCALE;
      }
    }
        
    /* permission to participate in this round? */
    if(sync_state == SYNCED) {
      
    #if LWB_CONF_SCHED_COMPRESS
      lwb_sched_uncompress((uint8_t*)schedule.slot, 
                           LWB_SCHED_N_SLOTS(&schedule));
    #endif /* LWB_CONF_SCHED_COMPRESS */
      
      static uint16_t i;
      t_slot_ofs = (LWB_CONF_T_SCHED + LWB_CONF_T_GAP); 
    
      /* --- DATA SLOTS --- */
      
      if(LWB_SCHED_HAS_DATA_SLOT(&schedule)) {
        /* set the slot duration */
        if(IS_DATA_ROUND(&schedule)) {
          /* this is a data round */
          t_slot = LWB_CONF_T_DATA;
        } else {
          /* it's a request round */
          t_slot = LWB_CONF_T_CONT;
          node_registered = 0;
        }
        for(i = 0; i < LWB_SCHED_N_SLOTS(&schedule); i++) {
          if(schedule.slot[i] == node_id) {
            node_registered = 1;
            stats.t_slot_last = schedule.time;
            /* this is our data slot, send a data packet */
            if(!FIFO_EMPTY(&out_buffer)) {
              if(IS_DATA_ROUND(&schedule)) {
                lwb_out_buffer_get((uint8_t*)glossy_payload, &payload_len);
              } else {
                payload_len = LWB_CONF_SRQ_PKT_LEN;
                /* request as many data slots as there are pkts in the queue*/
                *(uint8_t*)glossy_payload = lwb_get_send_buffer_state();
              }
              LWB_WAIT_UNTIL(t_ref + t_slot_ofs);
              LWB_SEND_PACKET();
              DEBUG_PRINT_VERBOSE("packet sent (%ub)", payload_len);
            } else {
              DEBUG_PRINT_VERBOSE("no message to send (data slot ignored)");
            }
          } else
          {
            payload_len = GLOSSY_UNKNOWN_PAYLOAD_LEN;
            if(!IS_DATA_ROUND(&schedule)) {
              /* the payload length is known in the request round */
              payload_len = LWB_CONF_SRQ_PKT_LEN;
            }
            /* receive a data packet */
            LWB_WAIT_UNTIL(t_ref + t_slot_ofs - t_guard);
            LWB_RCV_PACKET();
            payload_len = glossy_get_payload_len();
            /* forward the packet to the application task if the initiator was
             * the host or sink or if the custom forward filter is 'true' */
            if(LWB_CONF_SRC_PKT_FILTER(glossy_payload)) {
  #if LWB_CONF_WRITE_TO_BOLT
              bolt_write((uint8_t*)glossy_payload, payload_len);
  #else /* LWB_CONF_WRITE_TO_BOLT */
              lwb_in_buffer_put((uint8_t*)glossy_payload, payload_len);
  #endif /* LWB_CONF_WRITE_TO_BOLT */
            }
            stats.rx_total += payload_len;
            stats.pck_cnt++;
          }
          t_slot_ofs += (t_slot + LWB_CONF_T_GAP);
        }
      }
      
      /* --- CONTENTION SLOT --- */
      
      /* is there a contention slot in this round? */
      if(LWB_SCHED_HAS_CONT_SLOT(&schedule)) {
        t_slot = LWB_CONF_T_CONT;
        if(!FIFO_EMPTY(&out_buffer)) {
          /* if there is data in the output buffer, then request a slot */
          /* a slot request packet always looks the same (1 byte) */
          payload_len = LWB_CONF_SRQ_PKT_LEN;
          /* include the node ID in case this is the first request */
          glossy_payload[0] = 0;
          if(!node_registered) {
            payload_len = 2;
            glossy_payload[0] = node_id;
            DEBUG_PRINT_INFO("transmitting node ID");
          }
          /* wait until the contention slot starts */
          LWB_WAIT_UNTIL(t_ref + t_slot_ofs);
          LWB_SEND_PACKET();
        } else {
          /* no request pending -> just receive / relay packets */
          payload_len = LWB_CONF_SRQ_PKT_LEN;
          LWB_WAIT_UNTIL(t_ref + t_slot_ofs - t_guard);
          LWB_RCV_PACKET();
        }
        t_slot_ofs += LWB_CONF_T_CONT + LWB_CONF_T_GAP;
                   
        /* --- RECEIVE 2ND SCHEDULE --- */
      
        /* only rcv the 2nd schedule if there was a contention slot */
        payload_len = 2;  /* we expect exactly 2 bytes */
        LWB_WAIT_UNTIL(t_ref + t_slot_ofs - t_guard);
        LWB_RCV_PACKET();
        if(LWB_DATA_RCVD) {
          uint16_t new_period = glossy_payload[0];
          if(new_period != 0) {                      /* zero means no change */
            schedule.period = new_period;          /* extract updated period */
            schedule.n_slots = 0;                                  /* clear! */
          } /* else: all good, no need to change anything */
        } else {
          DEBUG_PRINT_INFO("2nd schedule missed");
        }
      }
    }
    
    /* --- COMMUNICATION ROUND ENDS --- */    
    /* time for other computations */
    
    if(IS_STATE_IDLE(&schedule)) {
      /* print out some stats (note: takes ~2ms to compose this string!) */
      DEBUG_PRINT_INFO("%s %lu T=%u n=%u tp=%u p=%u r=%u b=%u "
                       "u=%u per=%d snr=%ddbm dr=%d", 
                       lwb_sync_state_to_string[sync_state],
                       schedule.time, 
                       schedule.period * (1000 / LWB_PERIOD_SCALE), 
                       LWB_SCHED_N_SLOTS(&schedule), 
                       stats.t_proc_max,
                       stats.pck_cnt,
                       stats.relay_cnt, 
                       stats.bootstrap_cnt, 
                       stats.unsynced_cnt,
                       glossy_get_per(),
                       stats.glossy_snr,
                       stats.drift);
        
      /* poll the post process */
      if(post_proc) {
        process_poll(post_proc);
      }      
    #if LWB_CONF_T_PREPROCESS
      t_preprocess = LWB_CONF_T_PREPROCESS;
    #endif /* LWB_CONF_T_PREPROCESS */
    }
    /* erase the schedule (slot allocations only) */
    memset(&schedule.slot, 0, sizeof(schedule.slot));
        
    /* schedule the wakeup for the next round */
    LWB_LF_WAIT_UNTIL(t_ref_lf + (schedule.period * RTIMER_SECOND_LF) /
                      LWB_PERIOD_SCALE - 
                      (t_guard / RTIMER_HF_LF_RATIO) - t_preprocess);
  }

  PT_END(&lwb_pt);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(lwb_process, ev, data) 
{
  PROCESS_BEGIN();
  
#if !LWB_CONF_USE_XMEM
  /* pass the start addresses of the memory blocks holding the queues */
  fifo_init(&in_buffer, (uint16_t)in_buffer_mem);
  fifo_init(&out_buffer, (uint16_t)out_buffer_mem); 
#else  /* LWB_CONF_USE_XMEM */
  /* allocate memory for the message buffering (in ext. memory) */
  fifo_init(&in_buffer, xmem_alloc(LWB_CONF_IN_BUFFER_SIZE * 
                                   (LWB_CONF_MAX_DATA_PKT_LEN + 1)));
  fifo_init(&out_buffer, xmem_alloc(LWB_CONF_OUT_BUFFER_SIZE * 
                                    (LWB_CONF_MAX_DATA_PKT_LEN + 1)));   
#endif /* LWB_CONF_USE_XMEM */

#ifdef LWB_CONF_TASK_ACT_PIN
  PIN_CFG_OUT(LWB_CONF_TASK_ACT_PIN);
  PIN_CLR(LWB_CONF_TASK_ACT_PIN);
#endif /* LWB_CONF_TASK_ACT_PIN */
  
  PT_INIT(&lwb_pt); /* initialize the protothread */

  if(node_id == HOST_ID) {
    schedule_len = lwb_sched_init(&schedule);
  }

  /* start in 10ms */
  rtimer_clock_t t_wakeup = rtimer_now_lf() + RTIMER_SECOND_LF / 100;
  rtimer_id_t    rt_id    = LWB_CONF_LF_RTIMER_ID;
    
  if(node_id == HOST_ID) {
    /* update the global time and wait for the next full second */
    schedule.time = ((t_wakeup + RTIMER_SECOND_LF) / RTIMER_SECOND_LF);
    lwb_sched_set_time(schedule.time);
    t_wakeup = (rtimer_clock_t)schedule.time * RTIMER_SECOND_LF;
    rtimer_schedule(rt_id, t_wakeup, 0, lwb_thread_host);
  } else {
    rtimer_schedule(rt_id, t_wakeup, 0, lwb_thread_src);
  }

  /* instead of terminating the process here, use it for other tasks such as
   * external memory access */  
#if LWB_CONF_USE_XMEM
  while(1) {
    LWB_TASK_SUSPENDED;
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    LWB_TASK_RESUMED;
    /* is there anything to do? */
    if(xmem_task.op == 1) {           /* read operation */
      if(xmem_read(xmem_task.xmem_addr, LWB_CONF_MAX_DATA_PKT_LEN + 1, 
                   xmem_buffer)) {
        xmem_wait_until_ready();   /* wait for the data transfer to complete */
        /* trust the data in the memory, no need to check the length field */
        uint8_t len = *(xmem_buffer + LWB_CONF_MAX_DATA_PKT_LEN);
        memcpy(xmem_task.sram_ptr, xmem_buffer, len);
        if(xmem_task.notify) {
          *xmem_task.notify = len;
        }
      }
    } else if(xmem_task.op == 2) {    /* write operation */
      memcpy(xmem_buffer, xmem_task.sram_ptr, xmem_task.len);
      *(xmem_buffer + LWB_CONF_MAX_DATA_PKT_LEN) = xmem_task.len;
      xmem_wait_until_ready();      /* wait for ongoing transfer to complete */
      xmem_write(xmem_task.xmem_addr, LWB_CONF_MAX_DATA_PKT_LEN + 1,
                 xmem_buffer);
    } // else: no operation pending
    xmem_task.op = 0;
  }  
#endif /* LWB_CONF_USE_XMEM */

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
void
lwb_start(struct process *pre_lwb_proc, struct process *post_lwb_proc)
{
  pre_proc = pre_lwb_proc;
  post_proc = (struct process*)post_lwb_proc;
  printf("Starting '%s'\r\n", lwb_process.name);    
  printf(" pkt_len=%u data_len=%u slots=%u n_tx_d=%u n_tx_s=%u hops=%u\r\n", 
         LWB_CONF_MAX_PKT_LEN,
         LWB_CONF_MAX_DATA_PKT_LEN, 
         LWB_CONF_MAX_DATA_SLOTS, 
         LWB_CONF_TX_CNT_DATA, 
         LWB_CONF_TX_CNT_SCHED,
         LWB_CONF_MAX_HOPS);
  /* ceil the values (therefore + RTIMER_SECOND_HF / 1000 - 1) */
  printf(" slots [ms]: sched=%u data=%u cont=%u\r\n",
   (uint16_t)RTIMER_HF_TO_MS(LWB_CONF_T_SCHED + (RTIMER_SECOND_HF / 1000 - 1)),
   (uint16_t)RTIMER_HF_TO_MS(LWB_CONF_T_DATA + (RTIMER_SECOND_HF / 1000 - 1)),
   (uint16_t)RTIMER_HF_TO_MS(LWB_CONF_T_CONT + (RTIMER_SECOND_HF / 1000 - 1)));
  process_start(&lwb_process, NULL);
}
/*---------------------------------------------------------------------------*/

#endif /* LWB_VERSION */