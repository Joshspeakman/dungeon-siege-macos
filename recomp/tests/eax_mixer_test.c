/* Real guest COM entry points and CoreAudio mixer, without opening a device.
 * clang -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined
 * -Wl,-dead_strip tests/eax_mixer_test.c runtime/win32/eax_reverb.c
 * -framework AudioToolbox -o /tmp/eax_mixer_test */
#include "../runtime/win32/dsound.c"
#include <assert.h>
#include <stdarg.h>
uint8_t *G_MEM; uint32_t w32_process_heap;
static uint32_t cursor=0x10000;
uint32_t heap_alloc(uint32_t h,uint32_t f,uint32_t n){uint32_t p=cursor;cursor+=(n+15)&~15u;assert(cursor<1<<20);return p;}
int heap_free(uint32_t h,uint32_t p){return 1;}
void w32_audio_out(const float *p,uint32_t n,int rate,int source){}
static uint32_t call(void (*fn)(Ctx *),int n,...){
 Ctx c={.esp=0x1000};va_list ap;va_start(ap,n);for(int i=0;i<n;i++)rt_w32(G_MEM,c.esp+4+i*4,va_arg(ap,uint32_t));va_end(ap);
 fn(&c);assert(c.esp==0x1004+4*n);return c.eax;
}
static uint32_t bits(float f){uint32_t v;memcpy(&v,&f,4);return v;}
static uint32_t guid(uint32_t d1,int version){uint32_t p=heap_alloc(0,0,16);rt_w32(G_MEM,p,d1);rt_w16(G_MEM,p+4,version==2?0xb224:0xc341);rt_w16(G_MEM,p+6,version==2?0x11d2:0x11d1);memcpy(GP(p+8),version==2?EAX20_LISTENER:EAX10,8);return p;}
static uint32_t set(Buf *b,uint32_t g,uint32_t id,const void *data,uint32_t n){if(n)memcpy(GP(0x4000),data,n);return call(k_Set,7,b->obj+16,g,id,0u,0u,n?0x4000u:0u,n);}
static uint32_t get(Buf *b,uint32_t g,uint32_t id,uint32_t n){return call(k_Get,8,b->obj+16,g,id,0u,0u,0x5000u,n,0x6000u);}
static double impulse(Buf *b,int tail){
 float out[1024];double energy=0;
 b->playing=1;b->play=0;b->frac=0;b->lp_l=b->lp_r=b->lp_send=0;
 eaxr_free(reverb);reverb=eaxr_new(OUT_RATE);eax_changed();
 for(int k=0;k<180;k++){
  AudioBufferList io={.mNumberBuffers=1,.mBuffers={{2,sizeof out,out}}};assert(!render(0,0,0,0,512,&io));
  for(int i=0;i<1024;i++){assert(isfinite(out[i]));if(!tail || k>=2)energy+=(double)out[i]*out[i];}
 }
 return energy;
}
int main(void){
 G_MEM=calloc(1,1<<20);unsetenv("W32_AUDIO_MUTE");Buf *b;new_buf(&b);b->is3d=1;
 eax_environment(0);reverb=eaxr_new(OUT_RATE);primary_playing=1;
 uint32_t l2=guid(0x0306a6a8,2),b2=guid(0x0306a6a7,2),l1=guid(0x4a4e6fc1,1),b1=guid(0x4a4e6fc0,1);
 uint32_t sets[]={l2,b2,l1,b1},max[]={15,14,4,1};
 for(int j=0;j<4;j++){
  for(uint32_t id=0;id<=max[j];id++)assert(!call(k_QuerySupport,4,b->obj+16,sets[j],id,0x6000u));
  assert(call(k_QuerySupport,4,b->obj+16,sets[j],max[j]+1,0x6000u)!=0 && !rt_r32(G_MEM,0x6000));
 }
 int32_t room=-2500;assert(!set(b,l2,0x80000002u,&room,4));assert(eax.room==-1000);
 assert(!get(b,l2,2,4) && (int32_t)rt_r32(G_MEM,0x5000)==-1000);
 int32_t occlusion=-6000;assert(!set(b,b2,0x80000009u,&occlusion,4));assert(b->eax.occlusion==0);
 assert(!set(b,l2,0,0,0));assert(eax.room==room && b->eax.occlusion==occlusion);
 assert(get(b,l2,1,4)==DSERR_INVALIDPARAM && rt_r32(G_MEM,0x6000)==56);
 float nan=NAN;EaxListener before=eax;assert(set(b,l2,5,&nan,4)==DSERR_INVALIDPARAM && !memcmp(&eax,&before,sizeof eax));
 struct {uint32_t env;float volume,decay,damping;} e1={8,.5f,2.5f,.75f};
 assert(!set(b,l1,0,&e1,sizeof e1));
 for(uint32_t i=1;i<=4;i++){assert(!get(b,l1,i,4));if(i==1)assert(rt_r32(G_MEM,0x5000)==8);else assert(fabsf(F(rt_r32(G_MEM,0x5000))-((float *)&e1)[i-1])<.001f);}
 assert(set(b,l1,4,&e1.damping,0)==DSERR_INVALIDPARAM);
 assert(!call(t_SetPosition,5,b->obj+8,bits(4),bits(0),bits(0),1u));assert(b->spatial.pos[0]==0);
 assert(!call(l_SetRolloffFactor,3,b->obj+8,bits(.5f),1u));assert(lis.rolloff==1);
 assert(!call(l_CommitDeferredSettings,1,b->obj+8));assert(b->spatial.pos[0]==4 && lis.rolloff==.5f);
 double left,right,d;gains3d(b,&left,&right,&d);assert(right>left*10 && d==4);
 assert(!call(t_SetPosition,5,b->obj+8,bits(-4),bits(0),bits(0),0u));gains3d(b,&left,&right,&d);assert(left>right*10);
 /* Direct obstruction must not filter the separate send to the room. */
 b->spatial.pos[0]=0;b->ch=1;b->bits=16;b->rate=OUT_RATE;b->align=2;b->size=1024;b->mem=0x8000;
 rt_w16(G_MEM,b->mem,16384);eax_buffer_defaults(b);eax_environment(8);
 double wet=impulse(b,1);assert(wet>1e-5);
 b->eax.obstruction=-8000;b->eax.obstruction_lf=1;double obstructed=impulse(b,1);assert(fabs(wet-obstructed)<1e-8);
 b->eax.obstruction=0;b->eax.room=-10000;double dry=impulse(b,1);assert(dry==0);
 b->eax.room=0;b->eax.occlusion=-6000;b->eax.occlusion_lf=1;b->eax.occlusion_room=1;
 double occluded=impulse(b,1);assert(occluded<wet*.01);
 eax_buffer_defaults(b);eax.room=-10000;assert(impulse(b,1)==0);
 printf("EAX: property negotiation, validation, EAX1 round trips, EAX2 and 3D deferred commits, positioning, wet/dry and obstruction/occlusion routing passed (wet %.8f, dry %.8f, occluded %.8f)\n",wet,dry,occluded);
 eaxr_free(reverb);free(G_MEM);return 0;
}
