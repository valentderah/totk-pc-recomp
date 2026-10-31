// Optional private capability query only. The bridge owns all NGX allocations and shuts them down.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "host.h"
#include "vk.h"
#include "ngx_probe.h"
#include "dlaa_settings.h"
#include "vk_projection_probe.h"
static HMODULE bridge_module;
static int resident_after_failure;
static SRWLOCK sdk_call_lock=SRWLOCK_INIT;
static int eval_pending;
int ngx_probe_sequence_enabled(void){const char *e=getenv("TOTK_NGX_SEQUENCE");return e&&*e&&*e!='0';}
int ngx_probe_eval_enabled(void){const char *e=getenv("TOTK_NGX_EVALUATE");return e&&*e&&*e!='0';}
static HMODULE load_bridge(const char *path){
    if(!bridge_module)bridge_module=LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    return bridge_module;
}
int ngx_probe_device_extensions(VkInstance instance,VkPhysicalDevice physical,char names[][VK_MAX_EXTENSION_NAME_SIZE],unsigned capacity){
    if(resident_after_failure||eval_pending)return -5;
    const char *enabled=getenv("TOTK_NGX_PROBE");if(!enabled||!*enabled||*enabled=='0')return 0;
    const char *bridge=getenv("TOTK_NGX_BRIDGE"),*runtime=getenv("TOTK_NGX_RUNTIME_DIR"),*directory=getenv("TOTK_DUMP_DIR");if(!bridge||!runtime||!directory)return -3;
    char logs[2048],report[2048];snprintf(logs,sizeof logs,"%s/ngx-probe",directory);snprintf(report,sizeof report,"%s/ngx-device-extensions.json",directory);
    if(!CreateDirectoryA(logs,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return -3;
    HMODULE module=load_bridge(bridge);if(!module){rt_log("NGX probe: extension-query bridge load failed %lu",GetLastError());return -3;}
    typedef int (__cdecl *Query)(void*,void*,const char*,const char*,char*,unsigned,const char*);
    Query query=(Query)GetProcAddress(module,"totk_ngx_device_extensions");
    AcquireSRWLockExclusive(&sdk_call_lock);int result=query?query(instance,physical,runtime,logs,(char*)names,capacity,report):-3;ReleaseSRWLockExclusive(&sdk_call_lock);return result;
}
void ngx_probe_once(void){
    static int checked;if(checked)return;checked=1;
    const char *enabled=getenv("TOTK_NGX_PROBE");if(!enabled||!*enabled||*enabled=='0')return;
    if(ngx_probe_eval_enabled()){rt_log("NGX offscreen: evaluation armed; capability-only shutdown deferred");return;}
    const char *bridge=getenv("TOTK_NGX_BRIDGE"),*runtime=getenv("TOTK_NGX_RUNTIME_DIR"),*directory=getenv("TOTK_DUMP_DIR");
    if(!bridge||!runtime||!directory){rt_log("NGX probe: missing private paths");return;}
    char logs[2048],report[2048];snprintf(logs,sizeof logs,"%s/ngx-probe",directory);snprintf(report,sizeof report,"%s/ngx-capabilities.json",directory);
    if(!CreateDirectoryA(logs,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS){rt_log("NGX probe: private log directory unavailable");return;}
    HMODULE module=load_bridge(bridge);
    if(!module){rt_log("NGX probe: bridge load failed %lu",GetLastError());return;}
    typedef int (__cdecl *Probe)(void*,void*,void*,void*,void*,const char*,const char*,const char*);
    Probe probe=(Probe)GetProcAddress(module,"totk_ngx_capability_probe");
    if(probe){VkCtx *ctx=vk_ctx();int result=probe(ctx->instance,ctx->phys,ctx->device,(void*)vkGetInstanceProcAddr,(void*)vkGetDeviceProcAddr,runtime,logs,report);rt_log("NGX probe: capability query returned %d; no feature created or evaluated",result);}
    else rt_log("NGX probe: bridge entry point missing");
    typedef int (__cdecl *UnloadSafe)(void);
    UnloadSafe may_unload=(UnloadSafe)GetProcAddress(module,"totk_ngx_can_unload");
    if(may_unload&&may_unload()){FreeLibrary(module);bridge_module=NULL;}
    else {resident_after_failure=1;rt_log("NGX probe: shutdown not confirmed; keep bridge and callbacks resident until process exit");}
}
static int record_status(int result){
    const char *preview=getenv("TOTK_DLAA_PREVIEW");
    if(preview&&*preview&&*preview!='0')dlaa_settings_evaluated(result);
    return result;
}
int ngx_probe_record_eval(const TotkNgxEvalFrame *frame){
    if(!ngx_probe_eval_enabled()||resident_after_failure||(eval_pending&&!ngx_probe_sequence_enabled()))return record_status(-5);
    const char *bridge=getenv("TOTK_NGX_BRIDGE"),*runtime=getenv("TOTK_NGX_RUNTIME_DIR"),*directory=getenv("TOTK_DUMP_DIR");if(!bridge||!runtime||!directory)return record_status(-3);
    char logs[2048],report[2048];snprintf(logs,sizeof logs,"%s/ngx-probe",directory);snprintf(report,sizeof report,"%s/ngx-evaluate.json",directory);
    HMODULE module=load_bridge(bridge);if(!module)return record_status(-3);
    typedef int (__cdecl *Record)(void*,void*,void*,void*,void*,const char*,const char*,const char*,const TotkNgxEvalFrame*);
    Record record=(Record)GetProcAddress(module,vk_projection_probe_sequence_enabled()?"totk_ngx_record_dlaa_jitter_sequence":ngx_probe_sequence_enabled()?"totk_ngx_record_dlaa_sequence":"totk_ngx_record_dlaa");if(!record)return record_status(-3);
    const char *preview=getenv("TOTK_DLAA_PREVIEW");
    if(!eval_pending&&preview&&*preview&&*preview!='0'){
        typedef int (__cdecl *Configure)(unsigned);Configure configure=(Configure)GetProcAddress(module,"totk_ngx_configure_replay_preset");
        unsigned preset=dlaa_settings_requested();
        if(!configure||configure(preset)!=1)return record_status(-4);
        rt_log("Live DLAA: requested preset %s",preset==11?"K":"L");
    }
    VkCtx *ctx=vk_ctx();AcquireSRWLockExclusive(&sdk_call_lock);eval_pending=1;
    int result=record(ctx->instance,ctx->phys,ctx->device,(void*)vkGetInstanceProcAddr,(void*)vkGetDeviceProcAddr,runtime,logs,report,frame);
    ReleaseSRWLockExclusive(&sdk_call_lock);
    return record_status(result);
}
int ngx_probe_finish_eval(void){
    if(!eval_pending)return 1;
    typedef int (__cdecl *Finish)(void);Finish finish=(Finish)GetProcAddress(bridge_module,"totk_ngx_finish_dlaa");
    AcquireSRWLockExclusive(&sdk_call_lock);int result=finish?finish():0;
    typedef int (__cdecl *UnloadSafe)(void);UnloadSafe may_unload=(UnloadSafe)GetProcAddress(bridge_module,"totk_ngx_can_unload");
    if(result==1&&may_unload&&may_unload()){FreeLibrary(bridge_module);bridge_module=NULL;eval_pending=0;}
    else {resident_after_failure=1;result=0;}
    ReleaseSRWLockExclusive(&sdk_call_lock);return result;
}
