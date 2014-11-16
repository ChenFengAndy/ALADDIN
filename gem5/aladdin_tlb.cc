#include <string>

#include "aladdin/common/cacti-p/cacti_interface.h"
#include "aladdin/common/cacti-p/io.h"

#include "aladdin_tlb.hh"
#include "CacheDatapath.h"
#include "debug/CacheDatapath.hh"

AladdinTLB::AladdinTLB(
    CacheDatapath *_datapath, unsigned _num_entries, unsigned _assoc,
    Cycles _hit_latency, Cycles _miss_latency, Addr _page_bytes,
    bool _is_perfect, unsigned _num_walks, unsigned _bandwidth,
    std::string _cacti_config) :
  datapath(_datapath),
  numEntries(_num_entries),
  assoc(_assoc),
  hitLatency(_hit_latency),
  missLatency(_miss_latency),
  pageBytes(_page_bytes),
  isPerfectTLB(_is_perfect),
  numOutStandingWalks(_num_walks),
  cacti_cfg(_cacti_config),
  bandwidth(_bandwidth)
{
  if (numEntries > 0)
    tlbMemory = new TLBMemory (_num_entries, _assoc, _page_bytes);
  else
    tlbMemory = new InfiniteTLBMemory();
  regStats();
}

AladdinTLB::~AladdinTLB()
{
  delete tlbMemory;
}

AladdinTLB::deHitQueueEvent::deHitQueueEvent(AladdinTLB *_tlb)
   : Event(Default_Pri, AutoDelete),
     tlb(_tlb) {}

void
AladdinTLB::deHitQueueEvent::process()
{
  assert(!tlb->hitQueue.empty());
  tlb->datapath->finishTranslation(tlb->hitQueue.front(), false);
  tlb->hitQueue.pop_front();
}

const char *
AladdinTLB::deHitQueueEvent::description() const
{
  return "TLB Hit";
}

AladdinTLB::outStandingWalkReturnEvent::outStandingWalkReturnEvent(
    AladdinTLB *_tlb) : Event(Default_Pri, AutoDelete), tlb(_tlb) {}

void
AladdinTLB::outStandingWalkReturnEvent::process()
{
  // TLB return events are free because only the CPU's hardware control units
  // can write to the TLB; programs can only read the TLB.
  assert(!tlb->missQueue.empty());
  Addr vpn = tlb->outStandingWalks.front();
  //insert TLB entry; for now, vpn == ppn
  tlb->insert(vpn, vpn);

  auto range = tlb->missQueue.equal_range(vpn);
  for(auto it = range.first; it!= range.second; ++it)
    tlb->datapath->finishTranslation(it->second, true);

  tlb->numOccupiedMissQueueEntries --;
  tlb->missQueue.erase(vpn);
  tlb->outStandingWalks.pop_front();
  updates++;  // Upon completion, increment TLB
}

const char *
AladdinTLB::outStandingWalkReturnEvent::description() const
{
  return "TLB Miss";
}


bool
AladdinTLB::translateTiming(PacketPtr pkt)
{
  Addr vaddr = pkt->req->getPaddr();
  DPRINTF(CacheDatapath, "Translating vaddr %#x.\n", vaddr);
  Addr offset = vaddr % pageBytes;
  Addr vpn = vaddr - offset;
  Addr ppn;

  reads++;  // Both TLB hits and misses perform a read.
  if (isPerfectTLB || tlbMemory->lookup(vpn, ppn))
  {
      DPRINTF(CacheDatapath, "TLB hit. Phys addr %#x.\n", ppn + offset);
      hits++;
      hitQueue.push_back(pkt);
      deHitQueueEvent *hq = new deHitQueueEvent(this);
      datapath->schedule(hq, datapath->clockEdge(hitLatency));
      return true;
  }
  else
  {
      // TLB miss! Let the TLB handle the walk, etc
      DPRINTF(CacheDatapath, "TLB miss for addr %#x\n", vaddr);

      if (missQueue.find(vpn) == missQueue.end())
      {
        if (numOutStandingWalks != 0 && outStandingWalks.size() >= numOutStandingWalks)
          return false;
        outStandingWalks.push_back(vpn);
        outStandingWalkReturnEvent *mq = new outStandingWalkReturnEvent(this);
        datapath->schedule(mq, datapath->clockEdge(missLatency));
        numOccupiedMissQueueEntries ++;
        DPRINTF(CacheDatapath, "Allocated TLB miss entry for addr %#x, page %#x\n",
                vaddr, vpn);
      }
      else
      {
        DPRINTF(CacheDatapath, "Collapsed into existing miss entry for page %#x\n", vpn);
      }
      misses++;
      missQueue.insert({vpn, pkt});
      return true;
  }
}

