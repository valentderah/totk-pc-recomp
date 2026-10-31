// MSVC-only ABI boundary to the official NGX SDK. Evaluation is an explicit one-shot offscreen test.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include "ngx_eval_abi.h"
static SRWLOCK log_lock=SRWLOCK_INIT;
static FILE *sdk_log;
static int unload_safe=1;
static unsigned replay_preset;
static int profile_cpu(void){static int mode=-1;if(mode<0){const char *v=getenv("TOTK_DLAA_CPU_STATS");mode=v&&*v=='1';}return mode;}
static uint64_t profile_ticks(void){if(!profile_cpu())return 0;LARGE_INTEGER t;QueryPerformanceCounter(&t);return (uint64_t)t.QuadPart;}
static void profile_eval(uint64_t api,uint64_t io,unsigned frames){
    if(!profile_cpu())return;static uint64_t sum_api,sum_io;static unsigned count;sum_api+=api;sum_io+=io;count++;
    if(count==120){LARGE_INTEGER f;QueryPerformanceFrequency(&f);fprintf(stderr,"DLAA SDK CPU: frame %u: evaluate %.3f ms/frame, reports %.3f ms/frame over %u calls\n",frames,1000.0*sum_api/f.QuadPart/count,1000.0*sum_io/f.QuadPart/count,count);sum_api=sum_io=count=0;}
}
static wchar_t session_runtime[2048],session_logs[2048];
static const wchar_t *session_paths[1];
static NVSDK_NGX_FeatureCommonInfo session_info;
static int validate_frame(const TotkNgxEvalFrame *f,bool sequence,bool jitter=false){
    if(!f)return -3;
    if(f->abi_version!=TOTK_NGX_EVAL_ABI||f->struct_size!=sizeof *f||!f->width||!f->height||f->width>8192||f->height>8192||!f->command_buffer||(sequence?f->reset>1:f->reset!=1)||f->reserved)return -4;
    if(!isfinite(f->jitter_x)||!isfinite(f->jitter_y)||(jitter?(fabsf(f->jitter_x)>.5f||fabsf(f->jitter_y)>.5f):(f->jitter_x!=0||f->jitter_y!=0))||!isfinite(f->frame_time_ms)||f->frame_time_ms<=0||f->frame_time_ms>1000)return -4;
    const VkFormat expected[5]={VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_D32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
    for(int i=0;i<5;i++)if(!f->image[i]||!f->view[i]||f->format[i]!=(uint32_t)expected[i])return -4;
    for(int i=0;i<4;i++)if(f->image[4]==f->image[i]||f->view[4]==f->view[i])return -4;
    return 0;
}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_validate_eval_frame(const TotkNgxEvalFrame *f){return validate_frame(f,false);}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_validate_sequence_frame(const TotkNgxEvalFrame *f){return validate_frame(f,true);}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_validate_jitter_sequence_frame(const TotkNgxEvalFrame *f){return validate_frame(f,true,true);}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_can_unload(void){return unload_safe;}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_device_extensions(void *instance,void *physical,const char *runtime_dir,const char *log_dir,char *names,unsigned capacity,const char *report){
    if(!instance||!physical||!runtime_dir||!log_dir||!names||!capacity||!report)return -3;
    wchar_t runtime[2048],logs[2048];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,runtime_dir,-1,runtime,2048)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,log_dir,-1,logs,2048))return -3;
    const wchar_t *paths[]={runtime};NVSDK_NGX_FeatureCommonInfo common={};common.PathListInfo.Path=paths;common.PathListInfo.Length=1;
    NVSDK_NGX_FeatureDiscoveryInfo discovery={};discovery.SDKVersion=NVSDK_NGX_Version_API;discovery.FeatureID=NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType=NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc={"0959d38d-8841-4e52-866c-108c2e6b537e",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"TotK-local-probe-1"};
    discovery.ApplicationDataPath=logs;discovery.FeatureInfo=&common;
    uint32_t count=0;VkExtensionProperties *properties=nullptr;
    NVSDK_NGX_Result result=NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements((VkInstance)instance,(VkPhysicalDevice)physical,&discovery,&count,&properties);
    FILE *file=nullptr;fopen_s(&file,report,"w");if(file)fprintf(file,"{\"query_result\":%u,\"count\":%u,\"extensions\":[",(unsigned)result,count);
    int valid=!NVSDK_NGX_FAILED(result)&&count<=capacity&&(!count||properties);
    if(valid)for(uint32_t i=0;i<count;i++){
        if(!memchr(properties[i].extensionName,0,VK_MAX_EXTENSION_NAME_SIZE)){valid=0;break;}
        memcpy(names+i*VK_MAX_EXTENSION_NAME_SIZE,properties[i].extensionName,VK_MAX_EXTENSION_NAME_SIZE);
        if(file)fprintf(file,"%s\"%s\"",i?",":"",properties[i].extensionName);
    }
    if(file){fprintf(file,"]}\n");fclose(file);}return valid?(int)count:-4;
}
static void NVSDK_CONV log_message(const char *message,NVSDK_NGX_Logging_Level level,NVSDK_NGX_Feature feature){
    AcquireSRWLockExclusive(&log_lock);if(sdk_log){fprintf(sdk_log,"[%d:%d] %s\n",(int)feature,(int)level,message);fflush(sdk_log);}ReleaseSRWLockExclusive(&log_lock);
}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_capability_probe(void *instance,void *physical,void *device,void *gipa,void *gdpa,const char *runtime_dir,const char *log_dir,const char *report){
    if(!instance||!physical||!device||!gipa||!gdpa||!runtime_dir||!log_dir||!report)return -3;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,runtime_dir,-1,session_runtime,2048)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,log_dir,-1,session_logs,2048))return -3;
    FILE *output=nullptr;if(fopen_s(&output,report,"w")||!output)return -2;
    char sdk_path[4096];snprintf(sdk_path,sizeof sdk_path,"%s/ngx-sdk.log",log_dir);fopen_s(&sdk_log,sdk_path,"w");
    session_paths[0]=session_runtime;memset(&session_info,0,sizeof session_info);session_info.PathListInfo.Path=session_paths;session_info.PathListInfo.Length=1;
    session_info.LoggingInfo.LoggingCallback=log_message;session_info.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_VERBOSE;session_info.LoggingInfo.DisableOtherLoggingSinks=true;
    unload_safe=0; // Once SDK initialization is attempted, keep callbacks resident until shutdown is confirmed.
    NVSDK_NGX_Result init=NVSDK_NGX_VULKAN_Init_with_ProjectID("0959d38d-8841-4e52-866c-108c2e6b537e",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"TotK-local-probe-1",session_logs,
        (VkInstance)instance,(VkPhysicalDevice)physical,(VkDevice)device,(PFN_vkGetInstanceProcAddr)gipa,(PFN_vkGetDeviceProcAddr)gdpa,&session_info,NVSDK_NGX_Version_API);
    NVSDK_NGX_Result caps=NVSDK_NGX_Result_Fail,query=NVSDK_NGX_Result_Fail,destroy=NVSDK_NGX_Result_Fail,shutdown=NVSDK_NGX_Result_Fail;
    int available=-1,needs_driver=-1,feature_init=-1;NVSDK_NGX_Parameter *parameters=nullptr;
    if(!NVSDK_NGX_FAILED(init)){
        caps=NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
        if(!NVSDK_NGX_FAILED(caps)&&parameters){
            query=NVSDK_NGX_Parameter_GetI(parameters,NVSDK_NGX_Parameter_SuperSampling_Available,&available);
            NVSDK_NGX_Parameter_GetI(parameters,NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,&needs_driver);
            NVSDK_NGX_Parameter_GetI(parameters,NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,&feature_init);
            destroy=NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        }
        shutdown=NVSDK_NGX_VULKAN_Shutdown1((VkDevice)device);
        if(!NVSDK_NGX_FAILED(shutdown))unload_safe=1;
    }
    fprintf(output,"{\"sdk_api\":%u,\"init_result\":%u,\"capability_result\":%u,\"availability_query_result\":%u,\"super_sampling_available\":%d,\"needs_updated_driver\":%d,\"feature_init_result\":%d,\"destroy_result\":%u,\"shutdown_result\":%u,\"feature_created\":false,\"evaluated\":false,\"dlaa_enabled\":false}\n",
        (unsigned)NVSDK_NGX_Version_API,(unsigned)init,(unsigned)caps,(unsigned)query,available,needs_driver,feature_init,(unsigned)destroy,(unsigned)shutdown);fclose(output);
    AcquireSRWLockExclusive(&log_lock);if(sdk_log)fclose(sdk_log);sdk_log=nullptr;ReleaseSRWLockExclusive(&log_lock);
    return !NVSDK_NGX_FAILED(init)&&!NVSDK_NGX_FAILED(caps)&&!NVSDK_NGX_FAILED(query)&&available==1&&!NVSDK_NGX_FAILED(destroy)&&!NVSDK_NGX_FAILED(shutdown)?1:0;
}

