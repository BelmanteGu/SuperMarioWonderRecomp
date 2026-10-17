#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#define N 4096
typedef struct { _Atomic uint64_t ra, bytes, calls; } E;
static E tab[3][N];
static void* (*rmemset)(void*,int,size_t); static void* (*rmemcpy)(void*,const void*,size_t);
static void rec(int k, void* ra, size_t n){
  uint64_t a=(uint64_t)ra; unsigned h=(a>>2)*2654435761u>>20 & (N-1);
  for(int i=0;i<N;i++){ E* e=&tab[k][(h+i)&(N-1)]; uint64_t cur=atomic_load(&e->ra);
    if(cur==a || (cur==0 && atomic_compare_exchange_strong(&e->ra,&cur,a)) || atomic_load(&e->ra)==a){ atomic_fetch_add(&e->bytes,n); atomic_fetch_add(&e->calls,1); return; } }
}
void* memset(void* d,int c,size_t n){ if(!rmemset) rmemset=dlsym(RTLD_NEXT,"memset"); if(n>=8192) rec(0,__builtin_return_address(0),n); return rmemset(d,c,n);}
void* memcpy(void* d,const void* s,size_t n){ if(!rmemcpy) rmemcpy=dlsym(RTLD_NEXT,"memcpy"); if(n>=8192) rec(1,__builtin_return_address(0),n); return rmemcpy(d,s,n);}
static void* dumper(void* x){ for(;;){ sleep(25); static const char* nm[2]={"memset","memcpy"}; FILE* f=fopen("/tmp/bigmem.txt","w");
  for(int k=0;k<2;k++) for(int i=0;i<N;i++){ E* e=&tab[k][i]; if(e->calls) fprintf(f,"%s 0x%lx calls %lu MB %.1f\n",nm[k],(unsigned long)e->ra,(unsigned long)e->calls,e->bytes/1048576.0);} 
  fclose(f); FILE* a=fopen("/proc/self/maps","r"); FILE* b=fopen("/tmp/bigmem.maps","w"); char buf[512]; while(fgets(buf,512,a)) fputs(buf,b); fclose(a); fclose(b);} return 0; }
__attribute__((constructor)) static void init(){ pthread_t t; pthread_create(&t,0,dumper,0); }
