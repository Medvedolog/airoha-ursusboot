#include <lzma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t prop_byte(uint32_t lc, uint32_t lp, uint32_t pb) {
    return (uint8_t)((pb * 5U + lp) * 9U + lc);
}
static void put32le(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put64le(uint8_t *p, uint64_t v) { for (unsigned i=0;i<8;++i) p[i]=(uint8_t)(v>>(8U*i)); }
int main(int argc,char **argv) {
    if (argc != 4) { fprintf(stderr,"usage: %s INPUT.raw OUTPUT.lzma DICT_SIZE\n",argv[0]); return 2; }
    FILE *f=fopen(argv[1],"rb"); if(!f){perror("open input");return 2;}
    fseek(f,0,SEEK_END); long nlong=ftell(f); fseek(f,0,SEEK_SET); if(nlong<0)return 2;
    size_t n=(size_t)nlong; uint8_t *in=malloc(n?n:1); if(!in)return 2; if(fread(in,1,n,f)!=n)return 2; fclose(f);
    uint32_t dict=(uint32_t)strtoul(argv[3],NULL,0);
    lzma_options_lzma opt; if(lzma_lzma_preset(&opt,6))return 2;
    opt.dict_size=dict; opt.lc=2; opt.lp=2; opt.pb=3; opt.mode=LZMA_MODE_NORMAL; opt.nice_len=128; opt.mf=LZMA_MF_BT4; opt.depth=0;
    opt.ext_flags=0; lzma_set_ext_size(opt,(uint64_t)n);
    lzma_filter filters[2]={{LZMA_FILTER_LZMA1EXT,&opt},{LZMA_VLI_UNKNOWN,NULL}};
    size_t cap=n+n/2+1024*1024; uint8_t *out=malloc(cap+13); if(!out)return 2; size_t outn=0;
    lzma_ret r=lzma_raw_buffer_encode(filters,NULL,in,n,out+13,&outn,cap); if(r!=LZMA_OK){fprintf(stderr,"encode failed:%d\n",(int)r);return 3;}
    out[0]=prop_byte(2,2,3); put32le(out+1,dict); put64le(out+5,(uint64_t)n);
    FILE *o=fopen(argv[2],"wb"); if(!o){perror("open output");return 2;} if(fwrite(out,1,outn+13,o)!=outn+13)return 2; fclose(o);
    fprintf(stderr,"raw=%zu compressed=%zu prop=%02x dict=%u eopm=0\n",n,outn+13,out[0],dict);
    return 0;
}
