#include <nchan_module.h>

#include <assert.h>
#include "store.h"
#include "shmem.h"
#include "ipc.h"
#include "ipc-handlers.h"
#include "store-private.h"
#include "../spool.h"


#include <store/redis/store.h>
#include <subscribers/memstore_redis.h>
#include <subscribers/memstore_multi.h>


typedef struct {
  ngx_event_t                     gc_timer;
  nchan_llist_timed_t            *gc_head;
  nchan_llist_timed_t            *gc_tail;
  nchan_store_channel_head_t      unbuffered_dummy_chanhead;
  store_channel_head_shm_t        dummy_shared_chaninfo;
  nchan_store_channel_head_t     *hash;
  
  ngx_int_t                       workers;
  
#if FAKESHARD
  ngx_int_t                       fake_slot;
#endif
} memstore_data_t;

static void init_mpt(memstore_data_t *m) {
  ngx_memzero(&m->gc_timer, sizeof(m->gc_timer));
  m->gc_head = NULL;
  m->gc_tail = NULL;
  ngx_memzero(&m->unbuffered_dummy_chanhead, sizeof(nchan_store_channel_head_t));
  m->unbuffered_dummy_chanhead.id.data= (u_char *)"unbuffered fake";
  m->unbuffered_dummy_chanhead.id.len=15;
  m->unbuffered_dummy_chanhead.owner = memstore_slot();
  m->unbuffered_dummy_chanhead.slot = memstore_slot();
  m->unbuffered_dummy_chanhead.status = READY;
  m->unbuffered_dummy_chanhead.min_messages = 0;
  m->unbuffered_dummy_chanhead.max_messages = (ngx_uint_t )-1;
  m->unbuffered_dummy_chanhead.shared = &m->dummy_shared_chaninfo;
}

#if FAKESHARD

static memstore_data_t  mdata[MAX_FAKE_WORKERS];
static memstore_data_t fake_default_mdata;
memstore_data_t *mpt = &fake_default_mdata;

ngx_int_t memstore_slot() {
  return mpt->fake_slot;
}

#else

static memstore_data_t  mdata;
memstore_data_t *mpt = &mdata;


ngx_int_t memstore_slot() {
  return ngx_process_slot;
}

#endif


static shmem_t         *shm = NULL;
static shm_data_t      *shdata = NULL;
static ipc_t           *ipc;

shmem_t *nchan_memstore_get_shm(void){
  return shm;
}

ipc_t *nchan_memstore_get_ipc(void){
  return ipc;
}

#define CHANNEL_HASH_FIND(id_buf, p)    HASH_FIND( hh, mpt->hash, (id_buf)->data, (id_buf)->len, p)
#define CHANNEL_HASH_ADD(chanhead)      HASH_ADD_KEYPTR( hh, mpt->hash, (chanhead->id).data, (chanhead->id).len, chanhead)
#define CHANNEL_HASH_DEL(chanhead)      HASH_DEL( mpt->hash, chanhead)

#undef uthash_malloc
#undef uthash_free
#define uthash_malloc(sz) ngx_alloc(sz, ngx_cycle->log)
#define uthash_free(ptr,sz) ngx_free(ptr)

#define STR(buf) (buf)->data, (buf)->len
#define BUF(buf) (buf)->pos, ((buf)->last - (buf)->pos)

#define NCHAN_DEFAULT_SUBSCRIBER_POOL_SIZE (5 * 1024)
#define NCHAN_DEFAULT_CHANHEAD_CLEANUP_INTERVAL 1000
#define NCHAN_CHANHEAD_EXPIRE_SEC 1
#define NCHAN_NOBUFFER_MSG_EXPIRE_SEC 10



//#define DEBUG_LEVEL NGX_LOG_WARN
#define DEBUG_LEVEL NGX_LOG_DEBUG


#if FAKESHARD

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "MEMSTORE:(fake)%02i: " fmt, memstore_slot(), ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "MEMSTORE:(fake)%02i: " fmt, memstore_slot(), ##args)

static nchan_llist_timed_t *fakeprocess_top = NULL;
void memstore_fakeprocess_push(ngx_int_t slot) {
  assert(slot < MAX_FAKE_WORKERS);
  nchan_llist_timed_t *link = ngx_calloc(sizeof(*fakeprocess_top), ngx_cycle->log);
  link->data = (void *)slot;
  link->time = ngx_time();
  link->next = fakeprocess_top;
  if(fakeprocess_top != NULL) {
    fakeprocess_top->prev = link;
  }
  fakeprocess_top = link;
  //DBG("push fakeprocess %i onto stack", slot);
  mpt = &mdata[slot];
}

ngx_int_t memstore_fakeprocess_pop(void) {
  nchan_llist_timed_t   *next;
  if(fakeprocess_top == NULL) {
    DBG("can't pop empty fakeprocess stack");
    return 0;
  }
  next = fakeprocess_top->next;
  ngx_free(fakeprocess_top);
  if(next == NULL) {
    DBG("can't pop last item off of fakeprocess stack");
    return 0;
  }
  //DBG("pop fakeprocess to return to %i", (ngx_int_t)next->data);
  next->prev = NULL;
  fakeprocess_top = next;
  mpt = &mdata[(ngx_int_t )fakeprocess_top->data];
  return 1;
}

void memstore_fakeprocess_push_random(void) {
  return memstore_fakeprocess_push(rand() % MAX_FAKE_WORKERS);
}

#else

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "MEMSTORE:%02i: " fmt, memstore_slot(), ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "MEMSTORE:%02i: " fmt, memstore_slot(), ##args)

#endif

static ngx_int_t is_multi_id(ngx_str_t *id);

ngx_int_t memstore_channel_owner(ngx_str_t *id) {
  ngx_int_t       h;
  //multi is always self-owned
  if(is_multi_id(id)) {
    return memstore_slot();
  }
  
  h = ngx_crc32_short(id->data, id->len);
#if FAKESHARD
  #ifdef ONE_FAKE_CHANNEL_OWNER
  h++; //just to avoid the unused variable warning
  return ONE_FAKE_CHANNEL_OWNER;
  #else
  return h % MAX_FAKE_WORKERS;
  #endif
#else
  ngx_int_t       i, slot;
  i = h % shdata->max_workers;
  slot = shdata->procslot[i];
  if(slot == NCHAN_INVALID_SLOT) {
    ERR("something went wrong, the channel owner is invalid. i: %i h: %i, max: %i", i, h, shdata->max_workers);
    return NCHAN_INVALID_SLOT;
  }
  return slot;
#endif
}



#if NCHAN_MSG_LEAK_DEBUG

nchan_msg_t *msgdebug_head = NULL;

void msg_debug_add(nchan_msg_t *msg) {
  //ensure this message is present only once
  nchan_msg_t      *cur;
  shmtx_lock(shm);
  for(cur = msgdebug_head; cur != NULL; cur = cur->dbg_next) {
    assert(cur != msg);
  }
  
  if(msgdebug_head == NULL) {
    msg->dbg_next = NULL;
    msg->dbg_prev = NULL;
  }
  else {
    msg->dbg_next = msgdebug_head;
    msg->dbg_prev = NULL;
    assert(msgdebug_head->dbg_prev == NULL);
    msgdebug_head->dbg_prev = msg;
  }
  msgdebug_head = msg;
  shmtx_unlock(shm);
}
void msg_debug_remove(nchan_msg_t *msg) {
  nchan_msg_t *prev, *next;
  shmtx_lock(shm);
  prev = msg->dbg_prev;
  next = msg->dbg_next;
  if(msgdebug_head == msg) {
    assert(msg->dbg_prev == NULL);
    if(next) {
      next->dbg_prev = NULL;
    }
    msgdebug_head = next;
  }
  else {
    if(prev) {
      prev->dbg_next = next;
    }
    if(next) {
      next->dbg_prev = prev;
    }
  }
  
  msg->dbg_next = NULL;
  msg->dbg_prev = NULL;
  shmtx_unlock(shm);
}
void msg_debug_assert_isempty(void) {
  assert(msgdebug_head == NULL);
}
#endif


static ngx_int_t chanhead_messages_gc(nchan_store_channel_head_t *ch);

static void nchan_store_chanhead_gc_timer_handler(ngx_event_t *);

static ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data) {
  shm_data_t         *d;
  ngx_int_t           i;
  if(data) { //zone being passed after restart
    zone->data = data;
    ERR("reattached shm data at %p", data);
  }
  else {
    shm_init(shm);
    
    if((d = shm_calloc(shm, sizeof(*d), "root shared data")) == NULL) {
      return NGX_ERROR;
    }
    
    zone->data = d;
    shdata = d;
    shdata->rlch = NULL;
    shdata->max_workers = NGX_CONF_UNSET;
    for(i=0; i< NGX_MAX_PROCESSES; i++) {
      shdata->procslot[i]=NCHAN_INVALID_SLOT;
    }
    ERR("Shm created with data at %p", d);
  }
  return NGX_OK;
}

static store_message_t *create_shared_message(nchan_msg_t *m, ngx_int_t msg_already_in_shm);
static ngx_int_t chanhead_push_message(nchan_store_channel_head_t *ch, store_message_t *msg);

void reload_msgs(void) {
  nchan_msg_t                 *cur;
  ngx_str_t                   *chid;
  nchan_store_channel_head_t  *ch;
  nchan_loc_conf_t             cf;
  ngx_int_t                    owner;
  store_message_t             *smsg;
  nchan_reloading_channel_t   *rlch;
  
  ERR("time to reload? shdata: %p, rlch: %p", shdata, shdata == NULL ? NULL : shdata->rlch);
  
  shmtx_lock(shm);
  for(rlch = shdata->rlch; rlch != NULL; rlch = rlch->next) {
    chid = &rlch->id;
    owner = memstore_channel_owner(chid);
    ERR("serialized channel %p %V", rlch, chid);
    if(owner == memstore_slot()) {
      cf.use_redis  =   rlch->use_redis;
      cf.min_messages = rlch->min_messages;
      cf.max_messages = rlch->max_messages;
      
      ch = nchan_memstore_get_chanhead(chid, &cf);
      assert(ch);
      
      ERR("got chanhead %p for id %V", ch, chid);
      
      for(cur = rlch->msgs; cur != NULL; cur = cur->reload_next) {
        assert(ch->shared);
        
        if((smsg = create_shared_message(cur, 1)) == NULL) {
          ERR("can't allocate message for reloading. stop trying.");
          return;
        }
        
        chanhead_push_message(ch, smsg);
        ERR("Added message %p (%V) to %V", smsg, msgid_to_str(&smsg->msg->id), chid);
      }
      
      shm_free_immutable_string(shm, chid);
    }
  }
  shmtx_unlock(shm);
}


