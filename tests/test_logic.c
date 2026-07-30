#include "record.h"
#include "payload.h"
#include "excursion.h"
#include "app_config.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
#define CHECK(c,msg) do{ if(c){printf("  PASS  %s\n",msg);} else {printf("  FAIL  %s\n",msg); fails++;} }while(0)

static struct sample_record mk(uint32_t seq,int16_t amb_cc,uint16_t hum,uint32_t pa,uint16_t fl){
    struct sample_record r; memset(&r,0,sizeof r);
    r.seq=seq; r.ts_ms=1735689600000LL+seq*1000LL; r.amb_cc=amb_cc; r.probe_cc=REC_TEMP_INVALID;
    r.hum_cpct=hum; r.press_pa=pa; r.batt_mv=3987; r.flags=fl; return r;
}

int main(void){
 char buf[2048]; size_t len;
 printf("\n=== 1. RECORD LAYOUT (on-flash format) ===\n");
 CHECK(sizeof(struct sample_record)==40,"sizeof(sample_record)==40");
 CHECK(_Alignof(struct sample_record)==8,"alignment==8");

 printf("\n=== 2. TELEMETRY PAYLOAD ENCODING ===\n");
 struct sample_record recs[3]={
   mk(1, 523,4560,101325,REC_FLAG_TIME_VALID|REC_FLAG_AMB_VALID),
   mk(2, 812,4600,101300,REC_FLAG_TIME_VALID|REC_FLAG_AMB_VALID|REC_FLAG_EXC_HIGH),
   mk(3, 495,4550,101280,REC_FLAG_TIME_VALID|REC_FLAG_AMB_VALID|REC_FLAG_FIX_VALID)};
 recs[2].lat_e7=487758210; recs[2].lon_e7=91829340;
 int rc=payload_build_telemetry(buf,sizeof buf,"pharma-tracker-001",recs,3,NULL,0,&len);
 CHECK(rc==0,"payload_build_telemetry returns 0");
 CHECK(len>0 && len==strlen(buf),"reported length matches strlen");
 CHECK(buf[0]=='{' && buf[len-1]=='}',"payload is a balanced JSON object");
 int depth=0,ok=1; for(size_t i=0;i<len;i++){ if(buf[i]=='{'||buf[i]=='[')depth++; if(buf[i]=='}'||buf[i]==']')depth--; if(depth<0)ok=0; }
 CHECK(ok&&depth==0,"braces/brackets balanced");
 CHECK(strstr(buf,"\"la\":487758210")!=NULL,"GNSS fix emitted only when FIX_VALID");
 CHECK(strstr(buf,"\"pt\"")==NULL,"probe field omitted when probe invalid");
 printf("  bytes=%zu for 3 records (%.1f B/record)\n",len,(double)len/3);

 printf("\n=== 3. BUFFER OVERFLOW SAFETY ===\n");
 char tiny[64]; size_t tl;
 rc=payload_build_telemetry(tiny,sizeof tiny,"pharma-tracker-001",recs,3,NULL,0,&tl);
 CHECK(rc==-ENOMEM,"overflow returns -ENOMEM (never truncates+sends)");
 rc=payload_build_telemetry(NULL,0,"x",recs,1,NULL,0,&tl);
 CHECK(rc==-EINVAL,"NULL buffer rejected");
 rc=payload_build_telemetry(buf,sizeof buf,"x",recs,0,NULL,0,&tl);
 CHECK(rc==-EINVAL,"zero records rejected");

 printf("\n=== 4. MAX BATCH SIZING (vs 2048 B budget) ===\n");
 struct sample_record big[16];
 for(int i=0;i<16;i++){ big[i]=mk(1000+i,523,4560,101325,REC_FLAG_TIME_VALID|REC_FLAG_AMB_VALID|REC_FLAG_FIX_VALID); big[i].lat_e7=487758210; big[i].lon_e7=91829340; }
 for(int n=1;n<=16;n++){ if(payload_build_telemetry(buf,2048,"pharma-tracker-001",big,n,NULL,0,&len)==0) printf("  n=%2d -> %4zu B\n",n,len); else { printf("  n=%2d -> OVERFLOWS 2048\n",n); break; } }

 printf("\n=== 5. EXCURSION STATE MACHINE (profile %s, %d..%d cC) ===\n",APP_PROFILE_NAME,APP_TEMP_LO_CC,APP_TEMP_HI_CC);
 excursion_init();
 struct excursion_stats st;
 for(int i=0;i<5;i++){ struct sample_record r=mk(i,500,4500,101325,REC_FLAG_TIME_VALID|REC_FLAG_AMB_VALID); { bool edge=false; excursion_update(&r,&edge); }; }
 excursion_get_stats(&st);
 CHECK(!st.alarm_active,"in-band samples -> no alarm");
 for(int i=0;i<6;i++){ struct sample_record r=mk(10+i,1200,4500,101325,REC_FLAG_TIME_VALID|REC_FLAG_AMB_VALID); { bool edge=false; excursion_update(&r,&edge); }; }
 excursion_get_stats(&st);
 CHECK(st.alarm_active,"sustained 12.00C -> alarm raised");
 CHECK(st.max_cc==1200,"max tracked correctly");
 CHECK(st.min_cc==500,"min tracked correctly");
 CHECK(st.time_high_s>0,"time-above-range accumulated");
 printf("  events=%u samples=%u hi_s=%u mkt_valid=%d mkt=%d cC\n",st.events,st.samples,st.time_high_s,st.mkt_valid,st.mkt_cc);
 if(st.mkt_valid) CHECK(st.mkt_cc>=st.min_cc && st.mkt_cc<=st.max_cc,"MKT lies within [min,max]");

 printf("\n=== 6. SHADOW PAYLOAD ===\n");
 rc=payload_build_shadow(buf,sizeof buf,&st,3987,42,0,&len);
 CHECK(rc==0,"payload_build_shadow returns 0");
 depth=0;ok=1; for(size_t i=0;i<len;i++){ if(buf[i]=='{')depth++; if(buf[i]=='}')depth--; if(depth<0)ok=0; }
 CHECK(ok&&depth==0,"shadow JSON balanced");
 CHECK(strstr(buf,"\"state\"")&&strstr(buf,"\"reported\""),"AWS shadow envelope present");
 printf("  %.*s%s\n",(int)(len>200?200:len),buf,len>200?"...":"");

 printf("\n================================================\n");
 printf("  RESULT: %s  (%d failure%s)\n",fails?"FAILURES":"ALL CHECKS PASSED",fails,fails==1?"":"s");
 printf("================================================\n");
 return fails?1:0;
}