void
AladdinTLB::insert(Addr vpn, Addr ppn)
{
    tlbMemory->insert(vpn, ppn);
}

bool
AladdinTLB::canRequestTranslation()
{
  return requests_this_cycle < bandwidth &&
         numOccupiedMissQueueEntries < numOutStandingWalks;
}

void
AladdinTLB::regStats()
{
  using namespace Stats;
  hits.name("system.datapath.tlb.hits")
      .desc("TLB hits")
      .flags(total | nonan);
  misses.name("system.datapath.tlb.misses")
        .desc("TLB misses")
        .flags(total | nonan);
  hitRate.name("system.datapath.tlb.hitRate")
         .desc("TLB hit rate")
         .flags(total | nonan);
  hitRate = hits / (hits + misses);
  reads.name("system.datapath.tlb.reads")
       .desc("TLB reads")
       .flags(total | nonan);
  updates.name("system.datapath.tlb.updates")
         .desc("TLB updates")
         .flags(total | nonan);
}

void
AladdinTLB::computeCactiResults()
{
  DPRINTF(CacheDatapath, "Invoking CACTI for TLB power and area estimates.\n");
  uca_org_t cacti_result = cacti_interface(cacti_cfg);
  readEnergy = cacti_result.power.readOp.dynamic * 1e9;
  writeEnergy = cacti_result.power.writeOp.dynamic * 1e9;
  leakagePower = cacti_result.power.readOp.leakage * 1000;
  area = cacti_result.area;
}

void
AladdinTLB::getAveragePower(
    unsigned int cycles, unsigned int cycleTime,
    float* avg_power, float* avg_dynamic, float* avg_leak)
{
  *avg_dynamic = (reads.value() * readEnergy + updates.value() * writeEnergy) /
                 (cycles * cycleTime);
  *avg_leak = leakagePower;
  *avg_power = *avg_dynamic + *avg_leak;
}

std::string
AladdinTLB::name() const
{
  return datapath->name() + ".tlb";
}

bool
TLBMemory::lookup(Addr vpn, Addr& ppn, bool set_mru)
{
    int way = (vpn / pageBytes) % ways;
    for (int i=0; i < sets; i++) {
        if (entries[way][i].vpn == vpn && !entries[way][i].free) {
            ppn = entries[way][i].ppn;
            assert(entries[way][i].mruTick > 0);
            if (set_mru) {
                entries[way][i].setMRU();
            }
            entries[way][i].hits++;
            return true;
        }
    }
    ppn = Addr(0);
    return false;
}

void
TLBMemory::insert(Addr vpn, Addr ppn)
{
    Addr a;
    if (lookup(vpn, a)) {
        return;
    }
    int way = (vpn / pageBytes) % ways;
    AladdinTLBEntry* entry = NULL;
    Tick minTick = curTick();
    for (int i=0; i < sets; i++) {
        if (entries[way][i].free) {
            entry = &entries[way][i];
            break;
        } else if (entries[way][i].mruTick <= minTick) {
            minTick = entries[way][i].mruTick;
            entry = &entries[way][i];
        }
    }
    assert(entry);
    if (!entry->free) {
        DPRINTF(CacheDatapath, "Evicting entry for vpn %#x\n", entry->vpn);
    }

    entry->vpn = vpn;
    entry->ppn = ppn;
    entry->free = false;
    entry->setMRU();
}
