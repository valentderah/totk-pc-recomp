// Local, windowless, same-input SDK replay. No game or emulator process is accessed.
#define WIN32_LEAN_AND_MEAN
#define VMA_IMPLEMENTATION
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "ngx_eval_abi.h"
namespace fs=std::filesystem;
static LONG validation_errors,validation_warnings;
static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *data,void*){
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)InterlockedIncrement(&validation_errors);
    else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)InterlockedIncrement(&validation_warnings);
    fprintf(stderr,"Vulkan: %s\n",data->pMessage);return VK_FALSE;
}
static void check(VkResult r,const char *what){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(what)+": "+std::to_string(r));}
static void require(bool value,const char *what){if(!value)throw std::runtime_error(what);}
struct Row {uint64_t frame;float x,y,ms;std::array<std::string,4> files;};
using Query=int(__cdecl*)(void*,void*,const char*,const char*,char*,unsigned,const char*);
using Configure=int(__cdecl*)(unsigned);
using Record=int(__cdecl*)(void*,void*,void*,void*,void*,const char*,const char*,const char*,const TotkNgxEvalFrame*);
using Finish=int(__cdecl*)();
struct Image {VkImage image{};VkImageView view{};VmaAllocation allocation{};};
struct Replay {
    HMODULE module{};Query query{};Configure configure{};Record record{};Finish finish{},can_unload{};
    VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    VkCommandPool pool{};VkCommandBuffer cb{};VkFence fence{};VmaAllocator allocator{};VkBuffer staging{};VmaAllocation staging_allocation{};void *mapped{};
    std::array<Image,5> images{};bool sdk_started{},closed{},unsafe{};std::string gpu;
    void close(){
        if(closed||unsafe)return;
        if(device){VkResult idle=vkDeviceWaitIdle(device);if(idle!=VK_SUCCESS){unsafe=true;check(idle,"device idle before cleanup");}}
        if(sdk_started){if(finish()!=1||can_unload()!=1){unsafe=true;throw std::runtime_error("SDK cleanup unconfirmed; retain resources until process exit");}sdk_started=false;}
        if(module&&can_unload&&can_unload()!=1){unsafe=true;throw std::runtime_error("Bridge not unloadable; retain resources until process exit");}
        if(device){for(auto &i:images){if(i.view)vkDestroyImageView(device,i.view,nullptr);if(i.image)vmaDestroyImage(allocator,i.image,i.allocation);}
            if(staging)vmaDestroyBuffer(allocator,staging,staging_allocation);if(allocator)vmaDestroyAllocator(allocator);
            if(fence)vkDestroyFence(device,fence,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);device={};}
        if(messenger){auto destroy=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");destroy(instance,messenger,nullptr);}
        if(instance)vkDestroyInstance(instance,nullptr);if(module)FreeLibrary(module);closed=true;
    }
    ~Replay(){try{close();}catch(const std::exception &e){fprintf(stderr,"Cleanup: %s\n",e.what());}}
    void open(const std::string &bridge,const std::string &runtime,const fs::path &out,bool validation){
        module=LoadLibraryExA(bridge.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);require(module,"Bridge load failed");
        query=(Query)GetProcAddress(module,"totk_ngx_device_extensions");configure=(Configure)GetProcAddress(module,"totk_ngx_configure_replay_preset");
        record=(Record)GetProcAddress(module,"totk_ngx_record_dlaa_jitter_sequence");finish=(Finish)GetProcAddress(module,"totk_ngx_finish_dlaa");can_unload=(Finish)GetProcAddress(module,"totk_ngx_can_unload");
        require(query&&configure&&record&&finish&&can_unload,"Bridge entry point missing");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="TotK same-input replay";app.apiVersion=VK_API_VERSION_1_3;
        const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=diagnostic;
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;
        if(validation){ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&extension;ic.pNext=&debug;}
        check(vkCreateInstance(&ic,nullptr,&instance),"instance");
        if(validation){auto create=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");require(create,"Debug callback unavailable");check(create(instance,&debug,nullptr,&messenger),"debug messenger");}
        uint32_t count=0;check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"GPU count");std::vector<VkPhysicalDevice> devices(count);check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"GPUs");
        for(auto d:devices){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(d,&p);if(p.vendorID==0x10de){physical=d;gpu=p.deviceName;break;}}require(physical,"NVIDIA GPU unavailable");
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());uint32_t family=n;
        for(uint32_t i=0;i<n;i++)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}require(family<n,"Graphics queue missing");
        char names[16][VK_MAX_EXTENSION_NAME_SIZE]{};int required=query(instance,physical,runtime.c_str(),out.string().c_str(),(char*)names,16,(out/"extensions.json").string().c_str());require(required>=0&&required<=16,"SDK extension query failed");std::vector<const char*> extensions;
        for(int i=0;i<required;i++)extensions.push_back(names[i]);
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&f11;f11.pNext=&f12;f12.pNext=&f13;vkGetPhysicalDeviceFeatures2(physical,&features);require(f13.synchronization2&&f12.bufferDeviceAddress,"Required core features missing");
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;dc.enabledExtensionCount=(uint32_t)extensions.size();dc.ppEnabledExtensionNames=extensions.data();check(vkCreateDevice(physical,&dc,nullptr,&device),"device");vkGetDeviceQueue(device,family,0,&queue);
        VmaAllocatorCreateInfo ac{};ac.instance=instance;ac.physicalDevice=physical;ac.device=device;ac.vulkanApiVersion=VK_API_VERSION_1_3;check(vmaCreateAllocator(&ac,&allocator),"allocator");
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;check(vkCreateCommandPool(device,&pc,nullptr,&pool),"command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(device,&ca,&cb),"command buffer");VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fc,nullptr,&fence),"fence");
    }
    void allocate(uint32_t w,uint32_t h,const std::array<VkFormat,5> &formats,size_t bytes){
        for(int i=0;i<5;i++){
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=formats[i];ci.extent={w,h,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;if(i==4)ci.usage|=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;check(vmaCreateImage(allocator,&ci,&ai,&images[i].image,&images[i].allocation,nullptr),"image");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=images[i].image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=formats[i];vi.subresourceRange={(VkImageAspectFlags)(i==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};check(vkCreateImageView(device,&vi,nullptr,&images[i].view),"view");
        }
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;VmaAllocationCreateInfo ai{};ai.usage=VMA_MEMORY_USAGE_AUTO;ai.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;VmaAllocationInfo info;check(vmaCreateBuffer(allocator,&bi,&ai,&staging,&staging_allocation,&info),"staging");mapped=info.pMappedData;
    }
    void barrier(int i,VkImageLayout old,VkImageLayout next){
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=old==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[i].image;b.subresourceRange={(VkImageAspectFlags)(i==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};info.imageMemoryBarrierCount=1;info.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cb,&info);
    }
};
int main(int argc,char **argv){
    Replay r;
    try{
        require(argc==7,"usage: replay manifest.tsv bridge.dll runtime-dir output-dir preset(0,11,12) validation(0,1)");fs::path out=argv[4];require(!fs::exists(out),"Output already exists");fs::create_directories(out);
        unsigned preset=(unsigned)std::stoul(argv[5]);require(preset==0||preset==11||preset==12,"Preset outside experiment scope");
        std::ifstream manifest(argv[1]);uint32_t w=0,h=0,n=0;manifest>>w>>h>>n;require(w==2560&&h==1440&&(n==8||n==32),"Expected native bounded manifest");std::vector<Row> rows;
        for(unsigned i=0;i<n;i++){Row row{};manifest>>row.frame>>row.x>>row.y>>row.ms;for(auto &p:row.files)manifest>>std::quoted(p);require((bool)manifest&&std::isfinite(row.x)&&std::isfinite(row.y)&&fabs(row.x)<=.5&&fabs(row.y)<=.5&&std::isfinite(row.ms)&&row.ms>0&&row.ms<=1000,"Invalid manifest row");if(i)require(row.frame==rows.back().frame+1,"Frame gap");rows.push_back(row);}std::string extra;require(!(manifest>>extra),"Unexpected manifest trailing data");
        r.open(fs::absolute(argv[2]).string(),fs::absolute(argv[3]).string(),fs::absolute(out),std::stoi(argv[6])!=0);require(r.configure(preset)==1,"Replay preset rejected");
        const std::array<VkFormat,5> formats={VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_D32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
        size_t pixels=(size_t)w*h;std::array<size_t,4> sizes={pixels*4,pixels*4,pixels*8,pixels*4};std::array<size_t,4> offsets={0,sizes[0],sizes[0]+sizes[1],sizes[0]+sizes[1]+sizes[2]};size_t output_offset=pixels*20,output_bytes=pixels*8;r.allocate(w,h,formats,output_offset+output_bytes);
        std::ofstream events(out/"replay-frames.jsonl");std::string runtime=fs::absolute(argv[3]).string(),logs=fs::absolute(out).string(),report=(fs::absolute(out)/"ngx-evaluate.json").string();
        for(unsigned k=0;k<n;k++){
            const Row &row=rows[k];for(int i=0;i<4;i++){std::ifstream input(row.files[i],std::ios::binary|std::ios::ate);require(input&&input.tellg()==(std::streamoff)sizes[i],"Missing or wrong-size input");input.seekg(0);input.read((char*)r.mapped+offsets[i],sizes[i]);require((bool)input,"Incomplete input read");}check(vmaFlushAllocation(r.allocator,r.staging_allocation,0,VK_WHOLE_SIZE),"flush input");
            check(vkResetCommandBuffer(r.cb,0),"reset CB");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(r.cb,&begin),"begin CB");
            for(int i=0;i<4;i++){r.barrier(i,k?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkBufferImageCopy copy{};copy.bufferOffset=offsets[i];copy.imageSubresource={(VkImageAspectFlags)(i==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,0,1};copy.imageExtent={w,h,1};vkCmdCopyBufferToImage(r.cb,r.staging,r.images[i].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);r.barrier(i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);}
            r.barrier(4,k?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(r.cb,r.images[4].image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);r.barrier(4,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            TotkNgxEvalFrame f{};f.abi_version=TOTK_NGX_EVAL_ABI;f.struct_size=sizeof f;f.width=w;f.height=h;f.command_buffer=(uint64_t)(uintptr_t)r.cb;f.reset=k==0;f.jitter_x=row.x;f.jitter_y=row.y;f.frame_time_ms=row.ms;
            for(int i=0;i<5;i++){f.image[i]=(uint64_t)(uintptr_t)r.images[i].image;f.view[i]=(uint64_t)(uintptr_t)r.images[i].view;f.format[i]=formats[i];}
            r.sdk_started=true;int result=r.record(r.instance,r.physical,r.device,(void*)vkGetInstanceProcAddr,(void*)vkGetDeviceProcAddr,runtime.c_str(),logs.c_str(),report.c_str(),&f);require(result==1,"SDK record failed");
            if(k==0)require(r.configure(preset==11?12:11)==-5,"Preset configuration changed during a live SDK session");
            r.barrier(4,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy read{};read.bufferOffset=output_offset;read.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};read.imageExtent={w,h,1};vkCmdCopyImageToBuffer(r.cb,r.images[4].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.staging,1,&read);r.barrier(4,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};mb.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;mb.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;mb.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;mb.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};di.memoryBarrierCount=1;di.pMemoryBarriers=&mb;vkCmdPipelineBarrier2(r.cb,&di);check(vkEndCommandBuffer(r.cb),"end CB");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&r.cb;check(vkResetFences(r.device,1,&r.fence),"reset fence");check(vkQueueSubmit(r.queue,1,&si,r.fence),"submit");check(vkWaitForFences(r.device,1,&r.fence,VK_TRUE,60000000000ull),"GPU completion");check(vmaInvalidateAllocation(r.allocator,r.staging_allocation,0,VK_WHOLE_SIZE),"readback");
            std::ofstream output(out/("ngx-output-"+std::to_string(row.frame)+"-2560x1440.rgba16f"),std::ios::binary);output.write((char*)r.mapped+output_offset,output_bytes);require((bool)output,"Output export failed");events<<"{\"index\":"<<k+1<<",\"frame\":"<<row.frame<<",\"reset\":"<<(k==0?1:0)<<",\"gpu_completion_observed\":true}\n";
        }
        require((bool)events,"Replay event export failed");events.close();std::string gpu=r.gpu;r.close();
        std::ofstream summary(out/"replay-summary.json");summary<<"{\"frames\":"<<n<<",\"preset_requested\":"<<preset<<",\"gpu\":"<<std::quoted(gpu)<<",\"gpu_completion_observed\":true,\"sdk_cleanup\":1,\"validation_errors\":"<<validation_errors<<",\"validation_warnings\":"<<validation_warnings<<",\"presented\":false}\n";require((bool)summary,"Summary export failed");require(validation_errors==0,"Vulkan errors observed");return 0;
    }catch(const std::exception &e){fprintf(stderr,"Replay failed: %s\n",e.what());return 1;}
}
