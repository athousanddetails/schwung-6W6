/*
 * metal_candidates.cpp — cut an A/B file for one metal voice so the owner can
 * pick a voicing by ear. Count-in clicks:
 *   1 = hardware recording      2 = what ships now
 *   3 = candidate A             4 = candidate B          5 = candidate C
 *
 *   ./metal_candidates <ref.wav> <out.wav> <voice oh|ch|cy> \
 *        <A: tonal noise hp lp sat> <B: ...> <C: ...>
 *
 * Renders through Sd606MetalVoice (the shipping fork), every hit normalised
 * to the same peak, so only SHAPE is being compared.
 */
#include "score.h"
#include "HiHats.hpp"
#include "sd606_cymbal.h"
#include "sd606_metal_hw.h"
#include "sd606_metal_voice.h"
using namespace SynthDrums606;

static void click(std::vector<float>&o,int rate,int n){
    for(int c=0;c<n;++c){ for(int i=0;i<(int)(0.012*rate);++i) o.push_back(0.6f*sinf(2*M_PI*1500*i/(float)rate)*(1.0f-i/(0.012f*rate)));
                          o.insert(o.end(),(size_t)(0.10*rate),0.0f);} o.insert(o.end(),(size_t)(0.25*rate),0.0f);}
static void norm(std::vector<float>&x){float p=0;for(float v:x)p=std::max(p,fabsf(v));for(float&v:x)v=v/(p>0?p:1)*0.8f;}
static std::vector<float> rend(const HiHatSpec&sp,int rate,size_t n){
    Sd606MetalVoice v; v.init((double)rate,0x6065u); v.trigger(sp,1.0f,1.0f);
    std::vector<float> o(n); for(float&s:o)s=v.process(); trim_onset(o); o.resize(n,0.0f); return o;}
static void wav(const char*path,const std::vector<float>&x,int rate){
    FILE*f=fopen(path,"wb"); uint32_t nb=(uint32_t)x.size()*2;
    auto p32=[&](uint32_t v){fputc(v&255,f);fputc((v>>8)&255,f);fputc((v>>16)&255,f);fputc((v>>24)&255,f);};
    auto p16=[&](uint32_t v){fputc(v&255,f);fputc((v>>8)&255,f);};
    fwrite("RIFF",1,4,f);p32(36+nb);fwrite("WAVEfmt ",1,8,f);p32(16);p16(1);p16(1);p32(rate);p32(rate*2);p16(2);p16(16);fwrite("data",1,4,f);p32(nb);
    for(float v:x){int s=(int)(std::max(-1.0f,std::min(1.0f,v))*32767);p16((uint16_t)(int16_t)s);} fclose(f);}
int main(int argc,char**argv){
    if(argc<4+15){fprintf(stderr,"usage: see header\n");return 1;}
    int rate; auto ref=load_wav(argv[1],&rate); trim_onset(ref); norm(ref);
    const char*voice=argv[3];
    HiHatSpec cur = !strcmp(voice,"oh") ? kHwOpenHatSpec
                  : !strcmp(voice,"ch") ? kHwClosedHatSpec : kCymbalSpec;
    HiHatSpec hw  = !strcmp(voice,"oh") ? kHwOpenHatSpec
                  : !strcmp(voice,"ch") ? kHwClosedHatSpec : kCymbalSpec;
    const size_t len=std::max(ref.size(),(size_t)(0.5*rate))+(size_t)(0.2*rate);
    std::vector<float> o; click(o,rate,1); o.insert(o.end(),ref.begin(),ref.end()); o.insert(o.end(),(size_t)(0.5*rate),0.0f);
    auto c=rend(cur,rate,len); norm(c); click(o,rate,2); o.insert(o.end(),c.begin(),c.end()); o.insert(o.end(),(size_t)(0.5*rate),0.0f);
    for(int k=0;k<3;++k){ HiHatSpec s=hw; const char**a=(const char**)argv+4+k*5;
        s.tonalMix=atof(a[0]); s.noiseMix=atof(a[1]); s.noiseHighPassHz=atof(a[2]); s.noiseLowPassHz=atof(a[3]); s.saturationDrive=atof(a[4]);
        auto r=rend(s,rate,len); norm(r); click(o,rate,3+k); o.insert(o.end(),r.begin(),r.end()); o.insert(o.end(),(size_t)(0.5*rate),0.0f);}
    wav(argv[2],o,rate); printf("%s  %.1fs\n",argv[2],o.size()/(double)rate); return 0;}
