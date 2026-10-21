#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(double)t.tv_sec+1e-9*t.tv_nsec;}
int main(int argc,char**argv){
 if(argc!=3)return 1;const char*kind=argv[1];int64_t n=strtoll(argv[2],NULL,10);if(n<1||n>10000000)return 2;
 ray_runtime_t*rt=ray_runtime_create(0,NULL);if(!rt)return 3;
 int list=!strcmp(kind,"list"),raw=!strcmp(kind,"raw"),bulk=!strcmp(kind,"bulk");
 ray_t*v=list?ray_list_new(n):ray_vec_new(RAY_I64,n);if(!v||RAY_IS_ERR(v))return 4;
 memset(ray_data(v),0,(size_t)n*8);int64_t seed=7;ray_t*item=list?ray_vec_from_raw(RAY_I64,&seed,1):NULL;if(list&&(!item||RAY_IS_ERR(item)))return 5;
 int64_t block[4096];for(int i=0;i<4096;i++)block[i]=7;
 double start=now();
 if(list){for(int64_t i=0;i<n;i++)v=ray_list_append(v,item);}
 else if(bulk){for(int64_t i=0;i<n;i+=4096){int64_t count=n-i<4096?n-i:4096;v=ray_vec_append_raw(v,block,count);}}
 else{for(int64_t i=0;i<n;i++){int64_t value=3*i+7;v=raw?ray_vec_append_raw(v,&value,1):ray_vec_append(v,&value);}}
 double elapsed=now()-start;if(!v||RAY_IS_ERR(v)||v->len!=n)return 6;
 for(int64_t i=0;i<n;i++){if(list){if(((ray_t**)ray_data(v))[i]!=item)return 7;}else if(((int64_t*)ray_data(v))[i]!=(bulk?7:3*i+7))return 8;}
 printf("case=%s n=%lld ms=%.9f ns_per_elem=%.6f valid=1\n",kind,(long long)n,elapsed*1e3,elapsed*1e9/n);
 ray_release(v);ray_release(item);ray_runtime_destroy(rt);return 0;
}