static ngx_int_t nchan_store_init_worker(ngx_cycle_t *cycle) {
  ngx_core_conf_t    *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  ngx_int_t           workers = ccf->worker_processes;
  ngx_int_t           i;
  ngx_atomic_t       *procslot;
  
#if FAKESHARD
  for(i = 0; i < MAX_FAKE_WORKERS; i++) {
  memstore_fakeprocess_push(i);
#endif
  
  
  init_mpt(mpt);
  
  if(mpt->gc_timer.handler == NULL) {
    mpt->gc_timer.handler=&nchan_store_chanhead_gc_timer_handler;
    mpt->gc_timer.log=ngx_cycle->log;
  }

#if FAKESHARD
  memstore_fakeprocess_pop();
  }
#endif

  ipc_start(ipc, cycle);
  
  DBG("init memstore worker pid:%i slot:%i max workers :%i or %i", ngx_pid, memstore_slot(), shdata->max_workers, workers);

  if(shdata->max_workers != workers) {
    DBG("update number of workers from %i to %i", shdata->max_workers, workers);
    shdata->max_workers = workers;
  }
  
  procslot = shdata->procslot;
  for(i = 0; i < NGX_MAX_PROCESSES; i++) {
    if(ngx_atomic_cmp_set(&procslot[i], NCHAN_INVALID_SLOT, ngx_process_slot)) {
      DBG("set procslot %i to %i", i, ngx_process_slot);
      break;
    }
  }
  
  mpt->workers = workers;
  
  if(i >= workers) {
    //we're probably reloading or something
    ERR("that was a reload just now");
  }
  
  //reload_msgs();
  
  DBG("shm: %p, shdata: %p", shm, shdata);
  return NGX_OK;
}


static void spooler_add_handler(channel_spooler_t *spl, subscriber_t *sub, void *privdata) {
  nchan_store_channel_head_t   *head = (nchan_store_channel_head_t *)privdata;
  head->sub_count++;
  head->channel.subscribers++;
  if(sub->type == INTERNAL) {
    head->internal_sub_count++;
    if(head->shared) {
      assert(head->status == READY || head->status == STUBBED);
      ngx_atomic_fetch_add(&head->shared->internal_sub_count, 1);
    }
  }
  else {
    if(head->shared) {
      assert(head->status == READY || head->status == STUBBED);
      ngx_atomic_fetch_add(&head->shared->sub_count, 1);
    }
    if(head->use_redis) {
      nchan_store_redis_fakesub_add(&head->id, 1);
    }
    
    if(head->multi) {
      ngx_int_t     i, max = head->multi_count;
      subscriber_t *sub;
      for(i = 0; i < max; i++) {
        sub = head->multi[i].sub;
        sub->fn->notify(sub, NCHAN_SUB_MULTI_NOTIFY_ADDSUB, (void *)1);
      }
    }
    
  }
  head->last_subscribed = ngx_time();
  if(head->shared) {
    assert(head->status == READY || head->status == STUBBED);
    head->shared->last_seen = ngx_time();
  }
  assert(head->sub_count >= head->internal_sub_count);
}

static void spooler_bulk_dequeue_handler(channel_spooler_t *spl, subscriber_type_t type, ngx_int_t count, void *privdata) {
  nchan_store_channel_head_t   *head = (nchan_store_channel_head_t *)privdata;
  if (type == INTERNAL) {
    //internal subscribers are *special* and don't really count
    head->internal_sub_count -= count;
    if(head->shared) {
      ngx_atomic_fetch_add(&head->shared->internal_sub_count, -count);
    }
  }
  else {
    if(head->shared){
      ngx_atomic_fetch_add(&head->shared->sub_count, -count);
    }
    /*
    else if(head->shared == NULL) {
      assert(head->shutting_down == 1);
    }
    */
    if(head->use_redis) {
      nchan_store_redis_fakesub_add(&head->id, -count);
    }
    
    if(head->multi) {
      ngx_int_t     i, max = head->multi_count;
      subscriber_t *sub;
      for(i = 0; i < max; i++) {
        sub = head->multi[i].sub;
        sub->fn->notify(sub, NCHAN_SUB_MULTI_NOTIFY_ADDSUB, (void *)-count);
      }
    }
  }
  head->sub_count -= count;
  head->channel.subscribers = head->sub_count - head->internal_sub_count;
  assert(head->sub_count >= 0);
  assert(head->internal_sub_count >= 0);
  assert(head->channel.subscribers >= 0);
  assert(head->sub_count >= head->internal_sub_count);
  if(head->sub_count == 0 && head->foreign_owner_ipc_sub == NULL) {
    chanhead_gc_add(head, "sub count == 0 after spooler dequeue");
  }
}

static ngx_int_t start_chanhead_spooler(nchan_store_channel_head_t *head) {
  start_spooler(&head->spooler, &head->id, &head->status, &nchan_store_memory);
  head->spooler.fn->set_add_handler(&head->spooler, spooler_add_handler, head);
  head->spooler.fn->set_bulk_dequeue_handler(&head->spooler, spooler_bulk_dequeue_handler, head);
  return NGX_OK;
}

ngx_int_t memstore_ready_chanhead_unless_stub(nchan_store_channel_head_t *head) {
  if(head->stub) {
    head->status = STUBBED;
  }
  else {
    head->status = READY;
    head->spooler.fn->handle_channel_status_change(&head->spooler);
  }
  return NGX_OK;
}

ngx_int_t memstore_ensure_chanhead_is_ready(nchan_store_channel_head_t *head) {
  ngx_int_t                      owner = head->owner;
  nchan_loc_conf_t               cf;
  ngx_int_t                      i;
  if(head == NULL) {
    return NGX_OK;
  }

  DBG("ensure chanhead ready: chanhead %p, status %i, foreign_ipc_sub:%p", head, head->status, head->foreign_owner_ipc_sub);
  if(head->status == INACTIVE) {//recycled chanhead
    chanhead_gc_withdraw(head, "readying INACTIVE");
  }
  if(!head->spooler.running) {
    DBG("ensure chanhead ready: Spooler for channel %p %V wasn't running. start it.", head, &head->id);
    start_chanhead_spooler(head);
  }
  
  for(i=0; i< head->multi_count; i++) {
    if(head->multi[i].sub == NULL) {
      if(memstore_multi_subscriber_create(head, i) == NULL) { //stores and enqueues automatically
        ERR("can't create multi subscriber for channel");
      }
    }
  }
  
  if(owner != memstore_slot()) {
    if(head->foreign_owner_ipc_sub == NULL && head->status != WAITING) {
      cf.min_messages = head->min_messages;
      cf.max_messages = head->max_messages;
      cf.use_redis = head->use_redis;
      DBG("ensure chanhead ready: request for %V from %i to %i", &head->id, memstore_slot(), owner);
      head->status = WAITING;
      memstore_ipc_send_subscribe(owner, &head->id, head, &cf);
    }
    else if(head->foreign_owner_ipc_sub != NULL && head->status == WAITING) {
      DBG("ensure chanhead ready: subscribe request for %V from %i to %i", &head->id, memstore_slot(), owner);
      memstore_ready_chanhead_unless_stub(head);
    }
  }
  else {
    if(head->use_redis && head->status != READY) {
      if(head->redis_sub == NULL) {
        head->redis_sub = memstore_redis_subscriber_create(head);
        nchan_store_redis.subscribe(&head->id, head->redis_sub, NULL, NULL);
        head->status = WAITING;
      }
      else {
        if(head->redis_sub->enqueued) {
          memstore_ready_chanhead_unless_stub(head);
        }
        else {
          head->status = WAITING;
        }
      }
    }
    else {
      memstore_ready_chanhead_unless_stub(head);
    }
  }
  return NGX_OK;
}


static ngx_int_t is_multi_id(ngx_str_t *id) {
  u_char         *cur = id->data;
  return (cur[0] == 'm' && cur[1] == '/' && cur[2] == NCHAN_MULTI_SEP_CHR);
}

static ngx_int_t parse_multi_id(ngx_str_t *id, ngx_str_t ids[]) {
  ngx_int_t       n = 0;
  u_char         *cur = id->data;
  u_char         *last = cur + id->len;
  u_char         *sep;
  
  if(is_multi_id(id)) {
    cur += 3;
    while((sep = ngx_strlchr(cur, last, NCHAN_MULTI_SEP_CHR)) != NULL) {
      ids[n].data=cur;
      ids[n].len = sep - cur;
      cur = sep + 1;
      n++;
    }
    return n;
  }
  return 0;
}

