#!/usr/bin/env python3
"""Run the production A/B EQ experiment cleanup transaction against host adapters."""
from pathlib import Path
import re
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / "src"


def function_source(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unclosed function: {signature}")


HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#define CX3_AB_SAMPLES 8
#define CX3_AB_PAGES 8
#define CX3_AB_HEAD 64
static void logSink(const char *, ...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct Cx3AbRingResult {
 UInt32 handedOver,samples,nent,pages,sw2hwStatus,hw2swStatus,unknown,cmdUs,dummyMtt;
 UInt32 queryEqStatus[2],queryEqWords[2][8],mttFirstEntry[2],mttLastEntry[2];
 UInt32 inboxMttH,inboxMttL;
 UInt64 busFirst[2],busLast[2],mttEntryOffset[2],ringVirt[2];
 UInt32 nonzeroBase[2],nonzeroFirst[2],nonzeroFinal[2],changedFirst[2],changedFinal[2];
 UInt32 changedOffsetLo[2],changedOffsetHi[2],pagesChanged[2],ownerSetBase[2],ownerSetFinal[2];
 UInt32 firstChangeUs[2],lastChangeUs[2],sampleUs[8],changedPerSample[2][8];
 UInt32 changedVsPrev[2][8],nonzeroPerSample[2][8],ownerSetPerSample[2][8];
 UInt32 changedPerPage[2][8],ownerSetPerPage[2][8];
 UInt8 headBefore[2][64],headFirst[2][64],headAfter[2][64];
};
struct IcmPage { UInt64 busAddr; };
struct Mlx4Icm {
 UInt64 allocationId; Mlx4DmaGeneration generation; UInt8 *virt; UInt64 bytes;
 IcmPage pages[CX3_AB_PAGES]; UInt32 pageCount;
};
struct Mlx4EqContextResult { UInt32 words[8]; };
static UInt32 checks=0, failures=0, cases=0;
static void ck(bool ok,const char *name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}}
static void ck(bool ok,const std::string &name){ck(ok,name.c_str());}
static std::vector<std::string> events;
static std::vector<UInt32> freedOffsets;
static std::vector<UInt32> freedIcmIds;
static std::map<void*,size_t> hostBlocks;
static UInt32 allocOrdinal=0, failIomallocAt=0, icmOrdinal=0, failIcmAt=0;
static UInt32 mttAllocOrdinal=0, failMttAllocAt=0;
static UInt32 mttWriteOrdinal=0, failMttWriteAt=0;
static UInt32 freeRefuseMask=0;
static bool sw2hwOK=true, hw2swOK=true;
static UInt64 mttValues[512];
static UInt64 fakeNow=0;
static UInt32 nextIcmId=1;
static UInt8 mailbox[MLX4_MAILBOX_SIZE];
static UInt32 mailboxAllocCalls=0;

static void *IOMalloc(size_t n){
 ++allocOrdinal; if(allocOrdinal==failIomallocAt)return 0;
 UInt8 *p=(UInt8*)std::calloc(1,n); if(p)hostBlocks[p]=n; return p;
}
static void IOFree(void *p,size_t n){
 if(!p)return; auto it=hostBlocks.find(p); ck(it!=hostBlocks.end()&&it->second==n,"adapter IOFree receives a live block and exact size");
 if(it!=hostBlocks.end()){hostBlocks.erase(it);std::free(p);}
}
static UInt64 abNowNs(){fakeNow+=1000000;return fakeNow;}
static void IOSleep(UInt32 ms){ck(ms==1,"synthetic clock receives expected one-ms tick");fakeNow+=(UInt64)ms*1000000;}
static void mlx4PutBe32(void *p,UInt32 v){UInt8 *b=(UInt8*)p;b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v;}
static UInt32 mlx4Log2(UInt32 v){UInt32 n=0;while(v>1){v>>=1;++n;}return n;}
static UInt32 abNonzero(const UInt8 *p,UInt64 n){UInt32 c=0;for(UInt64 i=0;i<n;++i)if(p[i])++c;return c;}
static UInt32 abOwnerSet(const UInt8 *p,UInt32 nent,UInt8 f){UInt32 c=0,s=MLX4_EQ_ENTRY_SIZE<<f,o=MLX4_EQE_OWNER_BYTE(f);for(UInt32 i=0;i<nent;++i)if(p[(UInt64)i*s+o]&0x80)++c;return c;}
static UInt32 abDiff(const UInt8 *a,const UInt8 *b,UInt64 n,UInt32 *lo,UInt32 *hi,UInt32 *pp){
 UInt32 c=0,l=0xffffffffu,h=0xffffffffu;UInt32 local[8]={0};
 for(UInt64 i=0;i<n;++i)if(a[i]!=b[i]){++c;if(l==0xffffffffu)l=(UInt32)i;h=(UInt32)i;UInt32 k=(UInt32)(i>>MLX4_ICM_PAGE_SHIFT);if(k<8)++local[k];}
 if(lo)*lo=l;if(hi)*hi=h;if(pp)for(UInt32 k=0;k<8;++k)pp[k]=local[k];return c;
}

class Mlx4Hw {
public:
 UInt64 fDeviceGeneration=7,fMttBase=0x100000,fMttBytes=512ull*MLX4_MTT_ENTRY_SIZE,fMboxPhys=0xabc;
 UInt32 fMttReserved=64; UInt8 *fMboxVirt=mailbox;
 Mlx4MttAllocator fMttAllocator; Mlx4AbRingRetention fAbRetainedRings[2];
 UInt32 swCalls=0,hwCalls=0,queryCalls=0,mttFreeCalls=0,icmFreeCalls=0;
 Mlx4Hw(){fMttAllocator.init(fMttReserved);}
 Mlx4Icm *icmAlloc(UInt32 pages,bool){
  ++icmOrdinal;events.push_back("icmAlloc"+std::to_string(icmOrdinal));if(icmOrdinal==failIcmAt)return 0;
  Mlx4Icm *x=(Mlx4Icm*)std::calloc(1,sizeof(Mlx4Icm));if(!x)return 0;
  x->allocationId=nextIcmId++;x->generation.init(fDeviceGeneration);x->pageCount=pages;x->bytes=(UInt64)pages*MLX4_ICM_PAGE_SIZE;
  x->virt=(UInt8*)std::calloc(1,(size_t)x->bytes);if(!x->virt){std::free(x);return 0;}
  for(UInt32 i=0;i<pages;++i)x->pages[i].busAddr=0x100000000ull+(UInt64)x->allocationId*0x100000+(UInt64)i*MLX4_ICM_PAGE_SIZE;
  return x;
 }
 void icmFree(Mlx4Icm *x){
  ck(x&&x->allocationId,"icmFree receives live ICM owner");bool hasLiveRange=false;
  for(UInt32 i=0;i<fMttAllocator.allocationCount;++i)if(fMttAllocator.allocations[i].live&&fMttAllocator.allocations[i].ownerIdentity==x)hasLiveRange=true;
  ck(!hasLiveRange,"icmFree adapter rejects release before exact MTT owner release");
  events.push_back("icmFree"+std::to_string(x->allocationId));freedIcmIds.push_back((UInt32)x->allocationId);++icmFreeCalls;std::free(x->virt);std::free(x);
 }
 bool markDmaHandoff(Mlx4Icm *const *blocks,UInt32 count);
 bool mttAlloc(UInt32 npages,Mlx4Icm *owner,UInt32 *off,UInt32 *addr);
 bool mttFree(UInt32 off,UInt32 npages,Mlx4Icm *owner);
 bool mttAllocAbDummy(UInt32 npages,UInt32 *off,UInt32 *addr);
 bool hasAbRetainedRings() const;
 bool retryAbRetainedRings();
 bool eqAbExperiment(bool,UInt32,UInt32,UInt8,UInt32,Cx3AbRingResult*);
 bool mttWrite(UInt32 off,UInt32 pages,const UInt64 *bus){
  ++mttWriteOrdinal;events.push_back("mttWrite"+std::to_string(mttWriteOrdinal));
  if(mttWriteOrdinal==failMttWriteAt||!bus||!pages||off+pages>512)return false;
  bool exactLiveOwner=false;for(UInt32 i=0;i<fMttAllocator.allocationCount;++i){const Mlx4MttAllocator::Range &r=fMttAllocator.allocations[i];if(r.live&&r.offset==off&&r.pages==pages&&r.ownerIdentity&&r.ownerAllocationId){exactLiveOwner=true;break;}}
  ck(exactLiveOwner,"mttWrite adapter requires exact live production allocator owner range");if(!exactLiveOwner)return false;
  for(UInt32 i=0;i<pages;++i)if(!bus[i])return false;
  for(UInt32 i=0;i<pages;++i)mttValues[off+i]=bus[i]|1ull;return true;
 }
 UInt64 mttEntryValue(UInt32 i){if(i>=512)std::abort();return mttValues[i];}
 bool mboxAlloc(){++mailboxAllocCalls;events.push_back("mboxAlloc");return true;}
 bool queryEq(UInt32 eqn,Mlx4EqContextResult *q,UInt32 *status){++queryCalls;events.push_back("QUERY_EQ");if(!q||eqn!=28)return false;std::memset(q,0,sizeof(*q));if(status)*status=0;return true;}
 bool eqHw2Sw(UInt32 eqn,UInt32 *status){++hwCalls;events.push_back("HW2SW_EQ");ck(eqn==28,"HW2SW adapter exact EQN");if(status)*status=hw2swOK?0u:0xfffffffdu;return hw2swOK;}
 bool cmdBox(UInt64 in,UInt64 out,UInt32 mod,UInt32 opmod,UInt32 op,UInt32 timeout,UInt32 *status){
  ++swCalls;events.push_back("SW2HW_EQ");ck(in==fMboxPhys&&out==0&&mod==28&&opmod==0&&op==MLX4_CMD_SW2HW_EQ&&timeout==MLX4_CMD_TIME_CLASS_A,"SW2HW adapter exact command arguments");
  UInt32 lo=((UInt32)mailbox[EQ_CONTEXT_MTT_BASE_L_OFFSET]<<24)|((UInt32)mailbox[EQ_CONTEXT_MTT_BASE_L_OFFSET+1]<<16)|((UInt32)mailbox[EQ_CONTEXT_MTT_BASE_L_OFFSET+2]<<8)|mailbox[EQ_CONTEXT_MTT_BASE_L_OFFSET+3];
  UInt64 addr=((UInt64)mailbox[EQ_CONTEXT_MTT_BASE_H_OFFSET]<<32)|lo;bool exactRange=false;
  for(UInt32 i=0;i<fMttAllocator.allocationCount;++i){const Mlx4MttAllocator::Range &r=fMttAllocator.allocations[i];if(r.live&&r.ownerIdentity&&r.ownerAllocationId&&addr==(UInt64)r.offset*MLX4_MTT_ENTRY_SIZE)exactRange=true;}
  ck(exactRange,"SW2HW adapter requires context MTT base to name a live production owner range");
  if(status)*status=sw2hwOK?0u:0xfffffffdu;
  if(sw2hwOK){for(UInt32 i=0;i<64;++i) if(mailbox[i])break; /* context consumed, no ring access */}
  return sw2hwOK;
 }
};

MARK_HANDOFF
MTT_ALLOC
MTT_FREE
DUMMY_ALLOC
HAS_RETAINED
RETRY_RETAINED
EQ_AB_EXPERIMENT

static void resetGlobals(){events.clear();freedOffsets.clear();freedIcmIds.clear();hostBlocks.clear();allocOrdinal=failIomallocAt=icmOrdinal=failIcmAt=mttAllocOrdinal=failMttAllocAt=mttWriteOrdinal=failMttWriteAt=freeRefuseMask=0;sw2hwOK=hw2swOK=true;fakeNow=0;mailboxAllocCalls=0;nextIcmId=1;std::memset(mttValues,0,sizeof(mttValues));}
static size_t firstEvent(const char *name){for(size_t i=0;i<events.size();++i)if(events[i]==name)return i;return events.size();}
static void checkCleanTemporary(){ck(hostBlocks.empty(),"all temporary host snapshots/page lists freed with exact sizes");}
static void scenarioNormal(bool handedB,UInt32 dummy){
 ++cases;
 resetGlobals();Mlx4Hw h;Cx3AbRingResult o{};bool ok=h.eqAbExperiment(handedB,28,512,0,dummy,&o);
 ck(ok,"normal experiment returns true");ck(o.unknown==0,"normal cleanup reports known ownership");
 ck(o.handedOver==(handedB?1u:0u)&&o.dummyMtt==dummy&&o.pages==4,"result identifies handed slot, dummy reservation, and multi-page rings");
 ck(h.icmFreeCalls==2&&!h.hasAbRetainedRings(),"normal cleanup frees both rings and no retained owners");
 ck(h.fMttAllocator.high==64&&h.fMttAllocator.allocationCount==0,"normal cleanup returns exact MTT ledger to initial index");
 ck(h.hwCalls==1&&h.swCalls==1,"normal command counts include one SW2HW and one HW2SW");
 ck(firstEvent("SW2HW_EQ")<firstEvent("QUERY_EQ")&&firstEvent("QUERY_EQ")<firstEvent("HW2SW_EQ")&&firstEvent("HW2SW_EQ")<firstEvent("mttFree")&&firstEvent("mttFree")<firstEvent("icmFree1"),"normal command and release ordering is SW2HW, query, HW2SW, MTT, then ICM");
 ck(freedIcmIds.size()==2&&freedIcmIds[0]==2&&freedIcmIds[1]==1,"normal ICM owners freed in reverse B/A allocation order");
 ck(freedOffsets.size()==2&&freedOffsets[0]>freedOffsets[1],"normal MTT ranges released in reverse B/A allocation order");
 if(dummy)ck(!h.fMttAllocator.abReservationTracked,"normal dummy reservation reclaimed");
 checkCleanTemporary();
}
static void scenarioSwFailure(){
 ++cases;
 resetGlobals();Mlx4Hw h;Cx3AbRingResult o{};sw2hwOK=false;bool ok=h.eqAbExperiment(false,28,512,0,5,&o);
 ck(!ok&&o.unknown==1,"ambiguous SW2HW failure returns false with unknown ownership");
 ck(h.swCalls==1&&h.hwCalls==0,"SW2HW failure does not invent an HW2SW command");
 ck(h.icmFreeCalls==1&&h.hasAbRetainedRings(),"only control ring frees; handed ring retained");
 auto &r=h.fAbRetainedRings[0];ck(r.valid()&&r.fwMayHold&&r.mttOffset==69&&r.pages==4,"handed ring exact retry metadata retained with fwMayHold");
 ck(!h.fAbRetainedRings[1].valid()&&h.fMttAllocator.allocationCount==2&&h.fMttAllocator.allocations[1].live&&h.fMttAllocator.allocations[1].ownerIdentity==r.icm,"only A slot and its exact live ledger range survive ambiguous SW2HW");
 ck(h.fMttAllocator.abReservationTracked&&h.fMttAllocator.high==73,"dummy reservation remains while handed MTT owner lives after control tail collapses");
 checkCleanTemporary();hw2swOK=true;ck(h.retryAbRetainedRings(),"retry releases ambiguous owner after successful HW2SW");
 ck(!h.hasAbRetainedRings()&&h.fMttAllocator.high==64,"retry reclaims ring and dummy reservation to initial index");
 ck(h.hwCalls==1&&h.icmFreeCalls==2,"retry issues one HW2SW and frees retained ring");
}
static void scenarioHwFailureThenRetry(){
 ++cases;
 resetGlobals();Mlx4Hw h;Cx3AbRingResult o{};hw2swOK=false;bool ok=h.eqAbExperiment(true,28,512,0,0,&o);
 ck(ok&&o.unknown==1&&h.hasAbRetainedRings(),"HW2SW failure retains handed ring and marks unknown while experiment bool remains true");
 ck(h.hwCalls==1&&h.icmFreeCalls==1&&h.fMttAllocator.high==72,"failed handback retains range while control range releases");
 ck(!h.fAbRetainedRings[0].valid(),"unused A retention slot remains empty when B was handed");
 UInt32 before=h.mttFreeCalls;ck(!h.retryAbRetainedRings(),"failed retry remains failed");
 ck(h.mttFreeCalls==before&&h.icmFreeCalls==1&&h.fAbRetainedRings[1].fwMayHold,"failed retry performs no unsafe release");
 hw2swOK=true;ck(h.retryAbRetainedRings(),"successful later retry releases owner");
 ck(h.hwCalls==3&&h.fMttAllocator.high==64&&!h.hasAbRetainedRings(),"retry sequence restores allocator and clears owner");
}
static void scenarioMttRefusal(bool handedB,UInt32 mask){
 ++cases;
 resetGlobals();Mlx4Hw h;Cx3AbRingResult o{};freeRefuseMask=mask;bool ok=h.eqAbExperiment(handedB,28,512,0,5,&o);
 ck(ok&&o.unknown==1,"MTT refusal preserves successful experiment bool but marks unknown cleanup");
 ck(h.hwCalls==1&&h.hasAbRetainedRings(),"MTT refusal retains retry metadata after one successful HW2SW");
 UInt32 owners=0;for(int i=0;i<2;++i)if(h.fAbRetainedRings[i].valid()){++owners;auto &r=h.fAbRetainedRings[i];ck(!r.fwMayHold,"MTT refusal records HW2SW already succeeded");ck(r.icm&&r.mttOffset==(i==0?69u:73u)&&r.pages==4&&r.eqn==28&&r.generation==7,"MTT refusal preserves exact slot owner, offset, size, EQN, and generation");}
 ck(owners==(mask==3?2u:1u),"expected retained slot count matches injected MTT refusal mask");
 ck(h.fMttAllocator.abReservationTracked,"dummy reservation remains while ring MTT owners are retained");
 freeRefuseMask=0;ck(h.retryAbRetainedRings(),"MTT refusal owners retry successfully");
 ck(h.hwCalls==1&&h.fMttAllocator.high==64&&!h.hasAbRetainedRings(),"retry does not repeat HW2SW and restores allocator");
 checkCleanTemporary();
}
static void scenarioPreHandoff(const char *name,UInt32 failIcm,UInt32 failMtt,UInt32 failAlloc,UInt32 failWrite,UInt32 refuseMask,bool retainedExpected){
 ++cases;
 resetGlobals();Mlx4Hw h;Cx3AbRingResult o{};failIcmAt=failIcm;failMttAllocAt=failMtt;failIomallocAt=failAlloc;failMttWriteAt=failWrite;freeRefuseMask=refuseMask;
 bool ok=h.eqAbExperiment(false,28,512,0,5,&o);ck(!ok,std::string(name)+" returns false");
 ck(h.swCalls==0&&h.hwCalls==0,std::string(name)+" fails before device commands");
 ck(h.hasAbRetainedRings()==retainedExpected,std::string(name)+" has expected retained owner state");
 ck(h.fMttAllocator.abReservationTracked==retainedExpected,std::string(name)+" reclaims dummy only when no MTT ring owner remains");
 if(!retainedExpected)ck(h.fMttAllocator.high==64&&h.fMttAllocator.allocationCount==0,std::string(name)+" returns allocator to initial index");
 else {auto &r=h.fAbRetainedRings[0];ck(r.valid()&&r.icm&&r.mttOffset==69&&r.pages==4&&r.eqn==28&&r.generation==7&&!r.fwMayHold,"pre-handoff MTT refusal preserves exact A-slot retry owner");ck(h.fMttAllocator.high>64,std::string(name)+" keeps exact MTT ranges pinned");freeRefuseMask=0;ck(h.retryAbRetainedRings(),std::string(name)+" retained pre-handoff owner retries");ck(h.fMttAllocator.high==64,std::string(name)+" retry restores initial allocator index");}
 checkCleanTemporary();
}
static void scenarioPriorOwnerBlocks(){
 ++cases;
 resetGlobals();Mlx4Hw h;Mlx4Icm *old=h.icmAlloc(2,false);UInt32 off=0,addr=0;ck(old&&h.mttAlloc(2,old,&off,&addr),"fixture establishes prior live MTT owner");
 UInt64 addrs[2]={old->pages[0].busAddr,old->pages[1].busAddr};ck(h.mttWrite(off,2,addrs),"fixture writes prior exact MTT range");
 Mlx4AbRingRetention &r=h.fAbRetainedRings[0];r.icm=old;r.mttOffset=off;r.pages=2;r.eqn=28;r.generation=7;r.fwMayHold=true;
 hw2swOK=false;UInt32 oldId=(UInt32)old->allocationId;Cx3AbRingResult o{};bool ok=h.eqAbExperiment(false,28,512,0,0,&o);
 ck(!ok&&o.unknown==1,"failed prior retry blocks new experiment");
 ck(icmOrdinal==1&&mttWriteOrdinal==1&&h.swCalls==0,"prior retained owner blocks before new allocation and commands");
 ck(r.icm==old&&r.mttOffset==off&&r.pages==2&&r.generation==7&&r.fwMayHold,"prior owner metadata is not overwritten");
 ck(h.icmFreeCalls==0&&h.fMttAllocator.allocations[0].live&&h.fMttAllocator.allocations[0].ownerIdentity==old,"prior owner and exact ledger remain live");
 (void)oldId;
}
int main(){
 scenarioNormal(false,0);scenarioNormal(true,5);scenarioNormal(true,0);scenarioNormal(false,5);
 scenarioSwFailure();scenarioHwFailureThenRetry();scenarioMttRefusal(false,1);scenarioMttRefusal(false,2);scenarioMttRefusal(false,3);scenarioMttRefusal(true,1);scenarioMttRefusal(true,2);
 scenarioPreHandoff("first ring allocation failure",1,0,0,0,0,false);
 scenarioPreHandoff("second ring allocation failure",2,0,0,0,0,false);
 scenarioPreHandoff("first ring MTT allocation failure",0,1,0,0,0,false);
 scenarioPreHandoff("second ring MTT allocation failure",0,2,0,0,0,false);
 scenarioPreHandoff("page list allocation failure",0,0,1,0,0,false);
 scenarioPreHandoff("baseline snapshot allocation failure",0,0,2,0,0,false);
 scenarioPreHandoff("working snapshot allocation failure",0,0,4,0,0,false);
 scenarioPreHandoff("MTT write failure",0,0,0,1,0,false);
 scenarioPreHandoff("pre-handoff MTT free refusal",0,0,0,1,1,true);
 scenarioPriorOwnerBlocks();
 std::printf("A/B experiment production transaction: %u cases, %u checks, %u failures\n",cases,checks,failures);
 return failures?1:0;
}
'''


def build_source(mutant=None):
    hw = (SRC / "Mlx4Hw.cpp").read_text()
    methods = {
        "MARK_HANDOFF": function_source(hw, "bool Mlx4Hw::markDmaHandoff("),
        "MTT_ALLOC": function_source(hw, "bool Mlx4Hw::mttAlloc("),
        "MTT_FREE": function_source(hw, "bool Mlx4Hw::mttFree("),
        "DUMMY_ALLOC": function_source(hw, "bool Mlx4Hw::mttAllocAbDummy("),
        "HAS_RETAINED": function_source(hw, "bool Mlx4Hw::hasAbRetainedRings() const"),
        "RETRY_RETAINED": function_source(hw, "bool Mlx4Hw::retryAbRetainedRings()"),
        "EQ_AB_EXPERIMENT": function_source(hw, "bool Mlx4Hw::eqAbExperiment("),
    }
    alloc_method = methods["MTT_ALLOC"]
    alloc_brace = alloc_method.index("{") + 1
    alloc_method = (alloc_method[:alloc_brace] +
                    "\n    if (++mttAllocOrdinal == failMttAllocAt) return false;\n" +
                    alloc_method[alloc_brace:])
    methods["MTT_ALLOC"] = alloc_method
    free_method = methods["MTT_FREE"]
    brace = free_method.index("{") + 1
    free_method = (free_method[:brace] + "\n    ++mttFreeCalls; events.push_back(\"mttFree\");\n"
                   "    if (owner && owner->allocationId >= 1 && owner->allocationId <= 2 && "
                   "(freeRefuseMask & (1u << (owner->allocationId - 1)))) return false;\n"
                   "    freedOffsets.push_back(offsetEntries);\n" + free_method[brace:])
    methods["MTT_FREE"] = free_method
    body = methods["EQ_AB_EXPERIMENT"]
    if mutant == "bypass-fwMayHold":
        old = "if (fwMayHold[k]) {"
        assert body.count(old) == 1
        body = body.replace(old, "if (false) {", 1)
    elif mutant == "discard-mtt-refusal-metadata":
        marker = "if (mttAllocated[k] && !mttFree(mttOff[k], pages, ring[k])) {"
        at = body.index(marker)
        tail = body[at:]
        old = "retained.icm = ring[k];"
        assert tail.count(old) == 1
        body = body[:at] + tail.replace(old, "/* mutation: lost retry owner */", 1)
    elif mutant == "skip-dummy-reclaim":
        old = "if (dummyMtt && !fMttAllocator.releaseAbReservationIfUnowned()) {"
        assert body.count(old) == 1
        body = body.replace(old, "if (false && dummyMtt && !fMttAllocator.releaseAbReservationIfUnowned()) {", 1)
    methods["EQ_AB_EXPERIMENT"] = body
    result = HARNESS
    for token, method in methods.items():
        result = result.replace(token, method)
    assert "MARK_HANDOFF" not in result and "EQ_AB_EXPERIMENT" not in result
    return result


def compile_run(source, tmp, label):
    binary = Path(tmp) / label
    cmd = ["/usr/bin/clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(SRC),
           "-x", "c++", "-", "-o", str(binary)]
    built = subprocess.run(cmd, input=source, text=True, capture_output=True, timeout=90)
    if built.returncode:
        raise RuntimeError(f"{label} compile failed (not a caught mutation):\n{built.stderr}")
    return binary


def main():
    with tempfile.TemporaryDirectory(prefix="cx3-ab-experiment-") as tmp:
        binary = compile_run(build_source(), tmp, "production")
        run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
        print(run.stdout, end="")
        if run.returncode:
            print(run.stderr, end="")
            raise SystemExit(run.returncode)
        for mutant in ("bypass-fwMayHold", "discard-mtt-refusal-metadata", "skip-dummy-reclaim"):
            mutant_bin = compile_run(build_source(mutant), tmp, mutant)
            mutant_run = subprocess.run([str(mutant_bin)], text=True, capture_output=True, timeout=30)
            failures = [line for line in mutant_run.stdout.splitlines() if line.startswith("FAIL ")]
            summary = next((line for line in mutant_run.stdout.splitlines()
                            if line.startswith("A/B experiment production transaction:")), "summary missing")
            counts = re.fullmatch(r"A/B experiment production transaction: (\d+) cases, (\d+) checks, (\d+) failures", summary)
            if (mutant_run.returncode != 1 or not counts or not failures or
                    int(counts.group(3)) != len(failures) or
                    "AddressSanitizer" in mutant_run.stderr or
                    "runtime error:" in mutant_run.stderr):
                raise AssertionError(
                    f"mutation did not complete with behavior assertion failures: {mutant}\n"
                    f"exit={mutant_run.returncode}\n{mutant_run.stdout}\n{mutant_run.stderr}")
            print(f"MUTATION CAUGHT: {mutant} (compiled; behavior exit {mutant_run.returncode})")
            print("  " + summary)
            print("  " + "; ".join(failures[:3]))


if __name__ == "__main__":
    main()
