#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/rollingquantile.h"
#include <vector>
#include <algorithm>
#include <limits>
#include <cstdio>
#include <cstdlib>

int testRandomOperations() {
    Vrr13::ExactRollingQuantile q;std::vector<int64_t> values;uint64_t seed=123456789;
    const auto random=[&]() {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;};
    for(int i=0;i<40000;++i) {
        if(values.empty()||random()%100<52) {
            int64_t v=int64_t(random()%2001)-1000;
            if(i%117==0)v=std::numeric_limits<int64_t>::min();
            if(i%119==0)v=std::numeric_limits<int64_t>::max();
            q.add(v);values.push_back(v);
        } else {
            const size_t at=random()%values.size();
            if(!q.remove(values[at]))return 1;
            values.erase(values.begin()+at);
        }
        if(i%101==0 && values.size()>10) {
            std::vector<int64_t> removed;
            for(int k=0;k<7;++k){size_t at=random()%values.size();removed.push_back(values[at]);values.erase(values.begin()+at);}
            q.removeBatch(std::move(removed));
        }
        if(q.size()!=values.size())return 2;
        if(values.empty())continue;
        const size_t rank=1+random()%values.size();auto sorted=values;std::sort(sorted.begin(),sorted.end());
        if(q.atRank(rank)!=sorted[rank-1])return 3;
        if(q.atRank(1)!=sorted.front()||q.atRank(q.size())!=sorted.back())return 4;
        if(i%257==0) {
            auto copied=q;auto moved=std::move(copied);Vrr13::ExactRollingQuantile assigned;assigned=moved;
            if(moved.atRank(rank)!=sorted[rank-1]||assigned.atRank(rank)!=sorted[rank-1])return 5;
            copied=std::move(assigned);if(copied.atRank(rank)!=sorted[rank-1])return 6;
        }
        if(i%10007==0){q.clear();values.clear();}
    }
    std::puts("40000 exact differential insert/expiry/rank/copy/move operations passed");
    return 0;
}

uint64_t state=0x123456789;
uint64_t randomValue(){state^=state<<13;state^=state>>7;state^=state<<17;return state;}
bool check(const Vrr13::ExactRollingQuantile& q,const std::vector<int64_t>& expected,bool every=false){
 if(q.size()!=expected.size()||q.empty()!=expected.empty())return false;
 if(expected.empty())return q.atRank(0)==0;
 if(q.atRank(0)!=expected.front()||q.atRank(expected.size()+1)!=expected.back())return false;
 const size_t step=every?1:std::max<size_t>(1,expected.size()/37);
 for(size_t i=0;i<expected.size();i+=step)if(q.atRank(i+1)!=expected[i])return false;
 return q.atRank(expected.size())==expected.back();
}
int testBoundaryRebalancing(){
 for(int pattern=0;pattern<5;++pattern){
  const size_t n=pattern==4?262144:32768;Vrr13::ExactRollingQuantile q;std::vector<int64_t> expected;
  for(size_t i=0;i<n;++i){int64_t v=pattern==0?int64_t(i):pattern==1?-int64_t(i):pattern==2?int64_t(randomValue()%7):pattern==3?int64_t(randomValue()%2000001)-1000000:500;
   if(i%1009==0)v=std::numeric_limits<int64_t>::min();if(i%1013==0)v=std::numeric_limits<int64_t>::max();q.add(v);expected.push_back(v);}
  std::sort(expected.begin(),expected.end());if(!check(q,expected,true))return 1;
  std::vector<int64_t> smallBatch;
  for(int i=0;i<17;++i){const size_t at=randomValue()%expected.size();smallBatch.push_back(expected[at]);expected.erase(expected.begin()+at);}
  q.removeBatch(smallBatch);if(!check(q,expected,true))return 11;
  for(int i=0;i<5000;++i){const size_t at=randomValue()%expected.size();const int64_t v=expected[at];if(!q.remove(v))return 2;expected.erase(expected.begin()+at);
   if(i%2==0){const int64_t added=int64_t(randomValue()%20001)-10000;q.add(added);expected.insert(std::upper_bound(expected.begin(),expected.end(),added),added);}if(i%67==0&&!check(q,expected))return 3;}
  std::vector<int64_t> removed;for(int i=0;i<9000;++i){const size_t at=randomValue()%expected.size();removed.push_back(expected[at]);expected.erase(expected.begin()+at);}q.removeBatch(removed);if(!check(q,expected,true))return 4;
  auto copied=q;auto moved=std::move(copied);if(!check(moved,expected)||!copied.empty())return 5;copied.add(-42);if(copied.size()!=1||copied.atRank(1)!=-42)return 6;
  Vrr13::ExactRollingQuantile assigned;assigned=std::move(moved);if(!check(assigned,expected)||!moved.empty())return 7;
  std::vector<int64_t> all=expected;assigned.removeBatch(all);if(!assigned.empty()||assigned.size()!=0)return 8;assigned.add(7);if(assigned.atRank(1)!=7)return 9;
  q.clear();if(!q.empty())return 10;
  std::printf("Pattern %d: %zu values, exact ranks, duplicate removal, rebalance, partial/full expiry and copy/move reuse passed\n",pattern,n);
 }
 return 0;
}

int main(){if(testRandomOperations())return 1;return testBoundaryRebalancing();}
