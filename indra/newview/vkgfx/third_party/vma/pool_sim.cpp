// #411 texture pool simulation (standalone; not part of the viewer build). See README.md.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#include <cstdio>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdlib>
static uint64_t gMedMax=4ull<<20, gMedBlock=64ull<<20, gDed=0, gDedPad=0; static uint64_t gDedN=0;
static uint64_t gAllocCalls=0,gFreeCalls=0,gLiveBlocks=0,gLiveBlockBytes=0;
static void VKAPI_PTR onAlloc(VmaAllocator,uint32_t,VkDeviceMemory,VkDeviceSize s,void*){++gAllocCalls;++gLiveBlocks;gLiveBlockBytes+=s;}
static void VKAPI_PTR onFree(VmaAllocator,uint32_t,VkDeviceMemory,VkDeviceSize s,void*){++gFreeCalls;--gLiveBlocks;gLiveBlockBytes-=s;}
static const uint32_t kClassK[]={2,3,4,6,8,12,16,24,32,48,64};
constexpr int NC=11;
int main(){ if(getenv("MEDMAX")) gMedMax=strtoull(getenv("MEDMAX"),0,10)<<10; if(getenv("MEDBLK")) gMedBlock=strtoull(getenv("MEDBLK"),0,10)<<20;
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&ai;
  VkInstance inst; if(vkCreateInstance(&ici,0,&inst)) return 1;
  uint32_t n=8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst,&n,pds); VkPhysicalDevice pd=pds[0];
  float pr=1; VkDeviceQueueCreateInfo q={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount=1; q.pQueuePriorities=&pr;
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.bufferDeviceAddress=1;
  const char* ext[]={VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
  VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&f12; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&q; dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=ext;
  VkDevice dev; if(vkCreateDevice(pd,&dci,0,&dev)) return 2;
  VmaVulkanFunctions vf{}; vf.vkGetInstanceProcAddr=vkGetInstanceProcAddr; vf.vkGetDeviceProcAddr=vkGetDeviceProcAddr;
  VmaDeviceMemoryCallbacks cb{onAlloc,onFree,nullptr};
  VmaAllocatorCreateInfo ac{}; ac.flags=VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT|VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  ac.physicalDevice=pd; ac.device=dev; ac.instance=inst; ac.vulkanApiVersion=VK_API_VERSION_1_3; ac.pVulkanFunctions=&vf; ac.pDeviceMemoryCallbacks=&cb;
  VmaAllocator al; if(vmaCreateAllocator(&ac,&al)) return 3;
  // memory type for a representative image
  VkImageCreateInfo ic={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType=VK_IMAGE_TYPE_2D; ic.format=VK_FORMAT_R8G8B8A8_UNORM; ic.extent={64,64,1}; ic.mipLevels=7; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_OPTIMAL; ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo aci{}; aci.usage=VMA_MEMORY_USAGE_UNKNOWN; aci.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  uint32_t mt; if(vmaFindMemoryTypeIndexForImageInfo(al,&ic,&aci,&mt)) return 4;
  VmaPool pools[NC+1];
  for(int c=0;c<=NC;c++){ VmaPoolCreateInfo pc{}; pc.memoryTypeIndex=mt; pc.flags=VMA_POOL_CREATE_IGNORE_BUFFER_IMAGE_GRANULARITY_BIT; pc.blockSize= c<NC? (getenv("SMBLK")? strtoull(getenv("SMBLK"),0,10)<<20 : (16ull<<20)):gMedBlock; if(vmaCreatePool(al,&pc,&pools[c])) return 5; }
  struct Fmt{VkFormat f;bool bc;} fm[]={{VK_FORMAT_BC7_UNORM_BLOCK,1},{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,1},{VK_FORMAT_R8G8B8A8_UNORM,0}};
  int dims[]={4,8,16,32,64,128,256,512,1024};
  std::mt19937 rng(42);
  struct Live{VkImage im;VmaAllocation a;VkDeviceSize req;VkDeviceSize cls;int pool;int d;uint64_t ser;};
  uint64_t serial=0;
  std::vector<Live> live;
  uint64_t dedicated=0, reqSum=0;
  auto mk=[&](int forced=-1){
    int f=rng()%3; int d; uint32_t r=rng()%100; if(getenv("TICKET")) d= r<72? rng()%4 : r<88? 4 : r<96? 5 : r<99? 6 : 7; else d= r<60? rng()%4 : r<80? 4 : r<90? 5 : r<96? 6 : r<99? 7 : 8;
    if(forced>=0) d=forced;
    int mips=1; for(int s=dims[d];s>1;s>>=1) mips++;
    VkImageCreateInfo c=ic; c.format=fm[f].f; c.extent={(uint32_t)dims[d],(uint32_t)dims[d],1}; c.mipLevels=mips; c.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if(fm[f].bc) c.flags|=VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    VkImage im; if(vkCreateImage(dev,&c,0,&im)) return;
    VkMemoryDedicatedRequirements dr{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; mr2.pNext=&dr;
    VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2}; ri.image=im;
    vkGetImageMemoryRequirements2(dev,&ri,&mr2);
    VkMemoryRequirements req=mr2.memoryRequirements;
    if(dr.prefersDedicatedAllocation||dr.requiresDedicatedAllocation) ++dedicated;
    int p=-1; VkDeviceSize cls=req.size;
    for(int k=0;k<NC && !getenv("NOPOOL");k++){ VkDeviceSize cs=(VkDeviceSize)kClassK[k]<<10; if(req.size<=cs && cs%req.alignment==0){p=k;cls=cs;break;} }
    if(p<0){ if(req.size<=gMedMax && !getenv("NOPOOL")) p=NC; else { uint64_t r64=(req.size+65535)/65536*65536; gDed+=r64; gDedN++; live.push_back({im,nullptr,req.size,r64,-1,d,serial++}); reqSum+=req.size; return; } }
    VkMemoryRequirements rq=req; rq.size=cls;
    VmaAllocationCreateInfo a{}; a.pool=pools[p]; a.flags=VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;
    VmaAllocation al2; if(vmaAllocateMemory(al,&rq,&a,&al2,nullptr)){printf("alloc fail\n"); vkDestroyImage(dev,im,0); return;}
    vmaBindImageMemory(al,al2,im);
    live.push_back({im,al2,req.size,cls,p,d,serial++}); reqSum+=req.size;
  };
  auto fr=[&](size_t i){ vkDestroyImage(dev,live[i].im,0); if(live[i].a) vmaFreeMemory(al,live[i].a); else { gDed-=live[i].cls; gDedN--; } reqSum-=live[i].req; live[i]=live.back(); live.pop_back(); };
  auto report=[&](const char* tag){
    uint64_t tb=0,ta=0; uint32_t tbl=0;
    printf("== %s: live %zu req %.1fMB | vkAlloc %llu vkFree %llu liveBlocks %llu (%.1fMB)\n",tag,live.size(),reqSum/1048576.0,(unsigned long long)gAllocCalls,(unsigned long long)gFreeCalls,(unsigned long long)gLiveBlocks,gLiveBlockBytes/1048576.0);
    for(int c=0;c<=NC;c++){ VmaStatistics s; vmaGetPoolStatistics(al,pools[c],&s); tb+=s.blockBytes; ta+=s.allocationBytes; tbl+=s.blockCount;
      if(s.blockCount) printf("  pool %2d (%s%u) blocks %u %.0fMB allocs %u alloc %.1fMB occ %.1f%%\n",c,c<NC?"":"tlsf ",c<NC?kClassK[c]:0,s.blockCount,s.blockBytes/1048576.0,s.allocationCount,s.allocationBytes/1048576.0,s.blockBytes?100.0*s.allocationBytes/s.blockBytes:0); }
    printf("  dedicated %llu %.1fMB | VRAM blocks+ded %.1fMB\n",(unsigned long long)gDedN,gDed/1048576.0,(tb+gDed)/1048576.0); printf("  total blocks %u %.1fMB, class-rounded %.1fMB, live req %.1fMB, slack %.1fMB (free %.1f internal %.1f)\n",tbl,tb/1048576.0,ta/1048576.0,reqSum/1048576.0,(tb-reqSum)/1048576.0,(tb-ta)/1048576.0,(ta-reqSum)/1048576.0);
  };
  {int N=getenv("TICKET")?14000:20000; for(int i=0;i<N;i++) mk();}
  report("after 20k");
  printf("dedicated-preferred %llu\n",(unsigned long long)dedicated);
  // churn: 200k ops replacing random image (mip change)
  for(int i=0;i<100000;i++){ fr(rng()%live.size()); mk(); }
  report("after 100k churn");
  // ladder mip-down: 60% of images re-created one level down (new first, then old freed)
  { size_t k=live.size()*6/10; for(size_t i=0;i<k;i++){ size_t j=rng()%live.size(); int d=live[j].d; if(d>0){ mk(d-1); fr(j);} } }
  report("after mip-down of 60%");
  if(getenv("CORR")){ std::vector<uint64_t> ss; for(auto&l:live) ss.push_back(l.ser); std::nth_element(ss.begin(),ss.begin()+ss.size()/2,ss.end()); uint64_t cut=ss[ss.size()/2]; for(size_t i=live.size();i--;) if(live[i].ser<cut) fr(i); }
  else { size_t k=live.size()*5/10; for(size_t i=0;i<k;i++){ size_t j=rng()%live.size(); fr(j);} }
  report("after unloading 50% (random)");
  for(int i=0;i<20000;i++){ fr(rng()%live.size()); mk(); }
  report("after 20k churn at 40%");
  while(!live.empty()) fr(live.size()-1);
  report("after freeing all");
  for(int c=0;c<=NC;c++) vmaDestroyPool(al,pools[c]);
  vmaDestroyAllocator(al); vkDestroyDevice(dev,0); vkDestroyInstance(inst,0);
}