static nchan_store_channel_head_t *chanhead_memstore_create(ngx_str_t *channel_id, nchan_loc_conf_t *cf) {
  nchan_store_channel_head_t   *head;
  ngx_int_t                     owner = memstore_channel_owner(channel_id);
  ngx_str_t                     ids[NCHAN_MEMSTORE_MULTI_MAX];
  ngx_int_t                     i, n = 0;
  
  head=ngx_alloc(sizeof(*head) + sizeof(u_char)*(channel_id->len), ngx_cycle->log);
  
  if(head == NULL) {
    ERR("can't allocate memory for (new) chanhead");
    return NULL;
  }
  head->slot = memstore_slot();
  head->owner = owner;
  head->shutting_down = 0;
  
  if(cf) {
    head->stub = 0;
    head->use_redis = cf->use_redis;
    head->redis_sub = NULL;
  }
  else {
    head->stub = 1;
  }
  
  if(head->slot == owner) {
    if((head->shared = shm_alloc(shm, sizeof(*head->shared), "channel shared data")) == NULL) {
      ERR("can't allocate shared memory for (new) chanhead");
      return NULL;
    }
    head->shared->sub_count = 0;
    head->shared->internal_sub_count = 0;
    head->shared->total_message_count = 0;
    head->shared->stored_message_count = 0;
    head->shared->last_seen = ngx_time();
  }
  else {
    head->shared = NULL;
  }
  
  //no lock needed, no one else knows about this chanhead yet.
  head->id.len = channel_id->len;
  head->id.data = (u_char *)&head[1];
  ngx_memcpy(head->id.data, channel_id->data, channel_id->len);
  head->sub_count=0;
  head->internal_sub_count=0;
  head->status = NOTREADY;
  head->msg_last = NULL;
  head->msg_first = NULL;
  head->foreign_owner_ipc_sub = NULL;
  head->last_subscribed = 0;
  
  head->multi=NULL;
  head->multi_count = 0;
  head->multi_waiting = 0;
  
  //set channel
  ngx_memcpy(&head->channel.id, &head->id, sizeof(ngx_str_t));
  head->channel.messages = 0;
  head->channel.subscribers = 0;
  head->channel.last_seen = ngx_time();
  head->min_messages = 0;
  head->max_messages = (ngx_int_t) -1;
  
  head->spooler.running=0;
  
  head->multi_waiting = 0;
  if((n = parse_multi_id(&head->id, ids)) > 0) {
    nchan_store_multi_t          *multi;
    if((multi = ngx_alloc(sizeof(*multi) * n, ngx_cycle->log)) == NULL) {
      ERR("can't allocate multi array for multi-channel %p", head);
    }
    
    head->latest_msgid.time = 0;
    head->latest_msgid.tagcount = n;
    head->oldest_msgid.time = 0;
    head->oldest_msgid.tagcount = n;
    for(i=0; i < n; i++) {
      head->latest_msgid.tag[n] = 0;
      head->oldest_msgid.tag[n] = 0;
      multi[i].id = ids[i];
      multi[i].sub = NULL;
    }
    
    head->multi_count = n;
    head->multi = multi;
    head->owner = head->slot; //multis are always self-owned
  }
  else {
    head->multi_count = 0;
    
    head->latest_msgid.time = 0;
    head->latest_msgid.tag[0] = 0;
    head->latest_msgid.tagcount = 1;
    
    head->oldest_msgid.time = 0;
    head->oldest_msgid.tag[0] = 0;
    head->oldest_msgid.tagcount = 1;
    
    head->multi = NULL;
  }
  
  start_chanhead_spooler(head);

  CHANNEL_HASH_ADD(head);
  
  return head;
}

nchan_store_channel_head_t * nchan_memstore_find_chanhead(ngx_str_t *channel_id) {
  nchan_store_channel_head_t     *head = NULL;
  CHANNEL_HASH_FIND(channel_id, head);
  if(head != NULL) {
    memstore_ensure_chanhead_is_ready(head);
  }
  return head;
}

nchan_store_channel_head_t *nchan_memstore_get_chanhead(ngx_str_t *channel_id, nchan_loc_conf_t *cf) {
  nchan_store_channel_head_t          *head;
  head = nchan_memstore_find_chanhead(channel_id);
  if(head==NULL) {
    head = chanhead_memstore_create(channel_id, cf);
    memstore_ensure_chanhead_is_ready(head);
  }
  return head;
}

ngx_int_t chanhead_gc_add(nchan_store_channel_head_t *head, const char *reason) {
  nchan_llist_timed_t         *chanhead_cleanlink;
  ngx_int_t                   slot = memstore_slot();
  DBG("Chanhead gc add %p %V: %s", head, &head->id, reason);
  chanhead_cleanlink = &head->cleanlink;
  if(!head->shutting_down) {
    assert(head->foreign_owner_ipc_sub == NULL); //we don't accept still-subscribed chanheads
  }
  if(head->slot != head->owner) {
    head->shared = NULL;
  }
  assert(head->slot == slot);
  if(head->status != INACTIVE) {
    chanhead_cleanlink->data=(void *)head;
    chanhead_cleanlink->time=ngx_time();
    chanhead_cleanlink->prev=mpt->gc_tail;
    if(mpt->gc_tail != NULL) {
      mpt->gc_tail->next=chanhead_cleanlink;
    }
    chanhead_cleanlink->next=NULL;
    mpt->gc_tail=chanhead_cleanlink;
    if(mpt->gc_head==NULL) {
      mpt->gc_head = chanhead_cleanlink;
    }
    head->status = INACTIVE;
  }
  else {
    DBG("gc_add chanhead %V: already added", &head->id);
  }

  //initialize gc timer
  if(! mpt->gc_timer.timer_set) {
    mpt->gc_timer.data=mpt->gc_head; //don't really care whre this points, so long as it's not null (for some debugging)
    ngx_add_timer(&mpt->gc_timer, NCHAN_DEFAULT_CHANHEAD_CLEANUP_INTERVAL);
  }

  return NGX_OK;
}

ngx_int_t chanhead_gc_withdraw(nchan_store_channel_head_t *chanhead, const char *reason) {
  //remove from gc list if we're there
  nchan_llist_timed_t    *cl;
  DBG("Chanhead gc withdraw %p %V: %s", chanhead, &chanhead->id, reason);
  
  if(chanhead->status == INACTIVE) {
    cl=&chanhead->cleanlink;
    if(cl->prev!=NULL)
      cl->prev->next=cl->next;
    if(cl->next!=NULL)
      cl->next->prev=cl->prev;

    if(mpt->gc_head==cl) {
      mpt->gc_head=cl->next;
    }
    if(mpt->gc_tail==cl) {
      mpt->gc_tail=cl->prev;
    }
    cl->prev = cl->next = NULL;
  }
  else {
    DBG("gc_withdraw chanhead %p (%V), but already inactive", chanhead, &chanhead->id);
  }
  
  return NGX_OK;
}


static ngx_str_t *msg_to_str(nchan_msg_t *msg) {
  static ngx_str_t str;
  ngx_buf_t *buf = msg->buf;
  if(ngx_buf_in_memory(buf)) {
    str.data = buf->start;
    str.len = buf->end - buf->start;
  }
  else {
    str.data= (u_char *)"{not in memory}";
    str.len =  15;
  }
  return &str;
}


static ngx_str_t *chanhead_msg_to_str(store_message_t *msg) {
  static ngx_str_t str;
  if (msg == NULL) {
    str.data=(u_char *)"{NULL}";
    str.len = 6;
    return &str;
  }
  else {
    return msg_to_str(msg->msg); //WHOA, shared space!
  }
}


ngx_int_t nchan_memstore_publish_generic(nchan_store_channel_head_t *head, nchan_msg_t *msg, ngx_int_t status_code, const ngx_str_t *status_line) {
  
  ngx_int_t          shared_sub_count = 0;
  
  if(head->shared) {
    if(!head->use_redis && !head->multi) {
      assert(head->status == READY || head->status == STUBBED);
    }
    shared_sub_count = head->shared->sub_count;
  }

  if(head==NULL) {
    if(msg) {
      DBG("tried publishing %V with a NULL chanhead", msgid_to_str(&msg->id));
    }
    else {
      DBG("tried publishing status %i msg with a NULL chanhead", status_code);
    }
    return NCHAN_MESSAGE_QUEUED;
  }

  if(msg) {
    DBG("tried publishing %V to chanhead %p (subs: %i)", msgid_to_str(&msg->id), head, head->sub_count);
    head->spooler.fn->respond_message(&head->spooler, msg);
    if(msg->temp_allocd) {
      ngx_free(msg);
    }
  }
  else {
    DBG("tried publishing status %i to chanhead %p (subs: %i)", status_code, head, head->sub_count);
    head->spooler.fn->respond_status(&head->spooler, status_code, status_line);
  }
    
  //TODO: be smarter about garbage-collecting chanheads
  if(head->owner == memstore_slot()) {
    //the owner is responsible for the chanhead and its interprocess siblings
    //when removed, said siblings will be notified via IPC
    chanhead_gc_add(head, "add owner chanhead after publish");
  }
  
  if(head->shared) {
    head->channel.subscribers = head->shared->sub_count;
  }
  
  return (shared_sub_count > 0) ? NCHAN_MESSAGE_RECEIVED : NCHAN_MESSAGE_QUEUED;
}

static ngx_int_t chanhead_messages_delete(nchan_store_channel_head_t *ch);

