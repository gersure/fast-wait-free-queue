#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "fifo.h"

typedef union {
  void * volatile data;
  char padding[64];
} cache_t;

typedef struct _fifo_node_t {
  struct {
    struct _fifo_node_t * next;
    char volatile flag;
  } retired __attribute__((aligned(64)));
  struct _fifo_node_t * volatile next __attribute__((aligned(64)));
  size_t id;
  cache_t buffer[0] __attribute__((aligned(64)));
} node_t;

typedef struct _fifo_pair_t pair_t;
typedef fifo_handle_t handle_t;

#define fetch_and_add(p, v) __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL)
#define compare_and_swap __sync_val_compare_and_swap
#define test_and_set(p) __atomic_test_and_set(p, __ATOMIC_RELAXED)
#define spin_while(cond) while (cond) __asm__ ("pause")

#define ENQ (0)
#define DEQ (1)
#define ALT(i) (1 - i)

static inline
node_t * new_node(size_t id, size_t size)
{
  size = sizeof(node_t) + sizeof(cache_t [size]);

  node_t * node;

  posix_memalign((void **) &node, 4096, size);
  memset(node, 0, size);

  node->id = id;
  return node;
}

static inline
int push(handle_t * rlist, node_t * node)
{
  if (rlist->tail == NULL) {
    rlist->head = node;
    rlist->tail = node;
  } else {
    assert(rlist->tail->id < node->id);
    rlist->tail->retired.next = node;
    rlist->tail = node;
  }

  return ++rlist->count;
}

static inline
size_t scan(handle_t * plist, size_t lowest)
{
  size_t hazard = -1;

  handle_t * p;

  for (p = plist; hazard > lowest && p != NULL; p = p->next) {
    if (p->node[ENQ] && p->node[ENQ]->id < hazard) {
      hazard = p->node[ENQ]->id;
    }

    if (p->node[DEQ] && p->node[DEQ]->id < hazard) {
      hazard = p->node[DEQ]->id;
    }
  }

  return hazard;
}

static inline
void clean(handle_t * rlist, size_t hazard)
{
  node_t * curr = rlist->head;

  while ((curr = rlist->head)) {
    if (curr->id < hazard) {
      rlist->head = curr->retired.next;
      rlist->count--;
      free(curr);
    } else {
      return;
    }
  }

  if (rlist->head == NULL) {
    rlist->tail = NULL;
  }
}

static inline
void try_free(node_t * node, handle_t * handle, fifo_t * fifo)
{
  if (!test_and_set(&node->retired.flag)) {
    int count = push(handle, node);

    if (count >= 2 * fifo->W) {
      size_t lowest = handle->head->id;
      size_t hazard = scan(fifo->plist, lowest);

      if (hazard > lowest) {
        clean(handle, hazard);
      }
    }
  }
}

static inline
node_t * update(node_t * node, size_t to, size_t size)
{
  size_t i;
  node_t * prev;
  node_t * next = NULL;

  for (i = node->id; i < to; ++i) {
    prev = node;
    node = prev->next;

    if (!node) {
      if (!next) {
        next = new_node(i + 1, size);
      }

      if (NULL == (node = compare_and_swap(&prev->next, NULL, next))) {
        node = next;
        next = NULL;
      }
    }
  }

  if (next) free(next);

  assert(node->id == to);
  return node;
}

static inline
void * volatile * acquire(fifo_t * fifo, handle_t * handle, int op)
{
  size_t i  = fetch_and_add(&fifo->tail[op].index, 1);
  size_t ni = i / fifo->S;
  size_t li = i % fifo->S;

  node_t * node = handle->node[op];

  if (node->id != ni) {
    node_t * prev = node;
    node = update(prev, ni, fifo->S);

    handle->node[op] = node;

    if (prev->id < handle->node[ALT(op)]->id) {
      try_free(prev, handle, fifo);
    }
  }

  return &node->buffer[li].data;
}

void fifo_put(fifo_t * fifo, handle_t * handle, void * data)
{
  void * volatile * ptr = acquire(fifo, handle, ENQ);
  *ptr = data;
}

void * fifo_get(fifo_t * fifo, handle_t * handle)
{
  void * volatile * ptr = acquire(fifo, handle, DEQ);
  void * data;

  spin_while((data = *ptr) == NULL);
  return data;
}

void fifo_init(fifo_t * fifo, size_t size, size_t width)
{
  fifo->S = size;
  fifo->W = width;

  node_t * node = new_node(0, size);

  fifo->tail[ENQ].index = 0;
  fifo->tail[DEQ].index = 0;
  fifo->T = node;

  fifo->plist = NULL;
}

void fifo_register(fifo_t * fifo, handle_t * me)
{
  me->head = NULL;
  me->tail = NULL;
  me->count  = 0;
  me->node[ENQ] = fifo->T;
  me->node[DEQ] = fifo->T;

  handle_t * curr = fifo->plist;

  do {
    me->next = curr;
  } while (me->next != (curr = compare_and_swap(&fifo->plist, curr, me)));
}

void fifo_unregister(fifo_t * fifo, handle_t * handle)
{
  node_t * enq = handle->node[ENQ];
  node_t * deq = handle->node[DEQ];

  handle->node[ENQ] = NULL;
  handle->node[DEQ] = NULL;

  try_free(enq, handle, fifo);
  try_free(deq, handle, fifo);

  while (handle->head) {
    size_t lowest = handle->head->id;
    size_t hazard = scan(fifo->plist, lowest);

    if (hazard > lowest) {
      clean(handle, hazard);
    }
  }
}

#ifdef BENCHMARK

typedef fifo_handle_t thread_local_t;
#include "bench.h"

static fifo_t fifo;

void init(int nprocs)
{
  fifo_init(&fifo, 510, nprocs);
}

void thread_init(int id, void * handle)
{
  fifo_register(&fifo, handle);
}

void thread_exit(int id, void * handle)
{
  fifo_unregister(&fifo, handle);
}

void enqueue(void * val, void * handle)
{
  fifo_put(&fifo, handle, val);
}

void * dequeue(void * handle)
{
  return fifo_get(&fifo, handle);
}

#endif
