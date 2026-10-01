/* Exact, bounded-memory comparison for the fixed-width ClickBench native schema.
 * No runtime symbol interning: check each distinct symbol mapping by its bytes,
 * then compare IDs through that mapping. Derived indexes are not data payload.
 * Build: cc -O3 -Iinclude bench/parquet_load/verify-native.c -o verify-native
 * Usage: verify-native LEFT RIGHT COLUMN ...
 *        verify-native --parted TABLE LEFT_SPLAYED RIGHT_PARTED COLUMN ...
 */
#define _POSIX_C_SOURCE 200809L
#include "rayforce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static void fail(const char* message) { fprintf(stderr,"%s\n",message); exit(1); }
typedef struct { unsigned char* data; size_t size; } mapping;
static mapping map_file(const char* root, const char* name) {
    char path[4096];
    if (snprintf(path,sizeof(path),"%s/%s",root,name) >= (int)sizeof(path)) fail("path too long");
    int fd = open(path,O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd,&st) || st.st_size < 32) fail(path);
    mapping m = {mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0),(size_t)st.st_size};
    close(fd); if (m.data == MAP_FAILED) fail("mmap failed"); return m;
}
static void unmap_file(mapping m) { munmap(m.data,m.size); }
static uint32_t u32(const void* p) { uint32_t x; memcpy(&x,p,4); return x; }
static uint64_t u64(const void* p) { uint64_t x; memcpy(&x,p,8); return x; }
typedef struct { mapping file; uint64_t count; size_t* offsets; } domain;
static domain read_domain(const char* root) {
    domain d = {.file=map_file(root,".sym")};
    if (u32(d.file.data) != 0x4c525453) fail("unexpected symbol file magic");
    d.count = u64(d.file.data+4);
    if (d.count > (d.file.size-12)/4) fail("invalid symbol count");
    d.offsets = malloc(d.count*sizeof(*d.offsets));
    if (!d.offsets) fail("offset allocation failed");
    size_t off=12;
    for (uint64_t i=0;i<d.count;i++) {
        if (off > d.file.size-4) fail("truncated symbol length");
        d.offsets[i]=off; uint32_t len=u32(d.file.data+off); off+=4;
        if (len > d.file.size-off) fail("truncated symbol bytes");
        off+=len;
    }
    if (off != d.file.size) fail("unexpected symbol file suffix");
    return d;
}
static int symbol_equal(domain* a, uint64_t x, domain* b, uint64_t y) {
    const unsigned char* p=a->file.data+a->offsets[x];
    const unsigned char* q=b->file.data+b->offsets[y];
    uint32_t n=u32(p); return n==u32(q) && !memcmp(p+4,q+4,n);
}
static size_t width(const ray_t* h) {
    static const size_t sizes[]={0,1,1,2,4,8,4,8,4,4,8,16};
    if (h->type==RAY_SYM) return (size_t)1 << (h->attrs&3);
    if (h->type<1 || h->type>RAY_GUID) fail("unsupported column type");
    return sizes[h->type];
}
static uint64_t id(const unsigned char* p,size_t w) {
    uint64_t v=0; memcpy(&v,p,w); return v;
}
static int part_order(const void* a,const void* b) {
    int64_t x=*(const int64_t*)a,y=*(const int64_t*)b;
    return (x>y)-(x<y);
}
int main(int argc,char** argv) {
    const char* table=NULL;
    if(argc>2 && !strcmp(argv[1],"--parted")) { table=argv[2]; argv+=2; argc-=2; }
    if (argc<4) fail("usage: verify-native LEFT RIGHT COLUMN ...");
    int64_t* parts=NULL; size_t nparts=1;
    if(table) {
        DIR* d=opendir(argv[2]); if(!d) fail("cannot open parted root");
        struct dirent* ent; nparts=0;
        while((ent=readdir(d))) {
            if(ent->d_name[0]=='.') continue;
            char* end; errno=0; long long p=strtoll(ent->d_name,&end,10);
            if(errno || *end || p<0 || end==ent->d_name) fail("invalid partition name");
            int64_t* grown=realloc(parts,(nparts+1)*sizeof(*parts));
            if(!grown) fail("partition allocation failed");
            parts=grown; parts[nparts++]=p;
        }
        closedir(d); if(!nparts) fail("no partitions");
        qsort(parts,nparts,sizeof(*parts),part_order);
        for(size_t p=0;p<nparts;p++) if(parts[p]!=(int64_t)p) fail("missing/duplicate partition");
    }
    mapping schema_a=map_file(argv[1],".d");
    domain left=read_domain(argv[1]),right=read_domain(argv[2]);
    uint64_t* translated=malloc(right.count*sizeof(*translated));
    if (!translated) fail("translation allocation failed");
    memset(translated,0xff,right.count*sizeof(*translated));
    int64_t total=-1,offset=0;
    for(size_t part=0;part<nparts;part++) {
    char leaf[4096];
    if(snprintf(leaf,sizeof(leaf),table ? "%s/%lld/%s" : "%s",argv[2],
                (long long)part,table)>=(int)sizeof(leaf)) fail("partition path too long");
    mapping schema_b=map_file(leaf,".d");
    if(schema_a.size!=schema_b.size || memcmp(schema_a.data,schema_b.data,schema_a.size)) fail("column names/order mismatch");
    unmap_file(schema_b);
    int64_t rows=-1;
    for(int c=3;c<argc;c++) {
        mapping a=map_file(argv[1],argv[c]),b=map_file(leaf,argv[c]);
        ray_t x,y; memcpy(&x,a.data,32); memcpy(&y,b.data,32);
        if (x.type!=y.type || x.len<0 || y.len<0 || (rows>=0 && rows!=y.len) ||
            (total>=0 && total!=x.len) || offset>x.len || y.len>x.len-offset) fail("column schema mismatch");
        total=x.len; rows=y.len; size_t wx=width(&x),wy=width(&y);
        if ((uint64_t)total>(a.size-32)/wx || (uint64_t)rows>(b.size-32)/wy) fail("truncated column");
        if (x.type==RAY_SYM) {
            for(int64_t r=0;r<rows;r++) {
                uint64_t i, j;
                if (wx==4 && wy==4) {
                    i=u32(a.data+32+(size_t)(offset+r)*4); j=u32(b.data+32+(size_t)r*4);
                } else {
                    i=id(a.data+32+(size_t)(offset+r)*wx,wx); j=id(b.data+32+(size_t)r*wy,wy);
                }
                if(i>=left.count || j>=right.count) fail("symbol outside domain");
                if(translated[j]!=i) {
                    if(!symbol_equal(&left,i,&right,j)) {
                        fprintf(stderr,"symbol mismatch column %s row %lld\n",argv[c],(long long)r); return 1;
                    }
                    translated[j]=i;
                }
            }
        } else if(memcmp(a.data+32+(size_t)offset*wx,b.data+32,(size_t)rows*wx)) {
            fprintf(stderr,"value mismatch column %s\n",argv[c]); return 1;
        }
        if(!table) { printf("verified %s %lld\n",argv[c],(long long)rows); fflush(stdout); }
        unmap_file(a); unmap_file(b);
    }
    offset+=rows;
    if(table) { printf("verified partition %zu rows %lld\n",part,(long long)rows); fflush(stdout); }
    }
    if(offset!=total) fail("total row count mismatch");
    printf("VERIFIED %lld rows %d columns %zu partitions\n",(long long)total,argc-3,nparts);
    free(parts);unmap_file(schema_a);
    free(translated);free(left.offsets);free(right.offsets);unmap_file(left.file);unmap_file(right.file);
    return 0;
}