static void handle_chanhead_gc_queue(ngx_int_t force_delete) {
  nchan_llist_timed_t          *cur, *next;
  nchan_store_channel_head_t   *ch = NULL;
  ngx_int_t                     i;
  DBG("handling chanhead GC queue");
#if FAKESHARD
  ngx_int_t        ifv;
  for(ifv = 0; ifv < MAX_FAKE_WORKERS; ifv++) {
  memstore_fakeprocess_push(ifv);
#endif
  
  
  for(cur=mpt->gc_head ; cur != NULL; cur=next) {
    ch = (nchan_store_channel_head_t *)cur->data;
    next=cur->next;
    if(force_delete || ngx_time() - cur->time > NCHAN_CHANHEAD_EXPIRE_SEC) {
      
      
      if (ch->sub_count > 0) { //there are subscribers
        if(force_delete) {
          DBG("chanhead %p (%V) is still in use by %i subscribers. Try to delete it anyway.", ch, &ch->id, ch->sub_count);
          //ch->spooler.prepare_to_stop(&ch->spooler);
          ch->spooler.fn->respond_status(&ch->spooler, NGX_HTTP_GONE, &NCHAN_HTTP_STATUS_410);
          assert(ch->sub_count == 0);
        }
      }
      
      force_delete ? chanhead_messages_delete(ch) : chanhead_messages_gc(ch);
      
      if(ch->msg_first != NULL) {
        assert(ch->channel.messages != 0);
        DBG("chanhead %p (%V) is still storing %i messages.", ch, &ch->id, ch->channel.messages);
        break;
      }
      
      if (ch->sub_count == 0 && ch->channel.messages == 0) {
        //end this crazy channel
        assert(ch->msg_first == NULL);
        
        stop_spooler(&ch->spooler, 0);
        if(ch->owner == memstore_slot() && ch->shared) {
          shm_free(shm, ch->shared);
        }
        DBG("chanhead %p (%V) is empty and expired. DELETE.", ch, &ch->id);
        CHANNEL_HASH_DEL(ch);
        if(ch->multi) {
          for(i=0; i < ch->multi_count; i++) {
            if(ch->multi[i].sub) {
              ch->multi[i].sub->fn->dequeue(ch->multi[i].sub);
            }
          }
          ngx_free(ch->multi);
        }
        ngx_free(ch);
      }
      else if(force_delete) {
        //counldn't force-delete channel, even though we tried
        assert(0);
      }
    }
    else {
      break; //dijkstra probably hates this
    }
  }
  mpt->gc_head=cur;
  if (cur==NULL) { //we went all the way to the end
    mpt->gc_tail=NULL;
  }
  else {
    cur->prev=NULL;
  }
  
#if FAKESHARD
  memstore_fakeprocess_pop();
  }
  for(ifv = 0; ifv < MAX_FAKE_WORKERS; ifv++) {
    memstore_fakeprocess_push(ifv);
    memstore_fakeprocess_pop();
  }
#endif
}

static ngx_int_t handle_unbuffered_messages_gc(ngx_int_t force_delete);

static void nchan_store_chanhead_gc_timer_handler(ngx_event_t *ev) {
  nchan_llist_timed_t  *head = mpt->gc_head;
  handle_chanhead_gc_queue(0);
  handle_unbuffered_messages_gc(0);
  if (!(ngx_quit || ngx_terminate || ngx_exiting || head == NULL || mpt->unbuffered_dummy_chanhead.msg_first == NULL)) {
    DBG("re-adding chanhead gc event timer");
    ngx_add_timer(ev, NCHAN_DEFAULT_CHANHEAD_CLEANUP_INTERVAL);
  }
  else if(head == NULL) {
    DBG("chanhead gc queue looks empty, stop gc_queue handler");
  }
}

static ngx_int_t empty_callback(){
  return NGX_OK;
}

typedef struct {
  ngx_int_t       n;
  nchan_channel_t chinfo;
  callback_pt     cb;
  void           *pd;
} delete_multi_data_t;

static ngx_int_t delete_multi_callback_handler(ngx_int_t code, nchan_channel_t* chinfo, delete_multi_data_t *d) {
  assert(d->n >= 1);
  d->n--;
  
  if(chinfo) {
    d->chinfo.subscribers += chinfo->subscribers;
    if(d->chinfo.last_seen < chinfo->last_seen) {
      d->chinfo.last_seen = chinfo->last_seen;
    }
  }
  
  if(d->n == 0) {
    d->cb(code, &d->chinfo, d->pd);
    ngx_free(d);
  }
  
  return NGX_OK;
}

static ngx_int_t nchan_store_delete_channel(ngx_str_t *channel_id, callback_pt callback, void *privdata) {
  ngx_int_t                owner = memstore_channel_owner(channel_id);
  if(!is_multi_id(channel_id)) {
    if(memstore_slot() != owner) {
      memstore_ipc_send_delete(owner, channel_id, callback, privdata);
    }
    else {
      nchan_memstore_force_delete_channel(channel_id, callback, privdata);
    }
  }
  else {
    //delete a multichannel
    ngx_int_t              i, max, slot;
    delete_multi_data_t   *d = ngx_calloc(sizeof(*d), ngx_cycle->log);
    assert(d);
    //everyone might have this multi. broadcast the delete everywhere
#if FAKESHARD
    max = MAX_FAKE_WORKERS;
#else
    max = shdata->max_workers;
#endif
    d->n = max;
    d->cb = callback;
    d->pd = privdata;
    
    for(i=0; i < max; i++) {
#if FAKESHARD
      slot = i;
#else
      slot = shdata->procslot[i];
#endif
      if(slot == memstore_slot()) {
        nchan_memstore_force_delete_channel(channel_id, (callback_pt )delete_multi_callback_handler, d);
      }
      else {
        memstore_ipc_send_delete(slot, channel_id, (callback_pt )delete_multi_callback_handler, d);
      }
    }
    
  }
  return NGX_OK;
}

static ngx_int_t chanhead_delete_message(nchan_store_channel_head_t *ch, store_message_t *msg);

ngx_int_t nchan_memstore_force_delete_channel(ngx_str_t *channel_id, callback_pt callback, void *privdata) {
  nchan_store_channel_head_t    *ch;
  nchan_channel_t                chaninfo_copy;
  store_message_t               *msg = NULL;
  
  assert(memstore_channel_owner(channel_id) == memstore_slot());
  
  if(callback == NULL) {
    callback = empty_callback;
  }
  if((ch = nchan_memstore_find_chanhead(channel_id))) {
    chaninfo_copy.messages = ch->shared->stored_message_count;
    chaninfo_copy.subscribers = ch->shared->sub_count;
    chaninfo_copy.last_seen = ch->shared->last_seen;
    
    nchan_memstore_publish_generic(ch, NULL, NGX_HTTP_GONE, &NCHAN_HTTP_STATUS_410);
    callback(NGX_OK, &chaninfo_copy, privdata);
    //delete all messages
    while((msg = ch->msg_first) != NULL) {
      chanhead_delete_message(ch, msg);
    }
    chanhead_gc_add(ch, "forced delete");
  }
  else {
    callback(NGX_OK, NULL, privdata);
  }
  return NGX_OK;
}

static ngx_int_t nchan_store_find_channel(ngx_str_t *channel_id, callback_pt callback, void *privdata) {
  ngx_int_t                    owner = memstore_channel_owner(channel_id);
  nchan_store_channel_head_t  *ch;
  
  if(memstore_slot() == owner) {
    ch = nchan_memstore_find_chanhead(channel_id);
    callback(NGX_OK, ch != NULL ? &ch->channel : NULL , privdata);
  }
  else {
    memstore_ipc_send_get_channel_info(owner, channel_id, callback, privdata);
  }
  return NGX_OK;
}

//initialization
static ngx_int_t nchan_store_init_module(ngx_cycle_t *cycle) {

#if FAKESHARD

  shdata->max_workers = MAX_FAKE_WORKERS;
  
  ngx_int_t          i;
  memstore_data_t   *cur;
  for(i = 0; i < MAX_FAKE_WORKERS; i++) {
    cur = &mdata[i];
    cur->fake_slot = i;
  }
  
  memstore_fakeprocess_push(0);

#else

  ngx_core_conf_t    *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  
  shdata->max_workers = ccf->worker_processes;

#endif
  
  DBG("memstore init_module pid %i. ipc: %p", ngx_pid, ipc);

  //initialize our little IPC
  if(ipc == NULL) {
    ipc = ipc_create(cycle);
    ipc_set_handler(ipc, memstore_ipc_alert_handler);
  }
  ipc_open(ipc,cycle, shdata->max_workers);

  return NGX_OK;
}

static ngx_int_t nchan_store_init_postconfig(ngx_conf_t *cf) {
  nchan_main_conf_t     *conf = ngx_http_conf_get_module_main_conf(cf, nchan_module);
  ngx_str_t              name = ngx_string("memstore");
  if(conf->shm_size==NGX_CONF_UNSET_SIZE) {
    conf->shm_size=NCHAN_DEFAULT_SHM_SIZE;
  }
  shm = shm_create(&name, cf, conf->shm_size, initialize_shm, &nchan_module);
  return NGX_OK;
}

static void nchan_store_create_main_conf(ngx_conf_t *cf, nchan_main_conf_t *mcf) {
  mcf->shm_size=NGX_CONF_UNSET_SIZE;
}

/*
static void serialize_chanhead_msgs_for_reload(nchan_store_channel_head_t *ch) {
  nchan_reloading_channel_t     *sch;
  store_message_t               *cur, *next;
  nchan_msg_t                   *msg, *firstmsg, *lastmsg;
  
  if(ch->msg_first == NULL) {
    //empty
    return;
  }
  
  firstmsg = ch->msg_first->msg;
  lastmsg = NULL;
  if(firstmsg != NULL) {
    if((sch = shm_alloc(shm, sizeof(sch) + ch->id.len, "channel reloading data")) == NULL) {
      ERR("unable to allocate reloading-channel for msg reload");
      return;
    }
    
    sch->id.len = ch->id.len;
    sch->id.data = (u_char *)&sch[1];
    ngx_memcpy(sch->id.data, ch->id.data, ch->id.len);
    
    sch->min_messages = ch->min_messages;
    sch->max_messages = ch->max_messages;
    sch->use_redis = ch->use_redis;
    sch->msgs = firstmsg;
  }
  
  for(cur = ch->msg_first; cur != NULL; cur = next) {
    msg = cur->msg;
    if(lastmsg) {
      lastmsg->reload_next = msg;
    }
    
    lastmsg = msg;
    
    next = cur->next;
    ngx_free(cur);
  }
  
  if(ch->shared) {
    ch->shared->stored_message_count = 0;
  }
  ch->channel.messages = 0;
  
  if(firstmsg) {
    shmtx_lock(shm);
    
    lastmsg->reload_next = NULL;
    
    sch->prev = NULL;
    sch->next = shdata->rlch;
    shdata->rlch = sch;
    
    shmtx_unlock(shm);
  }
  
  ch->msg_first = NULL;
  ch->msg_last = NULL;
}
*/

