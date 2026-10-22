#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "ops/internal.h"
#include "core/pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
 if(argc!=3)return 2;int64_t n=2000000,per=n/100;int wide=!strcmp(argv[1],"i64"),desc=!strcmp(argv[2],"desc");ray_runtime_t*rt=ray_runtime_create(0,NULL);if(!rt)return 3;ray_pool_t*pool=ray_pool_get();unsigned configured=pool?ray_pool_total_workers(pool):1;
 int64_t names[100];for(int i=0;i<100;i++){char s[32];int l=snprintf(s,sizeof s,"scan-sym-%04d",i);names[i]=ray_sym_intern(s,l);}
 int64_t*ids=malloc((size_t)n*8);void*raw=malloc((size_t)n*(wide?8:4));if(!ids||!raw)return 4;for(int64_t i=0;i<n;i++){int64_t group=i/per,j=i%per;if(desc){group=99-group;j=per-1-j;}ids[i]=names[group];if(wide)((int64_t*)raw)[i]=(j-per/2)*1000000;else((int32_t*)raw)[i]=(int32_t)j;}
 ray_t*cols[2]={ray_vec_from_raw(RAY_SYM,ids,n),ray_vec_from_raw(wide?RAY_I64:RAY_TIME,raw,n)};for(int k=0;k<2;k++)if(!cols[k]||RAY_IS_ERR(cols[k])||cols[k]->len!=n)return 5;
 double start=now();bool ordered=ray_key_cols_sorted(cols,2,(uint8_t)desc,n);double ms=(now()-start)*1000.0;if(!ordered||memcmp(ray_data(cols[0]),ids,(size_t)n*8)||memcmp(ray_data(cols[1]),raw,(size_t)n*(wide?8:4)))return 6;
 printf("kind=%s direction=%s configured=%u n=%lld ms=%.9f valid=1\n",argv[1],argv[2],configured,(long long)n,ms);ray_release(cols[0]);ray_release(cols[1]);free(ids);free(raw);ray_runtime_destroy(rt);return 0;
}
