/*
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED. ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

/*
 * Test the client-registered stacks support (`GC_register_stack`,
 * `GC_unregister_stack`, `GC_current_stack`): retention of objects
 * referenced only from a suspended registered stack, non-retention for
 * unused registered stacks, registration churn, and scanning of both
 * the active and the original stack across a `swapcontext()`-based
 * stack switch.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifndef GC_THREADS
#  define GC_THREADS
#endif

#define GC_NO_THREAD_REDIRECTS 1

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(GC_PTHREADS) || defined(GC_WIN32_THREADS)

int
main(void)
{
  printf("stacks test skipped (no client-registered stacks support)\n");
  return 0;
}

#else

#  define CHECK(cond, msg)                            \
    do {                                              \
      if (!(cond)) {                                  \
        fprintf(stderr, "FAILED: %s\n", msg);         \
        exit(1);                                      \
      }                                               \
    } while (0)

#  define FAKE_STACK_PTRS 256
#  define NODE_MAGIC 0x6ea51e57

struct node {
  int magic;
  int index;
  struct node *next;
};

/* Build a linked list of `n` nodes, referenced only via the result. */
static struct node *
make_list(int n)
{
  struct node *head = NULL;
  int i;

  for (i = 0; i < n; i++) {
    struct node *p = (struct node *)GC_MALLOC(sizeof(struct node));

    CHECK(p != NULL, "GC_MALLOC returned NULL");
    p->magic = NODE_MAGIC;
    p->index = i;
    p->next = head;
    head = p;
  }
  return head;
}

static void
check_list(const struct node *p, int n, const char *msg)
{
  int i;

  for (i = n; i > 0; i--) {
    CHECK(p != NULL && p->magic == NODE_MAGIC && p->index == i - 1, msg);
    p = p->next;
  }
  CHECK(NULL == p, msg);
}

/*
 * A fake stack: a block of pointers with a `GC_stack` descriptor.
 * `base` is the address just past the block, as for a real
 * downward-growing stack.
 */
struct fake_stack {
  void *ptrs[FAKE_STACK_PTRS];
  struct GC_stack gs;
};

static void
fake_stack_init(struct fake_stack *fs)
{
  memset(fs, 0, sizeof(*fs));
  fs->gs.base = &fs->ptrs[FAKE_STACK_PTRS];
  fs->gs.limit = &fs->ptrs[0];
  fs->gs.saved_sp = NULL;
}

static GC_word finalized_cnt = 0;

static void GC_CALLBACK
inc_finalized(void *obj, void *client_data)
{
  (void)obj;
  (void)client_data;
  ++finalized_cnt;
}

/*
 * Objects referenced only from a suspended registered stack must
 * survive collections; after the stack is unregistered (or its
 * `saved_sp` is cleared), they must be collectable.
 */
static void
test_suspended_retention(void)
{
  /*
   * Note: the fake stack is allocated with `malloc` deliberately;
   * static data would be scanned as an ordinary root regardless of
   * the stack registration.
   */
  struct fake_stack *fs = (struct fake_stack *)malloc(sizeof(*fs));
  int i;

  CHECK(fs != NULL, "malloc failed");
  fake_stack_init(fs);
  /* Reference the list only from the tail of the fake stack. */
  fs->ptrs[FAKE_STACK_PTRS - 1] = make_list(50);
  fs->gs.saved_sp = &fs->ptrs[FAKE_STACK_PTRS - 1];
  GC_register_stack(&fs->gs);

  for (i = 0; i < 3; i++)
    GC_gcollect();
  check_list((struct node *)fs->ptrs[FAKE_STACK_PTRS - 1], 50,
             "list referenced from suspended stack was clobbered");

  /* An unused registered stack (`saved_sp` is null) is not scanned. */
  for (i = 0; i < FAKE_STACK_PTRS; i++) {
    struct node *p = (struct node *)GC_MALLOC(sizeof(struct node));

    CHECK(p != NULL, "GC_MALLOC returned NULL");
    p->magic = NODE_MAGIC;
    GC_REGISTER_FINALIZER_NO_ORDER(p, inc_finalized, NULL, NULL, NULL);
    fs->ptrs[i] = p;
  }
  fs->gs.saved_sp = NULL;
  finalized_cnt = 0;
  for (i = 0; i < 3; i++) {
    GC_gcollect();
    (void)GC_invoke_finalizers();
  }
  /*
   * Conservative scanning may accidentally retain some of the objects,
   * but most of them should have been finalized, proving that the
   * unused stack itself was not treated as a root.
   */
  CHECK(finalized_cnt > FAKE_STACK_PTRS / 2,
        "objects referenced only from an unused registered stack "
        "were retained");

  GC_unregister_stack(&fs->gs);
  free(fs);
}