static void nchan_store_exit_worker(ngx_cycle_t *cycle) {
  nchan_store_channel_head_t         *cur, *tmp;
  ngx_int_t                           i, my_procslot_index = NCHAN_INVALID_SLOT, procslot;
    
  ERR("exit worker %i  (slot %i)", ngx_pid, ngx_process_slot);
  
#if FAKESHARD
  for(i = 0; i < MAX_FAKE_WORKERS; i++) {
  memstore_fakeprocess_push(i);
#endif
  HASH_ITER(hh, mpt->hash, cur, tmp) {
    cur->shutting_down = 1;
    
    //serialize_chanhead_msgs_for_reload(cur);
    
    chanhead_gc_add(cur, "exit worker");
  }
  handle_chanhead_gc_queue(1);
  handle_unbuffered_messages_gc(1);
  
  if(mpt->gc_timer.timer_set) {
    ngx_del_timer(&mpt->gc_timer);
  }
#if FAKESHARD
  memstore_fakeprocess_pop();
  }
#endif
  
  //are there any workers waiting in the wings?
  //don't care if this is 'ineficient', it only happens once per worker per load
  for(i = 0; i < NGX_MAX_PROCESSES; i++) {
    procslot = shdata->procslot[i];
    if(i < mpt->workers && ngx_process_slot == i) {
      my_procslot_index = i;
    }
    
    if(i > shdata->max_workers 
     && my_procslot_index != NCHAN_INVALID_SLOT 
     && procslot != NCHAN_INVALID_SLOT) { 
      //we got a live one without a valid procslot, and we know where we are.
      DBG("swap procslot %i and %i (ngx_process slot %i to %i)", my_procslot_index, i, ngx_process_slot, procslot); 
      shdata->procslot[my_procslot_index] = procslot;
      shdata->procslot[i] = NCHAN_INVALID_SLOT;
      
      break;
    }
  }
  
  if(my_procslot_index != NCHAN_INVALID_SLOT && shdata->procslot[my_procslot_index] == ngx_process_slot) {
    //still our procslot. don't need it anymore
    DBG("don't need procslot %i anymore", my_procslot_index);
    shdata->procslot[my_procslot_index] = NCHAN_INVALID_SLOT;
  }
  
  ipc_close(ipc, cycle);
  ipc_destroy(ipc, cycle); //only for this worker...
  
  shm_destroy(shm); //just for this worker...
  
#if FAKESHARD
  while(memstore_fakeprocess_pop()) {  };
#endif
#if NCHAN_MSG_LEAK_DEBUG
  msg_debug_assert_isempty();
#endif
}

static void nchan_store_exit_master(ngx_cycle_t *cycle) {
  DBG("exit master from pid %i", ngx_pid);
  
  ipc_close(ipc, cycle);
  ipc_destroy(ipc, cycle);
#if FAKESHARD
  while(memstore_fakeprocess_pop()) {  };
 #endif
  shm_free(shm, shdata);
  shm_destroy(shm);
}

static ngx_int_t validate_chanhead_messages(nchan_store_channel_head_t *ch) {
  /*
  ngx_int_t              count = ch->channel.messages;
  ngx_int_t              rev_count = count;
  ngx_int_t              owner = memstore_channel_owner(&ch->id);
  store_message_t        *cur;
  
  if(memstore_slot() == owner) {
    assert(ch->shared->stored_message_count == ch->channel.messages);
  }
  //walk it forwards
  for(cur = ch->msg_first; cur != NULL; cur=cur->next){
    count--;
  }
  for(cur = ch->msg_last; cur != NULL; cur=cur->prev){
    rev_count--;
  }
  
  assert(count == 0);
  assert(rev_count == 0);
  */
  return NGX_OK;
}

static ngx_int_t chanhead_withdraw_message(nchan_store_channel_head_t *ch, store_message_t *msg) {
  //DBG("withdraw message %V from ch %p %V", msgid_to_str(&msg->msg->id), ch, &ch->id);
  validate_chanhead_messages(ch);
  if(msg->msg->refcount > 0) {
    ERR("trying to withdraw (remove) message %p with refcount %i", msg, msg->msg->refcount);
    return NGX_ERROR;
  }
  if(ch->msg_first == msg) {
    //DBG("first message removed");
    ch->msg_first = msg->next;
  }
  if(ch->msg_last == msg) {
    //DBG("last message removed");
    ch->msg_last = msg->prev;
  }
  if(msg->next != NULL) {
    //DBG("set next");
    msg->next->prev = msg->prev;
  }
  if(msg->prev != NULL) {
    //DBG("set prev");
    msg->prev->next = msg->next;
  }
  
  ch->channel.messages--;
  
  ngx_atomic_fetch_add(&ch->shared->stored_message_count, -1);
  
  if(ch->channel.messages == 0) {
    assert(ch->msg_first == NULL);
    assert(ch->msg_last == NULL);
  }
  validate_chanhead_messages(ch);
  return NGX_OK;
}

static ngx_int_t delete_withdrawn_message( store_message_t *msg ) {
  ngx_buf_t         *buf = msg->msg->buf;
  ngx_file_t        *f = buf->file;
  
  if(f != NULL) {
    if(f->fd != NGX_INVALID_FILE) {
      DBG("close fd %u ", f->fd);
      ngx_close_file(f->fd);
    }
    else {
      DBG("delete withdrawn fd invalid");
    }
    ngx_delete_file(f->name.data); // assumes string is zero-terminated, which required trickery during allocation
  }
  //DBG("free msg %p", msg);
#if NCHAN_MSG_LEAK_DEBUG  
  msg_debug_remove(msg->msg);
#endif
  
  ngx_memset(msg->msg, 0xFA, sizeof(*msg->msg)); //debug stuff
  shm_free(shm, msg->msg);
  
  ngx_memset(msg, 0xBC, sizeof(*msg)); //debug stuff
  ngx_free(msg);
  return NGX_OK;
}

static ngx_int_t chanhead_delete_message(nchan_store_channel_head_t *ch, store_message_t *msg) {
  validate_chanhead_messages(ch);
  if(chanhead_withdraw_message(ch, msg) == NGX_OK) {
    DBG("delete msg %V", msgid_to_str(&msg->msg->id));
    delete_withdrawn_message(msg);
  }
  else {
    ERR("failed to withdraw and delete message %V", msgid_to_str(&msg->msg->id));
  }
  validate_chanhead_messages(ch);
  return NGX_OK;
}

static ngx_int_t chanhead_messages_gc_custom(nchan_store_channel_head_t *ch, ngx_uint_t min_messages, ngx_uint_t max_messages) {
  validate_chanhead_messages(ch);
  store_message_t   *cur = ch->msg_first;
  store_message_t   *next = NULL;
  time_t             now = ngx_time();
  ngx_int_t          started_count, tried_count, deleted_count;
  DBG("chanhead_gc max %i min %i count %i", max_messages, min_messages, ch->channel.messages);
  
  started_count = ch->channel.messages;
  tried_count = 0;
  deleted_count = 0;
  
  //is the message queue too big?
  while(cur != NULL && ch->channel.messages > max_messages) {
    tried_count++;
    next = cur->next;
    if(cur->msg->refcount > 0) {
      DBG("msg %p refcount %i > 0", &cur->msg, cur->msg->refcount); //not a big deal
    }
    else {
      DBG("delete queue-too-big msg %V", msgid_to_str(&cur->msg->id));
      chanhead_delete_message(ch, cur);
      deleted_count++;
    }
    cur = next;
  }
  
  while(cur != NULL && ch->channel.messages > min_messages && now > cur->msg->expires) {
    tried_count++;
    next = cur->next;
    if(cur->msg->refcount > 0) {
      DBG("msg %p refcount %i > 0", &cur->msg, cur->msg->refcount);
    }
    else {
      DBG("delete msg %p");
      chanhead_delete_message(ch, cur);
      deleted_count++;
    }
    cur = next;
  }
  DBG("message GC results: started with %i, walked %i, deleted %i msgs", started_count, tried_count, deleted_count);
  validate_chanhead_messages(ch);
  return NGX_OK;
}

static ngx_int_t chanhead_messages_gc(nchan_store_channel_head_t *ch) {
  //DBG("messages gc for ch %p %V", ch, &ch->id);
  return chanhead_messages_gc_custom(ch, ch->min_messages, ch->max_messages);
}

static ngx_int_t chanhead_messages_delete(nchan_store_channel_head_t *ch) {
  chanhead_messages_gc_custom(ch, 0, 0);
  return NGX_OK;
}

static ngx_int_t handle_unbuffered_messages_gc(ngx_int_t force_delete) {
  nchan_store_channel_head_t         *ch = &mpt->unbuffered_dummy_chanhead;
  DBG("handling unbuffered messages GC queue");
  
  #if FAKESHARD
  ngx_int_t        i;
  for(i = 0; i < MAX_FAKE_WORKERS; i++) {
  memstore_fakeprocess_push(i);
  #endif
  if(!force_delete) {
    chanhead_messages_gc(ch);
  }
  else {
    chanhead_messages_delete(ch);
  }
  
  #if FAKESHARD
  memstore_fakeprocess_pop();
  }
  #endif
  return NGX_OK;
}

store_message_t *chanhead_find_next_message(nchan_store_channel_head_t *ch, nchan_msg_id_t *msgid, nchan_msg_status_t *status) {
  store_message_t      *cur, *first;
  DBG("find next message %V", msgid_to_str(msgid));
  if(ch == NULL) {
    *status = MSG_NOTFOUND;
    return NULL;
  }
  chanhead_messages_gc(ch);
  
  first = ch->msg_first;
  cur = ch->msg_last;
  
  if(cur == NULL) {
    if(msgid->time == 0) {
      *status = MSG_EXPECTED;
    }
    else {
      *status = MSG_NOTFOUND;
    }
    return NULL;
  }
  
  assert(msgid->tagcount == 1 && first->msg->id.tagcount == 1);
  if(msgid == NULL || (msgid->time < first->msg->id.time || (msgid->time == first->msg->id.time && msgid->tag[0] < first->msg->id.tag[0])) ) {
    DBG("found message %V", msgid_to_str(&first->msg->id));
    *status = MSG_FOUND;
    return first;
  }

  while(cur != NULL) {
    assert(cur->msg->id.tagcount == 1);
    DBG("cur: (chid: %V)  %V %V", &ch->id, msgid_to_str(&cur->msg->id), chanhead_msg_to_str(cur));
    
    if(msgid->time > cur->msg->id.time || (msgid->time == cur->msg->id.time && msgid->tag[0] >= cur->msg->id.tag[0])){
      if(cur->next != NULL) {
        *status = MSG_FOUND;
        DBG("found message %V", msgid_to_str(&cur->next->msg->id));
        return cur->next;
      }
      else {
        *status = MSG_EXPECTED;
        return NULL;
      }
    }
    cur=cur->prev;
  }
  //DBG("looked everywhere, not found");
  *status = MSG_NOTFOUND;
  return NULL;
}