static struct {
    int buffered_reports;FILE *sequence_log,*jitter_log;
    VkDevice device;NVSDK_NGX_Handle *handle;NVSDK_NGX_Parameter *parameters;
    NVSDK_NGX_Resource_VK resources[5];char report[4096];unsigned width,height,frames,resets,creates,last_reset;int available,initialized,finished,jitter_mode;unsigned preset_requested,preset_echo;NVSDK_NGX_Result preset_query;float jitter_x,jitter_y;
    NVSDK_NGX_Result init,caps,query,caps_destroy,allocate,create,evaluate,release,destroy,shutdown;
} E;
extern "C" __declspec(dllexport) int __cdecl totk_ngx_configure_replay_preset(unsigned preset){
    if(E.initialized||!unload_safe)return -5;
    if(preset!=0&&preset!=NVSDK_NGX_DLSS_Hint_Render_Preset_K&&preset!=NVSDK_NGX_DLSS_Hint_Render_Preset_L)return -4;
    replay_preset=preset;return 1;
}
static void eval_report(void){
    if(E.buffered_reports&&E.frames>3&&!E.last_reset&&!E.finished&&E.evaluate==NVSDK_NGX_Result_Success&&E.frames%120)return;
    FILE *file=nullptr;if(fopen_s(&file,E.report,"w")||!file)return;
    fprintf(file,"{\"width\":%u,\"height\":%u,\"available\":%d,\"init\":%u,\"caps\":%u,\"query\":%u,\"caps_destroy\":%u,\"allocate\":%u,\"create\":%u,\"evaluate\":%u,\"release\":%u,\"destroy\":%u,\"shutdown\":%u,\"frames\":%u,\"resets\":%u,\"creates\":%u,\"finished\":%s,\"reset\":%s,\"jitter\":[%.9g,%.9g],\"motion_vectors_jittered\":%s,\"presented\":false,\"temporal_quality_validated\":false,\"preset_requested\":%u,\"preset_echo\":%u,\"preset_query\":%u}\n",
        E.width,E.height,E.available,(unsigned)E.init,(unsigned)E.caps,(unsigned)E.query,(unsigned)E.caps_destroy,(unsigned)E.allocate,(unsigned)E.create,(unsigned)E.evaluate,(unsigned)E.release,(unsigned)E.destroy,(unsigned)E.shutdown,E.frames,E.resets,E.creates,E.finished?"true":"false",E.last_reset?"true":"false",E.jitter_x,E.jitter_y,E.jitter_mode?"true":"false",E.preset_requested,E.preset_echo,(unsigned)E.preset_query);fclose(file);
}
static int record_dlaa(void *instance,void *physical,void *device,void *gipa,void *gdpa,const char *runtime_dir,const char *log_dir,const char *report,const TotkNgxEvalFrame *frame,bool sequence,bool jitter=false){
    int validation=validate_frame(frame,sequence,jitter);if(validation)return validation;
    if(replay_preset&&!jitter)return -5;
    if(!instance||!physical||!device||!gipa||!gdpa||!runtime_dir||!log_dir||!report)return -3;
    if(E.initialized){
        if(E.jitter_mode!=(int)jitter||!sequence||E.finished||!E.handle||E.evaluate!=NVSDK_NGX_Result_Success||E.device!=(VkDevice)device||E.width!=frame->width||E.height!=frame->height)return -5;
    }else{
    if(!unload_safe||frame->reset!=1)return -5;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,runtime_dir,-1,session_runtime,2048)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,log_dir,-1,session_logs,2048))return -3;
    E={};const char *buffered=getenv("TOTK_NGX_BUFFERED_REPORTS");E.buffered_reports=buffered&&*buffered=='1';E.preset_requested=replay_preset;E.preset_query=NVSDK_NGX_Result_Success;E.jitter_mode=jitter;E.device=(VkDevice)device;E.width=frame->width;E.height=frame->height;strncpy_s(E.report,report,_TRUNCATE);
    E.init=E.caps=E.query=E.caps_destroy=E.allocate=E.create=E.evaluate=E.release=E.destroy=E.shutdown=NVSDK_NGX_Result_Fail;
    char path[4096];snprintf(path,sizeof path,"%s/ngx-evaluate-sdk.log",log_dir);fopen_s(&sdk_log,path,"w");
    session_paths[0]=session_runtime;session_info={};session_info.PathListInfo.Path=session_paths;session_info.PathListInfo.Length=1;
    session_info.LoggingInfo.LoggingCallback=log_message;session_info.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_VERBOSE;session_info.LoggingInfo.DisableOtherLoggingSinks=true;
    unload_safe=0;
    E.init=NVSDK_NGX_VULKAN_Init_with_ProjectID("0959d38d-8841-4e52-866c-108c2e6b537e",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"TotK-offscreen-1",session_logs,
        (VkInstance)instance,(VkPhysicalDevice)physical,E.device,(PFN_vkGetInstanceProcAddr)gipa,(PFN_vkGetDeviceProcAddr)gdpa,&session_info,NVSDK_NGX_Version_API);
    if(NVSDK_NGX_FAILED(E.init)){eval_report();return 0;}E.initialized=1;
    NVSDK_NGX_Parameter *caps=nullptr;E.caps=NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps);
    if(!NVSDK_NGX_FAILED(E.caps)&&caps){E.query=NVSDK_NGX_Parameter_GetI(caps,NVSDK_NGX_Parameter_SuperSampling_Available,&E.available);E.caps_destroy=NVSDK_NGX_VULKAN_DestroyParameters(caps);}
    if(E.available!=1||NVSDK_NGX_FAILED(E.query)){eval_report();return 0;}
    E.allocate=NVSDK_NGX_VULKAN_AllocateParameters(&E.parameters);if(NVSDK_NGX_FAILED(E.allocate)||!E.parameters){eval_report();return 0;}
    if(E.preset_requested){
        NVSDK_NGX_Parameter_SetUI(E.parameters,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,E.preset_requested);
        E.preset_query=NVSDK_NGX_Parameter_GetUI(E.parameters,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,&E.preset_echo);
        if(NVSDK_NGX_FAILED(E.preset_query)||E.preset_echo!=E.preset_requested){eval_report();return 0;}
    }
    NVSDK_NGX_DLSS_Create_Params create={};create.Feature.InWidth=E.width;create.Feature.InHeight=E.height;create.Feature.InTargetWidth=E.width;create.Feature.InTargetHeight=E.height;create.Feature.InPerfQualityValue=NVSDK_NGX_PerfQuality_Value_DLAA;
    create.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if(jitter)create.InFeatureCreateFlags|=NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    VkCommandBuffer cb=(VkCommandBuffer)(uintptr_t)frame->command_buffer;
    E.creates++;E.create=NGX_VULKAN_CREATE_DLSS_EXT1(E.device,cb,1,1,&E.handle,E.parameters,&create);
    if(NVSDK_NGX_FAILED(E.create)||!E.handle){eval_report();return 0;}
    } // Initialize/create only once; later frames reuse the same feature and parameters.
    VkCommandBuffer cb=(VkCommandBuffer)(uintptr_t)frame->command_buffer;
    for(int i=0;i<5;i++){
        VkImageSubresourceRange range={(VkImageAspectFlags)(i==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        E.resources[i]=NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)(uintptr_t)frame->view[i],(VkImage)(uintptr_t)frame->image[i],range,(VkFormat)frame->format[i],E.width,E.height,i==4);
    }
    NVSDK_NGX_VK_DLSS_Eval_Params evaluate={};evaluate.Feature.pInColor=&E.resources[0];evaluate.Feature.pInOutput=&E.resources[4];evaluate.pInDepth=&E.resources[1];evaluate.pInMotionVectors=&E.resources[2];evaluate.pInBiasCurrentColorMask=&E.resources[3];
    evaluate.InRenderSubrectDimensions={E.width,E.height};evaluate.InReset=frame->reset;evaluate.InMVScaleX=1;evaluate.InMVScaleY=1;evaluate.InPreExposure=1;evaluate.InExposureScale=1;evaluate.InFrameTimeDeltaInMsec=frame->frame_time_ms;
    evaluate.InJitterOffsetX=frame->jitter_x;evaluate.InJitterOffsetY=frame->jitter_y;E.jitter_x=frame->jitter_x;E.jitter_y=frame->jitter_y;
    uint64_t api_start=profile_ticks();E.evaluate=NGX_VULKAN_EVALUATE_DLSS_EXT(cb,E.handle,E.parameters,&evaluate);uint64_t api_end=profile_ticks();E.frames++;E.resets+=frame->reset;E.last_reset=frame->reset;eval_report();
    if(sequence){char path[4096];snprintf(path,sizeof path,"%s/ngx-sequence-sdk.jsonl",log_dir);FILE *file=E.sequence_log;
        if(!file){fopen_s(&file,path,"a");if(E.buffered_reports&&file){setvbuf(file,nullptr,_IOFBF,65536);E.sequence_log=file;}}
        if(file){fprintf(file,"{\"index\":%u,\"reset\":%u,\"evaluate\":%u,\"creates\":%u}\n",E.frames,frame->reset,(unsigned)E.evaluate,E.creates);if(!E.buffered_reports)fclose(file);else if(E.frames%120==0)fflush(file);}}
    if(jitter){char path[4096];snprintf(path,sizeof path,"%s/ngx-raster-jitter-sdk.jsonl",log_dir);FILE *file=E.jitter_log;
        if(!file){fopen_s(&file,path,"a");if(E.buffered_reports&&file){setvbuf(file,nullptr,_IOFBF,65536);E.jitter_log=file;}}
        if(file){fprintf(file,"{\"index\":%u,\"jitter\":[%.9g,%.9g],\"motion_vectors_jittered\":true}\n",E.frames,frame->jitter_x,frame->jitter_y);if(!E.buffered_reports)fclose(file);else if(E.frames%120==0)fflush(file);}}
    profile_eval(api_end-api_start,profile_ticks()-api_end,E.frames);
    return NVSDK_NGX_FAILED(E.evaluate)?0:1;
}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_record_dlaa(void *instance,void *physical,void *device,void *gipa,void *gdpa,const char *runtime_dir,const char *log_dir,const char *report,const TotkNgxEvalFrame *frame){return record_dlaa(instance,physical,device,gipa,gdpa,runtime_dir,log_dir,report,frame,false);}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_record_dlaa_sequence(void *instance,void *physical,void *device,void *gipa,void *gdpa,const char *runtime_dir,const char *log_dir,const char *report,const TotkNgxEvalFrame *frame){return record_dlaa(instance,physical,device,gipa,gdpa,runtime_dir,log_dir,report,frame,true);}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_record_dlaa_jitter_sequence(void *instance,void *physical,void *device,void *gipa,void *gdpa,const char *runtime_dir,const char *log_dir,const char *report,const TotkNgxEvalFrame *frame){return record_dlaa(instance,physical,device,gipa,gdpa,runtime_dir,log_dir,report,frame,true,true);}
extern "C" __declspec(dllexport) int __cdecl totk_ngx_finish_dlaa(void){
    if(E.finished)return unload_safe?1:0;
    if(E.handle){E.release=NVSDK_NGX_VULKAN_ReleaseFeature(E.handle);E.handle=nullptr;}
    if(E.parameters){E.destroy=NVSDK_NGX_VULKAN_DestroyParameters(E.parameters);E.parameters=nullptr;}
    if(E.initialized){E.shutdown=NVSDK_NGX_VULKAN_Shutdown1(E.device);if(E.shutdown==NVSDK_NGX_Result_Success)unload_safe=1;}
    E.finished=1;eval_report();
    if(E.sequence_log){fclose(E.sequence_log);E.sequence_log=nullptr;}
    if(E.jitter_log){fclose(E.jitter_log);E.jitter_log=nullptr;}
    AcquireSRWLockExclusive(&log_lock);if(sdk_log)fclose(sdk_log);sdk_log=nullptr;ReleaseSRWLockExclusive(&log_lock);
    return unload_safe&&(!E.initialized||E.shutdown==NVSDK_NGX_Result_Success)?1:0;
}
