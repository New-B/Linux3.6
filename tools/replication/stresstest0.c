#define _GNU_SOURCE
#include <sys/mman.h>
#include <pthread.h>
#include <stdio.h>
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <numa.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>

#include "common.h"

#define ARRAY_NB_ENTRIES	1024

static int *array;
static int *replicated_array;

void* read_a()
{
   for(int i=0; i<ARRAY_NB_ENTRIES; i++) {
      printf("[Core %d] %d\n", sched_getcpu(), array[i]);
   }
   return NULL;
}

int main()
{
   assert(numa_num_configured_nodes() >= 2);
   set_affinity(gettid(), get_first_core_of_node(0));

   assert(posix_memalign((void**)&array, sysconf(_SC_PAGESIZE), ARRAY_NB_ENTRIES*sizeof(int)) == 0);
   assert(posix_memalign((void**)&replicated_array, sysconf(_SC_PAGESIZE), ARRAY_NB_ENTRIES*sizeof(int)) == 0);

   for(int i=0; i<ARRAY_NB_ENTRIES; i++)
      array[i] = i;

   assert(madvise(replicated_array, ARRAY_NB_ENTRIES*sizeof(int), MADV_REPLICATE) == 0);

   read_a();
   set_affinity(gettid(),get_first_core_of_node(1));
   read_a();

   free(array);
   free(replicated_array);
   return 0;
}