typedef struct {
  subscriber_t                *sub;
  ngx_int_t                    channel_owner;
  nchan_store_channel_head_t  *chanhead;
  ngx_str_t                   *channel_id;
  nchan_msg_id_t               msg_id;
  callback_pt                  cb;
  void                        *cb_privdata;
  unsigned                     reserved:1;
  unsigned                     subbed:1;
  unsigned                     allocd:1;
} subscribe_data_t;

//static subscribe_data_t        static_subscribe_data;

static subscribe_data_t *subscribe_data_alloc(ngx_int_t owner) {
  subscribe_data_t            *d;
  //fuck it, just always allocate. we need to handle multis and shit too
  d = ngx_alloc(sizeof(*d), ngx_cycle->log);
  assert(d);
  d->allocd = 1;
  /*if(memstore_slot() != owner) {
    d = ngx_alloc(sizeof(*d), ngx_cycle->log);
    d->allocd = 1;
  }
  else {
    d = &static_subscribe_data;
    d->allocd = 0;
  }*/
  return d;
}

static void subscribe_data_free(subscribe_data_t *d) {
  if(d->allocd) {
    ngx_free(d);
  }
}

#define SUB_CHANNEL_UNAUTHORIZED 0
#define SUB_CHANNEL_AUTHORIZED 1
#define SUB_CHANNEL_NOTSURE 2

static ngx_int_t nchan_store_subscribe_sub_reserved_check(ngx_int_t channel_status, void* _, subscribe_data_t *d);
static ngx_int_t nchan_store_subscribe_continued(ngx_int_t channel_status, void* _, subscribe_data_t *d);

static ngx_int_t nchan_store_subscribe(ngx_str_t *channel_id, subscriber_t *sub, callback_pt callback, void *privdata) {
  ngx_int_t                    owner = memstore_channel_owner(channel_id);
  subscribe_data_t            *d = subscribe_data_alloc(sub->cf->use_redis ? -1 : owner);
  
  assert(d != NULL);
  assert(callback != NULL);
  
  d->channel_owner = owner;
  d->channel_id = channel_id;
  d->cb = callback;
  d->cb_privdata = privdata;
  d->sub = sub;
  d->subbed = 0;
  d->reserved = 0;
  d->msg_id = sub->last_msgid;
  
  if(sub->cf->authorize_channel) {
    sub->fn->reserve(sub);
    d->reserved = 1;
    if(memstore_slot() != owner) {
      memstore_ipc_send_does_channel_exist(owner, channel_id, (callback_pt )nchan_store_subscribe_sub_reserved_check, d);
    }
    else {
      nchan_store_subscribe_continued(SUB_CHANNEL_NOTSURE, NULL, d);
    }
  }
  else {
    nchan_store_subscribe_continued(SUB_CHANNEL_AUTHORIZED, NULL, d);
  }
  
  return NGX_OK;
}

static ngx_int_t nchan_store_subscribe_sub_reserved_check(ngx_int_t channel_status, void* _, subscribe_data_t *d) {
  if(d->sub->fn->release(d->sub, 0) == NGX_OK) {
    d->reserved = 0;
    return nchan_store_subscribe_continued(channel_status, _, d);
  }
  else {//don't go any further, the sub has been deleted
    assert(d->sub->reserved == 0);
    subscribe_data_free(d);
    return NGX_OK;
  }
}

static ngx_int_t redis_subscribe_channel_authcheck_callback(ngx_int_t status, void *ch, void *d) {
  nchan_channel_t    *channel = (nchan_channel_t *)ch;
  subscribe_data_t   *data = (subscribe_data_t *)d;
  ngx_int_t           channel_status;
  if(status == NGX_OK) {
    channel_status = channel == NULL ? SUB_CHANNEL_UNAUTHORIZED : SUB_CHANNEL_AUTHORIZED;
    nchan_store_subscribe_continued(channel_status, NULL, data);
  }
  else {
    //error!!
    subscribe_data_free(data);
  }
  return NGX_OK;
}

static ngx_int_t nchan_store_subscribe_continued(ngx_int_t channel_status, void* _, subscribe_data_t *d) {
  nchan_store_channel_head_t  *chanhead = NULL;
  //store_message_t             *chmsg;
  //nchan_msg_status_t           findmsg_status;
  
  ngx_int_t                      use_redis = d->sub->cf->use_redis;
  
  switch(channel_status) {
    case SUB_CHANNEL_AUTHORIZED:
      chanhead = nchan_memstore_get_chanhead(d->channel_id, d->sub->cf);
      break;
    
    case SUB_CHANNEL_UNAUTHORIZED:
      chanhead = NULL;
      break;
    
    case SUB_CHANNEL_NOTSURE:
      chanhead = nchan_memstore_find_chanhead(d->channel_id);
      if(chanhead == NULL) {
        if(use_redis) {
          nchan_store_redis.find_channel(d->channel_id, redis_subscribe_channel_authcheck_callback, d);
          return NGX_OK;
        }
      }
      break;
  }
  
  if (chanhead == NULL) {
    d->sub->fn->respond_status(d->sub, NGX_HTTP_FORBIDDEN, NULL);
    
    if(d->reserved) {
      d->sub->fn->release(d->sub, 0);
      d->reserved = 0;
    }
    
    //sub should be destroyed by now.
    
    d->sub = NULL; //debug
    d->cb(NGX_HTTP_NOT_FOUND, NULL, d->cb_privdata);
    subscribe_data_free(d);
    return NGX_OK;
  }
  
  d->chanhead = chanhead;

  if(d->reserved) {
    d->sub->fn->release(d->sub, 1);
    d->reserved = 0;
  }
  
  chanhead->spooler.fn->add(&chanhead->spooler, d->sub);

  subscribe_data_free(d);

  return NGX_OK;
}

static ngx_str_t empty_id_str = ngx_string("-");

static ngx_int_t nchan_store_async_get_message(ngx_str_t *channel_id, nchan_msg_id_t *msg_id, callback_pt callback, void *privdata);



typedef struct {
  nchan_msg_status_t  msg_status;
  nchan_msg_t        *msg;
  ngx_int_t           n;
  
  nchan_msg_id_t      wanted_msgid;
  ngx_int_t           getting;
  ngx_int_t           multi_count;
  
  callback_pt         cb;
  void               *privdata;
} get_multi_message_data_t;

typedef struct {
  ngx_int_t                   n;
  get_multi_message_data_t   *d;
} get_multi_message_data_single_t;

static ngx_int_t nchan_store_async_get_multi_message_callback(nchan_msg_status_t status, nchan_msg_t *msg, get_multi_message_data_single_t *sd) {
  
  get_multi_message_data_t   *d = sd->d;
  nchan_msg_t                 retmsg;
  int16_t                     uptag;
  
  d->getting--;
  
  switch(status) {
    case MSG_EXPECTED:
      DBG("multi[%i] of %i msg EXPECTED", sd->n, d->multi_count);
      break;
    case MSG_NOTFOUND:
      DBG("multi[%i] of %i msg NOTFOUND", sd->n, d->multi_count);
      break;
    case MSG_FOUND:
      DBG("multi[%i] of %i msg FOUND %V %p", sd->n, d->multi_count, msgid_to_str(&msg->id), msg);
      break;
    default:
      assert(0);
  }
  
  
  if(d->msg_status == MSG_PENDING) {
    DBG("first response msg %V (n:%i) %p, saved", msg ? msgid_to_str(&msg->id) : &empty_id_str, sd->n, d->msg);
    d->msg_status = status;
    d->msg = msg;
    if(msg) {
      msg_reserve(msg, "get multi msg");
    }
    d->n = sd->n;
  }
  else if(msg) {
    DBG("prev best response: %V (n:%i) %p", d->msg ? msgid_to_str(&d->msg->id) : &empty_id_str, d->n, d->msg);
    if( d->msg == NULL
     || msg->id.time < d->msg->id.time 
     || (msg->id.time == d->msg->id.time && msg->id.tag[0] < d->msg->id.tag[0]) 
     || (msg->id.time == d->msg->id.time && msg->id.tag[0] == d->msg->id.tag[0] && sd->n < d->n) ) {
      DBG("got a better response %V (n:%i), replace.", msgid_to_str(&msg->id), sd->n);
      if(d->msg) {
        msg_release(d->msg, "get multi msg");
      }
      d->msg_status = status;
      d->msg = msg;
      msg_reserve(msg, "get multi msg");
      d->n = sd->n;
    }
    else {
      DBG("got a worse response %V (n:%i), keep prev.", msgid_to_str(&msg->id), sd->n);
    }
  }
  else if(d->msg == NULL && d->msg_status != MSG_EXPECTED) {
    d->msg_status = status;
  }
  
  if(d->getting == 0) {
    //got all the messages we wanted
    if(d->msg) {
      //retmsg = ngx_alloc(sizeof(*retmsg), ngx_cycle->log);
      //assert(retmsg);
      DBG("ready to respond with msg %V (n:%i) %p", msgid_to_str(&d->msg->id), d->n, d->msg);
      ngx_memcpy(&retmsg, d->msg, sizeof(retmsg));
      retmsg.shared = 0;
      retmsg.temp_allocd = 0;
      
      retmsg.prev_id = d->wanted_msgid;
      //TODO: some kind of missed-message check
      
      if(d->wanted_msgid.time == retmsg.id.time) {
        uptag = retmsg.id.tag[0];
        retmsg.id = d->wanted_msgid;
        retmsg.id.tag[d->n] = uptag;
        retmsg.id.tagactive = d->n;
      }
      else {
        retmsg.id.tagcount = d->multi_count;
        nchan_set_msg_id_multi_tag(&retmsg.id, 0, d->n, -1);
        retmsg.id.tagactive = d->n;
      }
      
      DBG("respond msg id transformed into %p %V", &retmsg, msgid_to_str(&retmsg.id));
      
      d->cb(d->msg_status, &retmsg, d->privdata);
      msg_release(d->msg, "get multi msg");
    }
    else {
      d->cb(d->msg_status, NULL, d->privdata);
    }
    ngx_free(d);
  }
  
  ngx_free(sd);
  return NGX_OK;
}

