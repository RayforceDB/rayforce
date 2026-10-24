#define _POSIX_C_SOURCE 200809L
#include "rayforce.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

static uint64_t now(clockid_t c) { struct timespec t; if (clock_gettime(c,&t)) exit(9); return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec; }
static ray_t* eval(const char* s) { ray_t* v=ray_eval_str(s); if(!v || RAY_IS_ERR(v)) { const char* msg=ray_error_msg(); fprintf(stderr,"eval failed: %s: %s\n",s,msg ? msg : "no error message"); if(v)ray_error_free(v); exit(3); } return v; }
static void discard(const char* s) { ray_t* v=eval(s); ray_release(v); }
int main(int argc,char** argv) {
 if(argc!=3) { fprintf(stderr,"usage: %s CASE(0..4) ROWS(1000..10000000, multiple of 1000)\n",argv[0]); return 2; }
 char* end = NULL;
 errno = 0; long choice = strtol(argv[1], &end, 10);
 if(errno || end == argv[1] || *end || choice < 0 || choice > 4) return 2;
 errno = 0; long long rows = strtoll(argv[2], &end, 10);
 if(errno || end == argv[2] || *end || rows < 1000 || rows > 10000000 || rows % 1000) return 2;
 int which = (int)choice; int64_t n = (int64_t)rows;
 ray_runtime_t* rt=ray_runtime_create(0,NULL); if(!rt)return 3;
 char setup[200]; snprintf(setup,sizeof(setup),"(set MX971 (%% (til %lld) 1000))",(long long)n); discard(setup);
 discard("(set MY971 (- 999 MX971))");
 snprintf(setup,sizeof(setup),"(set MK971 (%% (til %lld) 997))",(long long)n); discard(setup);
 discard("(set MB971 (table [x y k] (list MX971 MY971 MK971)))");
 const char* cases[]={"(min (at MB971 'x) (at MB971 'y))","(at (select {r: (min x y) from: MB971}) 'r)","(at (select {r: (if (< x y) x y) from: MB971}) 'r)","(at (select {r: (min x) from: MB971}) 'r)","(at (select {r: (sum (min x y)) from: MB971 by: k asc: k}) 'r)"};
 int64_t sums[997]={0}; for(int64_t i=0;i<n;i++){int64_t x=i%1000,y=999-x;sums[i%997]+=x<y?x:y;}
 for(int it=-2;it<5;it++){
  uint64_t c0=now(CLOCK_PROCESS_CPUTIME_ID),t0=now(CLOCK_MONOTONIC);ray_t* v=eval(cases[which]);uint64_t t1=now(CLOCK_MONOTONIC),c1=now(CLOCK_PROCESS_CPUTIME_ID);
  int64_t wantlen=which==3?1:which==4?997:n;
  if(v->type!=RAY_I64 || ray_len(v)!=wantlen){fprintf(stderr,"shape kind=%d type=%d len=%lld expected=%lld\n",which,(int)v->type,(long long)ray_len(v),(long long)wantlen);exit(4);}
  const int64_t* d=ray_data(v);
  for(int64_t j=0;j<wantlen;j++){int64_t x=j%1000,y=999-x,want=which==3?0:which==4?sums[j]:(x<y?x:y);if(d[j]!=want){fprintf(stderr,"value kind=%d at=%lld got=%lld want=%lld\n",which,(long long)j,(long long)d[j],(long long)want);exit(5);}}
  ray_release(v);
  if(it>=0)printf("PRICE %d %.9f %.9f\n",it,(double)(t1-t0)/1e6,(double)(c1-c0)/1e6);
 }
 puts("VALIDATED 7");ray_runtime_destroy(rt);return 0;
}