/* Exercise sorted insertion/removal with many stacks. */
static void
test_registration_churn(void)
{
  enum { N_STACKS = 100 };
  struct fake_stack *fs
      = (struct fake_stack *)malloc(N_STACKS * sizeof(*fs));
  int i;

  CHECK(fs != NULL, "malloc failed");
  for (i = 0; i < N_STACKS; i++) {
    fake_stack_init(&fs[i]);
    fs[i].ptrs[0] = make_list(3);
    fs[i].gs.saved_sp = &fs[i].ptrs[0];
    GC_register_stack(&fs[i].gs);
  }
  /* Unregister every other stack, then re-register. */
  for (i = 0; i < N_STACKS; i += 2)
    GC_unregister_stack(&fs[i].gs);
  GC_gcollect();
  for (i = 0; i < N_STACKS; i += 2) {
    GC_register_stack(&fs[i].gs);
    /* The list may have been collected while unregistered. */
    fs[i].ptrs[0] = make_list(3);
  }
  GC_gcollect();
  for (i = 0; i < N_STACKS; i++)
    check_list((struct node *)fs[i].ptrs[0], 3,
               "list referenced from one of many suspended stacks "
               "was clobbered");
  for (i = 0; i < N_STACKS; i++)
    GC_unregister_stack(&fs[i].gs);
  free(fs);
}

/* Note: musl has no makecontext/swapcontext, hence the glibc check. */
#  if defined(__linux__) && defined(__GLIBC__)
#    include <ucontext.h>

#    define CTX_STACK_SIZE (256 * 1024)

static ucontext_t ctx_main, ctx_alt;
static struct GC_stack alt_gs;
static struct node *volatile ctx_result;

static void
ctx_fn(void)
{
  /*
   * Runs on the registered alternate stack: allocate, then collect
   * while the captured stack pointer of this thread is inside the
   * alternate stack.  Both this list (referenced from the alternate
   * stack) and the caller's locals (scanned via `saved_sp` of the
   * original stack) must survive.
   */
  struct node *list = make_list(30);

  GC_gcollect();
  GC_gcollect();
  check_list(list, 30, "list allocated on the alternate stack "
                       "was clobbered");
  ctx_result = list;
  swapcontext(&ctx_alt, &ctx_main);
}

static void
test_stack_switch(void)
{
  struct GC_stack *orig_stack = GC_current_stack;
  struct node *local_list = make_list(20);
  void *stack_mem = malloc(CTX_STACK_SIZE);
  volatile int dummy = 0;

  CHECK(orig_stack != NULL, "GC_current_stack not initialized");
  CHECK(stack_mem != NULL, "malloc failed");
  alt_gs.base = (char *)stack_mem + CTX_STACK_SIZE;
  alt_gs.limit = stack_mem;
  alt_gs.saved_sp = NULL;
  GC_register_stack(&alt_gs);

  CHECK(getcontext(&ctx_alt) == 0, "getcontext failed");
  ctx_alt.uc_stack.ss_sp = stack_mem;
  ctx_alt.uc_stack.ss_size = CTX_STACK_SIZE;
  ctx_alt.uc_link = NULL;
  makecontext(&ctx_alt, ctx_fn, 0);

  /*
   * Switch per the protocol described in `gc.h` file.  Some slack
   * below the address of a local is used so that the scanned interval
   * covers this entire frame regardless of the layout of the locals.
   */
  orig_stack->saved_sp = (void *)((GC_word)(GC_uintptr_t)&dummy - 1024);
  GC_current_stack = &alt_gs;
  CHECK(swapcontext(&ctx_main, &ctx_alt) == 0, "swapcontext failed");
  GC_current_stack = orig_stack;
  orig_stack->saved_sp = NULL;

  check_list(local_list, 20, "list referenced from the original stack "
                             "was clobbered across a stack switch");
  check_list(ctx_result, 30, "list allocated on the alternate stack "
                             "was clobbered after switching back");
  GC_unregister_stack(&alt_gs);
  free(stack_mem);
}
#  endif /* __linux__ && __GLIBC__ */

int
main(void)
{
  GC_INIT();
  test_suspended_retention();
  test_registration_churn();
#  if defined(__linux__) && defined(__GLIBC__)
  test_stack_switch();
#  endif
  printf("stacks test succeeded\n");
  return 0;
}

#endif /* GC_PTHREADS && !GC_WIN32_THREADS */