static ngx_int_t nchan_store_async_get_multi_message(ngx_str_t *chid, nchan_msg_id_t *msg_id, callback_pt callback, void *privdata) {
  
  nchan_store_channel_head_t  *chead;
  nchan_store_multi_t         *multi = NULL;
  
  ngx_int_t                    n;
  uint8_t                      want[NCHAN_MEMSTORE_MULTI_MAX];
  ngx_str_t                    ids[NCHAN_MEMSTORE_MULTI_MAX];
  nchan_msg_id_t               req_msgid[NCHAN_MEMSTORE_MULTI_MAX] = {{0}};
  
  nchan_msg_id_t              *lastid;
  ngx_str_t                   *getmsg_chid;
  
  
  time_t                       time = msg_id->time;
  ngx_int_t                    i;
  
  get_multi_message_data_t    *d = ngx_alloc(sizeof(*d), ngx_cycle->log);
  assert(d);
  d->cb = callback;
  d->privdata = privdata;
  d->msg_status = MSG_PENDING;
  d->msg = NULL;
  d->n = -1;
  d->getting = 0;
  d->wanted_msgid = *msg_id;
  
  if((chead = nchan_memstore_get_chanhead(chid, NULL)) != NULL) {
    n = chead->multi_count;
    multi = chead->multi;
  }
  else {
    n = parse_multi_id(chid, ids);
  }
  
  //init loop
  for(i = 0; i < n; i++) {
    want[i] = 0;
  }
  
  d->multi_count = n;
  
  DBG("get multi msg %V (count: %i)", msgid_to_str(msg_id), n);
  
  
  if(msg_id->time == 0) {
    for(i = 0; i < n; i++) {
      req_msgid[i].time = 0;
      req_msgid[i].tag[0] = 0;
      req_msgid[i].tagcount = 1;
      want[i] = 1;
    }
    d->getting = n;
    DBG("want all msgs");
  }
  else {
    //nchan_decode_msg_id_multi_tag(tag, n, decoded_tags);
    
    //what msgids do we want?
    for(i = 0; i < n; i++) {
      req_msgid[i].time = time;
      if(msg_id->tag[i] == -1) {
        req_msgid[i].time --;
        req_msgid[i].tag[0] = 32767; //eeeeeh this is bad. but it's good enough.
      }
      else {
        req_msgid[i].tag[0] = msg_id->tag[i];
      }
      req_msgid[i].tagcount = 1;
      DBG("might want msgid %V from chan_index %i", msgid_to_str(&req_msgid[i]), i);
    }
    
    //what do we need to fetch?
    for(i = 0; i < n; i++) {
      if(multi) {
        lastid = &multi[i].sub->last_msgid;
        DBG("chan index %i last id %V", i, msgid_to_str(lastid));
        if(lastid->time == 0 
         || lastid->time > req_msgid[i].time
         || (lastid->time == req_msgid[i].time && lastid->tag[0] >= req_msgid[i].tag[0])) {
          want[i]=1;
          d->getting++;
          DBG("want %i", i);
        }
        else {
          DBG("Do not want %i", i);
        }
      }
      else {
        DBG("nomulti (lastid), want %i", i);
        want[i]=1;
        d->getting++;
      }
    }
  }
  
  //do it.
  for(i = 0; i < n; i++) {
    if(want[i]) {
      get_multi_message_data_single_t  *sd = ngx_alloc(sizeof(*sd), ngx_cycle->log);
      assert(sd);
      
      sd->d = d;
      sd->n = i;
      
      getmsg_chid = (multi == NULL) ? &ids[i] : &multi[i].id;
      DBG("get message from %V (n: %i) %V", getmsg_chid, i, msgid_to_str(&req_msgid[i]));
      nchan_store_async_get_message(getmsg_chid, &req_msgid[i], (callback_pt )nchan_store_async_get_multi_message_callback, sd);
    }
  }
  
  return NGX_OK;
}

static ngx_int_t nchan_store_async_get_message(ngx_str_t *channel_id, nchan_msg_id_t *msg_id, callback_pt callback, void *privdata) {
  store_message_t             *chmsg;
  ngx_int_t                    owner = memstore_channel_owner(channel_id);
  subscribe_data_t            *d; 
  nchan_msg_status_t           findmsg_status;
  nchan_store_channel_head_t  *chead;
  
  ngx_int_t                    use_redis = 0;
  
  if(callback==NULL) {
    ERR("no callback given for async get_message. someone's using the API wrong!");
    return NGX_ERROR;
  }
  
  if(is_multi_id(channel_id)) {
    return nchan_store_async_get_multi_message(channel_id, msg_id, callback, privdata);
  }
  
  chead = nchan_memstore_find_chanhead(channel_id);
  d = subscribe_data_alloc(owner);
  d->channel_owner = owner;
  d->channel_id = channel_id;
  d->cb = callback;
  d->cb_privdata = privdata;
  d->sub = NULL;
  ngx_memcpy(&d->msg_id, msg_id, sizeof(*msg_id));
  d->chanhead = chead;
  
  if(memstore_slot() != owner) {
    //check if we need to ask for a message
    memstore_ipc_send_get_message(d->channel_owner, d->channel_id, &d->msg_id, d);
  }
  else {
    chmsg = chanhead_find_next_message(d->chanhead, &d->msg_id, &findmsg_status);
    
    if(chmsg == NULL && use_redis) {
      subscribe_data_free(d);
      nchan_store_redis.get_message(channel_id, msg_id, callback, privdata);
    }
    else {
      return nchan_memstore_handle_get_message_reply(chmsg == NULL ? NULL : chmsg->msg, findmsg_status, d);
    }
  }
  
  return NGX_OK; //async only now!
}

ngx_int_t nchan_memstore_handle_get_message_reply(nchan_msg_t *msg, nchan_msg_status_t findmsg_status, void *data) {
  subscribe_data_t           *d = (subscribe_data_t *)data;
  //nchan_store_channel_head_t *chanhead = d->chanhead;
  
  d->cb(findmsg_status, msg, d->cb_privdata);
  
  subscribe_data_free(d);
  return NGX_OK;
}

static ngx_int_t chanhead_push_message(nchan_store_channel_head_t *ch, store_message_t *msg) {
  msg->next = NULL;
  msg->prev = ch->msg_last;
  
  assert(msg->msg->id.tagcount == 1);
  
  if(msg->prev != NULL) {
    msg->prev->next = msg;
    msg->msg->prev_id = msg->prev->msg->id;
  }
  else {
    msg->msg->prev_id.time = 0;
    msg->msg->prev_id.tag[0] = 0;
    msg->msg->prev_id.tagcount = 1;
  }
  
  //set time and tag
  if(msg->msg->id.time == 0) {
    msg->msg->id.time = ngx_time();
  }
  if(ch->msg_last && ch->msg_last->msg->id.time == msg->msg->id.time) {
    msg->msg->id.tag[0] = ch->msg_last->msg->id.tag[0] + 1;
  }
  else {
    msg->msg->id.tag[0] = 0;
  }

  if(ch->msg_first == NULL) {
    ch->msg_first = msg;
  }
  ch->channel.messages++;
  ngx_atomic_fetch_add(&ch->shared->stored_message_count, 1);
  ngx_atomic_fetch_add(&ch->shared->total_message_count, 1);

  ch->msg_last = msg;
  
  //DBG("create %V %V", msgid_to_str(&msg->msg->id), chanhead_msg_to_str(msg));
  chanhead_messages_gc(ch);
  assert(ch->msg_last == msg); //why does this happen?
  return ch->msg_last == msg ? NGX_OK : NGX_ERROR;
}

typedef struct {
  nchan_msg_t             msg;
  ngx_buf_t               buf;
  ngx_file_t              file;
} shmsg_memspace_t;

static nchan_msg_t *create_shm_msg(nchan_msg_t *m) {
  shmsg_memspace_t        *stuff;
  nchan_msg_t             *msg;
  ngx_buf_t               *mbuf = NULL, *buf=NULL;
  mbuf = m->buf;
  
  size_t                  total_sz, buf_body_size = 0, content_type_size = 0, buf_filename_size = 0;
  
  content_type_size += m->content_type.len;
  if(ngx_buf_in_memory_only(mbuf)) {
    buf_body_size = ngx_buf_size(mbuf);
  }
  if(mbuf->in_file && mbuf->file != NULL) {
    buf_filename_size = mbuf->file->name.len;
    if (buf_filename_size > 0) {
      buf_filename_size ++; //for null-termination
    }
  }
  
  total_sz = sizeof(*stuff) + (buf_filename_size + content_type_size + buf_body_size);
#if NCHAN_MSG_LEAK_DEBUG
  size_t    debug_sz = m->lbl.len;
  total_sz += debug_sz;
#endif
  
  if((stuff = shm_calloc(shm, total_sz, "message")) == NULL) {
    ERR("can't allocate 'shared' memory for msg for channel id");
    return NULL;
  }
  
  assert(m->id.tagcount == 1);
  
  msg = &stuff->msg;
  buf = &stuff->buf;
  
  ngx_memcpy(msg, m, sizeof(*msg));
  ngx_memcpy(buf, mbuf, sizeof(*buf));
  
  msg->buf = buf;
  
  msg->content_type.data = (u_char *)&stuff[1] + buf_filename_size;
  
  msg->content_type.len = content_type_size;
  if(content_type_size > 0) {
    ngx_memcpy(msg->content_type.data, m->content_type.data, content_type_size);
  }
  else {
    msg->content_type.data = NULL; //mostly for debugging, this isn't really necessary for correct operation.
  }
  
  if(buf_body_size > 0) {
    buf->pos = (u_char *)&stuff[1] + buf_filename_size + content_type_size;
    buf->last = buf->pos + buf_body_size;
    buf->start = buf->pos;
    buf->end = buf->last;
    ngx_memcpy(buf->pos, mbuf->pos, buf_body_size);
  }
  
  if(mbuf->file!=NULL) {
    buf->file = &stuff->file;
    ngx_memcpy(buf->file, mbuf->file, sizeof(*buf->file));
    
    buf->file->fd =NGX_INVALID_FILE;
    buf->file->log = ngx_cycle->log;
    
    buf->file->name.data = (u_char *)&stuff[1];
    
    ngx_memcpy(buf->file->name.data, mbuf->file->name.data, buf_filename_size-1);
  }
  msg->shared = 1;
  msg->temp_allocd = 0;
  
#if NCHAN_MSG_LEAK_DEBUG  
  msg->rsv = NULL;
  msg->lbl.len = m->lbl.len;
  msg->lbl.data = (u_char *)stuff + (total_sz - debug_sz);
  ngx_memcpy(msg->lbl.data, m->lbl.data, msg->lbl.len);
  
  msg_debug_add(msg);
#endif
  
  return msg;
}

void msg_reserve(nchan_msg_t *msg, char *lbl) {
  ngx_atomic_fetch_add(&msg->refcount, 1);
#if NCHAN_MSG_LEAK_DEBUG  
  msg_rsv_dbg_t     *rsv;
  shmtx_lock(shm);
  rsv=shm_locked_alloc(shm, sizeof(*rsv) + ngx_strlen(lbl), "msgdebug");
  rsv->lbl = (char *)(&rsv[1]);
  ngx_memcpy(rsv->lbl, lbl, ngx_strlen(lbl));
  if(msg->rsv == NULL) {
    msg->rsv = rsv;
    rsv->prev = NULL;
    rsv->next = NULL;
  }
  else {
    msg->rsv->prev = rsv;
    rsv->next = msg->rsv;
    rsv->prev = NULL;
    msg->rsv = rsv;
  }
  shmtx_unlock(shm);
#endif
}
void msg_release(nchan_msg_t *msg, char *lbl) {
  ngx_atomic_fetch_add(&msg->refcount, -1);
  assert(msg->refcount >= 0);
#if NCHAN_MSG_LEAK_DEBUG
  msg_rsv_dbg_t     *cur, *prev, *next;
  size_t             sz = ngx_strlen(lbl);
  ngx_int_t          rsv_found=0;
  shmtx_lock(shm);
  for(cur = msg->rsv; cur != NULL; cur = cur->next) {
    if(ngx_memcmp(lbl, cur->lbl, sz) == 0) {
      prev = cur->prev;
      next = cur->next;
      if(prev) {
        prev->next = next;
      }
      if(next) {
        next->prev = prev;
      }
      if(cur == msg->rsv) {
        msg->rsv = next;
      }
      shm_locked_free(shm, cur);
      rsv_found = 1;
      break;
    }
  }
  assert(rsv_found);
  shmtx_unlock(shm);
#endif
}

static store_message_t *create_shared_message(nchan_msg_t *m, ngx_int_t msg_already_in_shm) {
  store_message_t          *chmsg;
  nchan_msg_t              *msg;
  
  if(msg_already_in_shm) {
    msg = m;
  }
  else {
    if((msg=create_shm_msg(m)) == NULL ) {
      return NULL;
    }
  }
  if((chmsg = ngx_alloc(sizeof(*chmsg), ngx_cycle->log)) != NULL) {
    chmsg->prev = NULL;
    chmsg->next = NULL;
    chmsg->msg  = msg;
  }
  return chmsg;
}

static ngx_int_t nchan_store_publish_message(ngx_str_t *channel_id, nchan_msg_t *msg, nchan_loc_conf_t *cf, callback_pt callback, void *privdata) {
  return nchan_store_publish_message_generic(channel_id, msg, 0, cf, callback, privdata);
}

ngx_int_t nchan_store_publish_message_generic(ngx_str_t *channel_id, nchan_msg_t *msg, ngx_int_t msg_in_shm, nchan_loc_conf_t *cf, callback_pt callback, void *privdata) {
  nchan_store_channel_head_t  *chead;
  
  if((chead = nchan_memstore_get_chanhead(channel_id, cf)) == NULL) {
    ERR("can't get chanhead for id %V", channel_id);
    callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, privdata);
    return NGX_ERROR;
  }
  return nchan_store_chanhead_publish_message_generic(chead, msg, msg_in_shm, cf, callback, privdata);
}

ngx_int_t nchan_store_chanhead_publish_message_generic(nchan_store_channel_head_t *chead, nchan_msg_t *msg, ngx_int_t msg_in_shm, nchan_loc_conf_t *cf, callback_pt callback, void *privdata) {
  nchan_channel_t              channel_copy_data;
  nchan_channel_t             *channel_copy = &channel_copy_data;
  store_message_t             *shmsg_link;
  ngx_int_t                    sub_count;
  nchan_msg_t                 *publish_msg;
  ngx_int_t                    owner = chead->owner;
  ngx_int_t                    rc;
  
  if(callback == NULL) {
    callback = empty_callback;
  }

  assert(msg->id.tagcount == 1);
  
  //this coould be dangerous!!
  if(msg->id.time == 0) {
    msg->id.time = ngx_time();
  }
  msg->expires = ngx_time() + cf->buffer_timeout;
  
  if(memstore_slot() != owner) {
    publish_msg = create_shm_msg(msg);
    memstore_ipc_send_publish_message(owner, &chead->id, publish_msg, cf, callback, privdata);
    return NGX_OK;
  }
  
  chead->channel.expires = ngx_time() + cf->buffer_timeout;
  sub_count = chead->shared->sub_count;
  
  //TODO: address this weirdness
  //chead->min_messages = cf->min_messages;
  chead->min_messages = 0; // for backwards-compatibility, this value is ignored? weird...
  
  chead->max_messages = cf->max_messages;
  
  chanhead_messages_gc(chead);
  if(cf->max_messages == 0) {
    ///no buffer
    channel_copy=&chead->channel;
    
    if((shmsg_link = create_shared_message(msg, msg_in_shm)) == NULL) {
      callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, privdata);
      ERR("can't create unbuffered message for channel %V", &chead->id);
      return NGX_ERROR;
    }
    publish_msg= shmsg_link->msg;
    publish_msg->expires = ngx_time() + NCHAN_NOBUFFER_MSG_EXPIRE_SEC;
    
    if(chanhead_push_message(&mpt->unbuffered_dummy_chanhead, shmsg_link) != NGX_OK) {
      callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, privdata);
      ERR("can't enqueue unbuffered message for channel %V", &chead->id);
      return NGX_ERROR;
    }
    
    publish_msg->prev_id.time = 0;
    publish_msg->prev_id.tag[0] = 0;
    publish_msg->prev_id.tagcount = 1;
    
    DBG("publish unbuffer msg %V expire %i ", msgid_to_str(&publish_msg->id), cf->buffer_timeout);
  }
  else {
    
    if((shmsg_link = create_shared_message(msg, msg_in_shm)) == NULL) {
      callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, privdata);
      ERR("can't create shared message for channel %V", &chead->id);
      return NGX_ERROR;
    }
    
    if(chanhead_push_message(chead, shmsg_link) != NGX_OK) {
      callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, privdata);
      ERR("can't enqueue shared message for channel %V", &chead->id);
      return NGX_ERROR;
    }
    
    ngx_memcpy(channel_copy, &chead->channel, sizeof(*channel_copy));
    channel_copy->subscribers = sub_count;
    assert(shmsg_link != NULL);
    assert(chead->msg_last == shmsg_link);
    publish_msg = shmsg_link->msg;
  }
  
  chead->latest_msgid = publish_msg->id;
  
  //do the actual publishing
  assert(publish_msg->id.time != publish_msg->prev_id.time || ( publish_msg->id.time == publish_msg->prev_id.time && publish_msg->id.tag[0] != publish_msg->prev_id.tag[0]));
  DBG("publish %V expire %i", msgid_to_str(&publish_msg->id), cf->buffer_timeout);
  DBG("prev: %V", msgid_to_str(&publish_msg->prev_id));
  if(publish_msg->buf && publish_msg->buf->file) {
    DBG("fd %i", publish_msg->buf->file->fd);
  }
 
  rc = nchan_memstore_publish_generic(chead, publish_msg, 0, NULL);
  
  if(cf->use_redis) {
    rc = nchan_store_redis.publish(&chead->id, publish_msg, cf, callback, privdata);
  }
  else {
    callback(rc, channel_copy, privdata);
  }

  return rc;
}

nchan_store_t  nchan_store_memory = {
    //init
    &nchan_store_init_module,
    &nchan_store_init_worker,
    &nchan_store_init_postconfig,
    &nchan_store_create_main_conf,
    
    //shutdown
    &nchan_store_exit_worker,
    &nchan_store_exit_master,
    
    //async-friendly functions with callbacks
    &nchan_store_async_get_message, //+callback
    &nchan_store_subscribe, //+callback
    &nchan_store_publish_message, //+callback
    
    &nchan_store_delete_channel, //+callback
    &nchan_store_find_channel, //+callback
    
    //message stuff
    NULL,
    NULL,
    
};

