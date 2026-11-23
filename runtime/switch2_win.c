// Ported from adriadam10/mkw-recomp (MIT, Copyright (c) 2026 Adrià), runtime/input/switch2_win.c at eb8fe198 through dkb-pc-recomp; TOTK_SWITCH2 for MKW_SWITCH2.
// Switch 2 controllers over Bluetooth LE on Windows (switch2.h): WinRT through its C ABI (llvm-mingw's WIDL headers),
// the same flow as switch2_bluez.c: an advertisement watcher for Nintendo's manufacturer data (company id 0x0553),
// BluetoothLEDevice from the address, the GATT characteristics by their UUIDs (uncached: that read is what connects),
// notifications in, commands out as writes without response. WinRT raises its events on its own thread pool: the
// handlers only queue them, and the transport's thread runs them in pump, so switch2.c sees one thread as on Linux.
// IBluetoothLEDevice3 (GetGattServicesWithCacheModeAsync) is missing from the mingw headers: declared below from
// microsoft/windows-rs (IID and vtable order). Without WinRT, a Bluetooth adapter or LE it logs why and stops.
#define COBJMACROS
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Storage_Streams
#define WIDL_using_Windows_Devices_Bluetooth
#define WIDL_using_Windows_Devices_Bluetooth_Advertisement
#define WIDL_using_Windows_Devices_Bluetooth_GenericAttributeProfile
#define WIDL_using_Windows_Security_Cryptography
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.security.cryptography.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "switch2.h"
#include "switch2_proto.h"

// ---- IBluetoothLEDevice3 (windows-rs: Windows.Devices.Bluetooth.IBluetoothLEDevice3)

DEFINE_GUID(IID_LEDevice3, 0xaee9e493, 0x44ac, 0x40dc, 0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46);
typedef struct LEDevice3 LEDevice3;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(LEDevice3 *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(LEDevice3 *);
    ULONG (STDMETHODCALLTYPE *Release)(LEDevice3 *);
    void *GetIids, *GetRuntimeClassName, *GetTrustLevel, *get_DeviceAccessInformation, *RequestAccessAsync,
        *GetGattServicesAsync;
    HRESULT (STDMETHODCALLTYPE *GetGattServicesWithCacheModeAsync)(LEDevice3 *, BluetoothCacheMode,
        __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceServicesResult **);
} LEDevice3Vtbl;
struct LEDevice3 { const LEDevice3Vtbl *lpVtbl; };

// ---- Windows 11's preferred connection parameters (windows-rs: IBluetoothLEDevice6,
// IBluetoothLEPreferredConnectionParametersStatics and ...Request, IBluetoothLEConnectionParameters; the mingw
// headers name IBluetoothLEDevice6 only). Why: on Windows the link keeps the system's default connection interval, and
// a tester's Pro Controller 2 had "very noticeable input lag" (discussion #113, 12/11/2026); Switch2Connect asks for
// ThroughputOptimized for the same reason. The request holds while its object lives: the link keeps it.

DEFINE_GUID(IID_LEDevice6, 0xca7190ef, 0x0cae, 0x573c, 0xa1, 0xca, 0xe1, 0xfc, 0x5b, 0xfc, 0x39, 0xe2);
DEFINE_GUID(IID_PrefParamsStatics, 0x0e3e8edc, 0x2751, 0x55aa, 0xa8, 0x38, 0x8f, 0xae, 0xee, 0x81, 0x8d, 0x72);
DEFINE_GUID(IID_PrefParamsRequest, 0x8a375276, 0xa528, 0x5266, 0xb6, 0x61, 0xcc, 0xe6, 0xa5, 0xff, 0x97, 0x39);
DEFINE_GUID(IID_ConnParams, 0x33cb0771, 0x8da9, 0x508f, 0xa3, 0x66, 0x1c, 0xa3, 0x88, 0xc9, 0x29, 0xab);
typedef struct WinObj WinObj;   // any of the four: IInspectable's six slots, then its own
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WinObj *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WinObj *);
    ULONG (STDMETHODCALLTYPE *Release)(WinObj *);
    void *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (STDMETHODCALLTYPE *m[3])(WinObj *, void *, void *);
} WinObjVtbl;
struct WinObj { const WinObjVtbl *lpVtbl; };
#define WIN_CALL(o, i, a, b) ((HRESULT (STDMETHODCALLTYPE *)(WinObj *, void *, void *))(o)->lpVtbl->m[i])(o, a, b)
// IBluetoothLEDevice6: GetConnectionParameters(out), GetConnectionPhy(out), RequestPreferredConnectionParameters(in, out)
// Statics: Balanced(out), ThroughputOptimized(out), PowerOptimized(out). Request: Status(out int: 1 success,
// 2 device not available, 3 access denied). IBluetoothLEConnectionParameters: LinkTimeout, ConnectionLatency,
// ConnectionInterval (each out uint16; the interval in units of 1.25 ms)
static const wchar_t PREF_PARAMS[] = L"Windows.Devices.Bluetooth.BluetoothLEPreferredConnectionParameters";
static HRESULT factory(const wchar_t *cls, const IID *iid, void **out);

// The link's interval and latency in the log ("" when Windows is older than 11: no IBluetoothLEDevice6)
static void log_conn(IBluetoothLEDevice *dev, uint64_t addr, const char *when) {
    WinObj *d6 = NULL, *p = NULL;
    if (FAILED(IBluetoothLEDevice_QueryInterface(dev, &IID_LEDevice6, (void **)&d6))) return;
    if (SUCCEEDED(WIN_CALL(d6, 0, &p, NULL)) && p) {
        uint16_t iv = 0, lat = 0, to = 0;
        ((HRESULT (STDMETHODCALLTYPE *)(WinObj *, uint16_t *))p->lpVtbl->m[0])(p, &to);
        ((HRESULT (STDMETHODCALLTYPE *)(WinObj *, uint16_t *))p->lpVtbl->m[1])(p, &lat);
        ((HRESULT (STDMETHODCALLTYPE *)(WinObj *, uint16_t *))p->lpVtbl->m[2])(p, &iv);
        rt_log("input: Switch 2 controller %012llX %s: connection interval %.2f ms, latency %u, timeout %u ms",
               (unsigned long long)addr, when, iv * 1.25, lat, to * 10u);
        p->lpVtbl->Release(p);
    }
    d6->lpVtbl->Release(d6);
}

// Asks Windows 11 for its shortest connection interval; the request object to keep, NULL when refused or older
static WinObj *prefer_fast(IBluetoothLEDevice *dev, uint64_t addr) {
    WinObj *d6 = NULL, *st = NULL, *pp = NULL, *req = NULL;
    int status = 0;
    if (FAILED(IBluetoothLEDevice_QueryInterface(dev, &IID_LEDevice6, (void **)&d6))) {
        rt_log("input: Switch 2 controller %012llX: no connection parameters to ask for (Windows 10)", (unsigned long long)addr);
        return NULL;
    }
    if (SUCCEEDED(factory(PREF_PARAMS, &IID_PrefParamsStatics, (void **)&st)) && SUCCEEDED(WIN_CALL(st, 1, &pp, NULL)) &&
        SUCCEEDED(WIN_CALL(d6, 2, pp, &req)) && req)
        ((HRESULT (STDMETHODCALLTYPE *)(WinObj *, int *))req->lpVtbl->m[0])(req, &status);
    rt_log("input: Switch 2 controller %012llX: throughput-optimized connection %s", (unsigned long long)addr,
           status == 1 ? "asked for" : status == 2 ? "refused (device not available)" : status == 3 ? "refused (access denied)"
                                                                                       : "not asked (call failed)");
    if (pp) pp->lpVtbl->Release(pp);
    if (st) st->lpVtbl->Release(st);
    d6->lpVtbl->Release(d6);
    if (status != 1 && req) req->lpVtbl->Release(req), req = NULL;
    return req;
}

// IBufferByteAccess (robuffer.h declares it for C++ only): an IBuffer's bytes in place
DEFINE_GUID(IID_ByteAccess, 0x905a0fef, 0xbc53, 0x11df, 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda);
typedef struct ByteAccess ByteAccess;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ByteAccess *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ByteAccess *);
    ULONG (STDMETHODCALLTYPE *Release)(ByteAccess *);
    HRESULT (STDMETHODCALLTYPE *Buffer)(ByteAccess *, byte **);
} ByteAccessVtbl;
struct ByteAccess { const ByteAccessVtbl *lpVtbl; };

// ---- event handlers: one COM object shape for every TypedEventHandler<sender, args> (QueryInterface, AddRef,
// Release, Invoke), its Invoke handing sender and args to fn

typedef struct Handler Handler;
typedef void (*HandlerFn)(Handler *h, void *sender, void *args);
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Handler *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(Handler *);
    ULONG (STDMETHODCALLTYPE *Release)(Handler *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(Handler *, void *, void *);
} HandlerVtbl;
struct Handler {
    const HandlerVtbl *lpVtbl;
    const IID *iid;
    HandlerFn fn;
    LONG refs;
    int link, is_input;   // which link and characteristic it reports for
};

static HRESULT STDMETHODCALLTYPE h_query(Handler *h, REFIID riid, void **out) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAgileObject) || IsEqualIID(riid, h->iid)) {
        InterlockedIncrement(&h->refs);
        return *out = h, S_OK;
    }
    return *out = NULL, E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(Handler *h) { return (ULONG)InterlockedIncrement(&h->refs); }
static ULONG STDMETHODCALLTYPE h_release(Handler *h) {
    LONG n = InterlockedDecrement(&h->refs);
    if (!n) free(h);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE h_invoke(Handler *h, void *sender, void *args) { return h->fn(h, sender, args), S_OK; }
static const HandlerVtbl handler_vtbl = {h_query, h_addref, h_release, h_invoke};

static Handler *handler(const IID *iid, HandlerFn fn, int link, int is_input) {
    Handler *h = calloc(1, sizeof *h);
    if (h) h->lpVtbl = &handler_vtbl, h->iid = iid, h->fn = fn, h->refs = 1, h->link = link, h->is_input = is_input;
    return h;
}

// ---- the queue from WinRT's threads to the transport's

enum { EV_ADVERT, EV_VALUE, EV_GONE, QUEUE_LEN = 256 };
typedef struct {
    int kind, link, is_input;
    uint64_t addr, at;
    uint16_t n;
    uint8_t d[128];
} Event;
static Event queue[QUEUE_LEN];
static int q_head, q_count;
static CRITICAL_SECTION q_lock;
static HANDLE q_ready;

static void post(const Event *e) {
    EnterCriticalSection(&q_lock);
    if (q_count < QUEUE_LEN) queue[(q_head + q_count++) % QUEUE_LEN] = *e;   // full: dropped, as a lost notification
    LeaveCriticalSection(&q_lock);
    SetEvent(q_ready);
}

// The bytes of an IBuffer into e->d (at most its size)
static void buffer_bytes(IBuffer *b, Event *e) {
    UINT32 n = 0;
    byte *p = NULL;
    ByteAccess *acc = NULL;
    e->n = 0;
    if (!b || FAILED(IBuffer_get_Length(b, &n)) ||
        FAILED(IBuffer_QueryInterface(b, &IID_ByteAccess, (void **)&acc)))
        return;
    if (SUCCEEDED(acc->lpVtbl->Buffer(acc, &p)) && p) {
        e->n = (uint16_t)(n < sizeof e->d ? n : sizeof e->d);
        memcpy(e->d, p, e->n);
    }
    acc->lpVtbl->Release(acc);
}

static void on_advert(Handler *h, void *sender, void *args) {
    (void)h, (void)sender;
    IBluetoothLEAdvertisementReceivedEventArgs *a = args;
    IBluetoothLEAdvertisement *adv = NULL;
    __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData *list = NULL;
    Event e = {.kind = EV_ADVERT};
    UINT32 n = 0;
    if (FAILED(IBluetoothLEAdvertisementReceivedEventArgs_get_BluetoothAddress(a, &e.addr)) ||
        FAILED(IBluetoothLEAdvertisementReceivedEventArgs_get_Advertisement(a, &adv)))
        return;
    if (SUCCEEDED(IBluetoothLEAdvertisement_get_ManufacturerData(adv, &list)) &&
        SUCCEEDED(__FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_get_Size(list, &n)))
        for (UINT32 i = 0; i < n; i++) {
            IBluetoothLEManufacturerData *m = NULL;
            IBuffer *b = NULL;
            UINT16 company = 0;
            if (FAILED(__FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_GetAt(list, i, &m)))
                continue;
            if (SUCCEEDED(IBluetoothLEManufacturerData_get_CompanyId(m, &company)) &&
                SUCCEEDED(IBluetoothLEManufacturerData_get_Data(m, &b))) {
                buffer_bytes(b, &e);
                e.is_input = company;   // the company id rides in is_input
                IBuffer_Release(b);
                post(&e);
            }
            IBluetoothLEManufacturerData_Release(m);
        }
    if (list) __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_Release(list);
    IBluetoothLEAdvertisement_Release(adv);
}

static void on_value(Handler *h, void *sender, void *args) {
    (void)sender;
    IBuffer *b = NULL;
    Event e = {.kind = EV_VALUE, .link = h->link, .is_input = h->is_input, .at = switch2_now_ns()};
    if (FAILED(IGattValueChangedEventArgs_get_CharacteristicValue((IGattValueChangedEventArgs *)args, &b))) return;
    buffer_bytes(b, &e);
    IBuffer_Release(b);
    post(&e);
}

static void on_status(Handler *h, void *sender, void *args) {
    (void)args;
    BluetoothConnectionStatus st = BluetoothConnectionStatus_Connected;
    IBluetoothLEDevice_get_ConnectionStatus((IBluetoothLEDevice *)sender, &st);
    if (st == BluetoothConnectionStatus_Disconnected) post(&(Event){.kind = EV_GONE, .link = h->link});
}

// ---- WinRT helpers

static HRESULT factory(const wchar_t *cls, const IID *iid, void **out) {
    HSTRING_HEADER hh;
    HSTRING s;
    HRESULT hr = WindowsCreateStringReference(cls, (UINT32)wcslen(cls), &hh, &s);
    return FAILED(hr) ? hr : RoGetActivationFactory(s, iid, out);
}

// Waits up to ms for an IAsyncOperation (any): 1 when it completed
static int wait_async(void *op, int ms) {
    IAsyncInfo *info = NULL;
    AsyncStatus st = Started;
    if (!op || FAILED(IUnknown_QueryInterface((IUnknown *)op, &IID_IAsyncInfo, (void **)&info))) return 0;
    for (uint64_t end = switch2_now_ns() + (uint64_t)ms * 1000000; st == Started && switch2_now_ns() < end;) {
        if (FAILED(IAsyncInfo_get_Status(info, &st))) break;
        if (st == Started) Sleep(5);
    }
    if (st == Started) IAsyncInfo_Cancel(info);
    IAsyncInfo_Release(info);
    return st == Completed;
}

static int parse_guid(const char *s, GUID *g) {
    unsigned d[11];
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7],
               &d[8], &d[9], &d[10]) != 11)
        return 0;
    g->Data1 = d[0], g->Data2 = (unsigned short)d[1], g->Data3 = (unsigned short)d[2];
    for (int i = 0; i < 8; i++) g->Data4[i] = (unsigned char)d[3 + i];
    return 1;
}

static ICryptographicBufferStatics *bytes_to_buffer;
static uint8_t host[6];   // the adapter's address, little endian (what the controllers wake toward)
static int trace;         // TOTK_SWITCH2=2

// ---- links

enum { MAX_SVC = 8 };
struct S2Link {
    int id;
    uint64_t addr;
    IBluetoothLEDevice *dev;
    IGattDeviceService *svc[MAX_SVC];
    int nsvc;
    IGattCharacteristic *cmd, *rsp, *input, *vib;
    EventRegistrationToken t_status, t_rsp, t_input;
    WinObj *fast;   // the preferred connection parameters request (prefer_fast), held for the link's life
    uint16_t pid;
    int use;
    int gone;   // disconnected while its handshake ran: dropped once that returns
    S2Session *s;
    struct S2Link *next;
};
static S2Link *busy, *links;
static int next_id = 1;

static int write_value(IGattCharacteristic *c, const uint8_t *d, size_t n) {
    IBuffer *b = NULL;
    __FIAsyncOperation_1_GattCommunicationStatus *op = NULL;
    if (!c || FAILED(ICryptographicBufferStatics_CreateFromByteArray(bytes_to_buffer, (UINT32)n, (BYTE *)d, &b))) return 0;
    HRESULT hr = IGattCharacteristic_WriteValueWithOptionAsync(c, b, GattWriteOption_WriteWithoutResponse, &op);
    IBuffer_Release(b);
    if (op) __FIAsyncOperation_1_GattCommunicationStatus_Release(op);   // without response: nothing to wait for
    return SUCCEEDED(hr);
}

// Notifications of c on, each one queued for link id
static int subscribe(IGattCharacteristic *c, int id, int is_input, EventRegistrationToken *tok) {
    Handler *h = handler(&IID___FITypedEventHandler_2_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattValueChangedEventArgs,
                         on_value, id, is_input);
    __FIAsyncOperation_1_GattCommunicationStatus *op = NULL;
    GattCommunicationStatus st = GattCommunicationStatus_Unreachable;
    if (!h) return 0;
    HRESULT hr = IGattCharacteristic_add_ValueChanged(c, (void *)h, tok);
    h_release(h);
    if (FAILED(hr) || FAILED(IGattCharacteristic_WriteClientCharacteristicConfigurationDescriptorAsync(
                          c, GattClientCharacteristicConfigurationDescriptorValue_Notify, &op)))
        return 0;
    if (wait_async(op, 5000)) __FIAsyncOperation_1_GattCommunicationStatus_GetResults(op, &st);
    __FIAsyncOperation_1_GattCommunicationStatus_Release(op);
    return st == GattCommunicationStatus_Success;
}

static int link_write_cmd(S2Link *l, const uint8_t *d, size_t n) { return write_value(l->cmd, d, n); }
static int link_write_vib(S2Link *l, const uint8_t *d, size_t n) { return l->vib && write_value(l->vib, d, n); }
// The report-rate descriptor of the input characteristic written, when the controller has one (switch2_proto.h)
static void write_rate_desc(S2Link *l) {
    static const uint8_t pay[] = {S2_RATE_PAYLOAD};
    IGattCharacteristic3 *c3 = NULL;
    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptorsResult *op = NULL;
    IGattDescriptorsResult *r = NULL;
    __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptor *v = NULL;
    IGattDescriptor *d = NULL;
    __FIAsyncOperation_1_GattCommunicationStatus *wop = NULL;
    GattCommunicationStatus st = GattCommunicationStatus_Unreachable;
    IBuffer *b = NULL;
    UINT32 n = 0;
    GUID g;
    const char *what = "not found";
    parse_guid(S2_UUID_RATE_DESC, &g);
    if (SUCCEEDED(IGattCharacteristic_QueryInterface(l->input, &IID_IGattCharacteristic3, (void **)&c3)) &&
        SUCCEEDED(IGattCharacteristic3_GetDescriptorsForUuidAsync(c3, g, &op)) && wait_async(op, 5000))
        __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptorsResult_GetResults(op, &r);
    if (r && SUCCEEDED(IGattDescriptorsResult_get_Descriptors(r, &v)) && v &&
        SUCCEEDED(__FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptor_get_Size(v, &n)) && n &&
        SUCCEEDED(__FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptor_GetAt(v, 0, &d))) {
        what = "refused";
        if (SUCCEEDED(ICryptographicBufferStatics_CreateFromByteArray(bytes_to_buffer, sizeof pay, (BYTE *)pay, &b)) &&
            SUCCEEDED(IGattDescriptor_WriteValueAsync(d, b, &wop)) && wait_async(wop, 5000) &&
            SUCCEEDED(__FIAsyncOperation_1_GattCommunicationStatus_GetResults(wop, &st)) && st == GattCommunicationStatus_Success)
            what = "written";
    }
    rt_log("input: Switch 2 controller %012llX: report-rate descriptor %s", (unsigned long long)l->addr, what);
    if (wop) __FIAsyncOperation_1_GattCommunicationStatus_Release(wop);
    if (b) IBuffer_Release(b);
    if (d) IGattDescriptor_Release(d);
    if (v) __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptor_Release(v);
    if (r) IGattDescriptorsResult_Release(r);
    if (op) __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDescriptorsResult_Release(op);
    if (c3) IGattCharacteristic3_Release(c3);
}

static int link_start_input(S2Link *l) {
    write_rate_desc(l);
    return subscribe(l->input, l->id, 1, &l->t_input);
}
static void pump(int ms);
static void link_log(S2Link *l) { log_conn(l->dev, l->addr, "5 s into its reports"); }
static const S2LinkOps ops = {link_write_cmd, link_write_vib, link_start_input, pump, link_log};

static void release_char(IGattCharacteristic **c, EventRegistrationToken *tok) {
    if (!*c) return;
    if (tok && tok->value) IGattCharacteristic_remove_ValueChanged(*c, *tok);
    IGattCharacteristic_Release(*c);
    *c = NULL;
}

// Everything of the link released and closed: Windows drops the connection once nothing holds the device
static void free_link(S2Link *l) {
    release_char(&l->rsp, &l->t_rsp);
    release_char(&l->input, &l->t_input);
    release_char(&l->cmd, NULL);
    release_char(&l->vib, NULL);
    for (int i = 0; i < l->nsvc; i++) {
        IClosable *c = NULL;
        if (SUCCEEDED(IGattDeviceService_QueryInterface(l->svc[i], &IID_IClosable, (void **)&c))) IClosable_Close(c), IClosable_Release(c);
        IGattDeviceService_Release(l->svc[i]);
    }
    if (l->fast) {
        IClosable *c = NULL;
        if (SUCCEEDED(l->fast->lpVtbl->QueryInterface(l->fast, &IID_IClosable, (void **)&c))) IClosable_Close(c), IClosable_Release(c);
        l->fast->lpVtbl->Release(l->fast);
    }
    if (l->dev) {
        IClosable *c = NULL;
        if (l->t_status.value) IBluetoothLEDevice_remove_ConnectionStatusChanged(l->dev, l->t_status);
        if (SUCCEEDED(IBluetoothLEDevice_QueryInterface(l->dev, &IID_IClosable, (void **)&c))) IClosable_Close(c), IClosable_Release(c);
        IBluetoothLEDevice_Release(l->dev);
    }
    free(l);
}

static void drop(S2Link *l) {
    S2Link **p = &links;
    while (*p && *p != l) p = &(*p)->next;
    if (*p) *p = l->next;
    if (l->s) switch2_close(l->s);
    free_link(l);
}

static S2Link *link_by_id(int id) {
    for (S2Link *l = links; l; l = l->next)
        if (l->id == id) return l;
    return NULL;
}

static S2Link *link_by_addr(uint64_t addr) {
    for (S2Link *l = links; l; l = l->next)
        if (l->addr == addr) return l;
    return NULL;
}

// ---- advertisements: devices seen for us and not yet tried, and those that failed (not tried again for 10 s)

enum { MAX_PENDING = 8 };
static struct { uint64_t addr; uint16_t pid; int use; } pending[MAX_PENDING];
static int npending;
static struct { uint64_t addr, until; } failed[MAX_PENDING];

static int first_seen(uint64_t addr) {   // TOTK_SWITCH2=2 logs each device once
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == addr) return 0;
    if (n < 64) seen[n++] = addr;
    return 1;
}

static void advert(const Event *e) {
    uint16_t company = (uint16_t)e->is_input;
    s2_advert a;
    int ours = company == S2_COMPANY_ID && s2_advert_parse(e->d, e->n, &a);
    if (trace && first_seen(e->addr))
        rt_log("input: Switch 2 trace: %012llX company 0x%04x, %d bytes%s", (unsigned long long)e->addr, company, e->n,
               company == S2_COMPANY_ID ? (ours ? ", a Switch 2 controller" : ", Nintendo, not a Switch 2 controller") : "");
    if (!ours) return;
    int use = s2_advert_use(&a, host);
    if (use == S2_ADV_IGNORE || link_by_addr(e->addr)) return;
    for (int i = 0; i < npending; i++)
        if (pending[i].addr == e->addr) return;
    for (int i = 0; i < MAX_PENDING; i++)
        if (failed[i].addr == e->addr && switch2_now_ns() < failed[i].until) return;
    if (npending == MAX_PENDING) return;
    pending[npending].addr = e->addr, pending[npending].pid = a.pid, pending[npending].use = use;
    npending++;
}

static void pump(int ms) {
    WaitForSingleObject(q_ready, (DWORD)ms);
    for (;;) {
        Event e;
        EnterCriticalSection(&q_lock);
        int have = q_count > 0;
        if (have) e = queue[q_head], q_head = (q_head + 1) % QUEUE_LEN, q_count--;
        LeaveCriticalSection(&q_lock);
        if (!have) return;
        if (e.kind == EV_ADVERT) {
            advert(&e);
            continue;
        }
        S2Link *l = link_by_id(e.link);
        if (!l) continue;
        if (e.kind == EV_GONE) {
            if (l == busy) l->gone = 1;
            else drop(l);
        } else if (l->s) {
            if (e.is_input) switch2_report(l->s, e.d, e.n, e.at);
            else switch2_reply(l->s, e.d, e.n);
        }
    }
}

// ---- connecting

static void give_up(uint64_t addr, const char *why) {
    rt_log("input: Switch 2 controller %012llX: %s", (unsigned long long)addr, why);
    int slot = 0;
    for (int i = 1; i < MAX_PENDING; i++)
        if (failed[i].until < failed[slot].until) slot = i;
    failed[slot].addr = addr, failed[slot].until = switch2_now_ns() + 10000000000ull;
}

// The characteristics of one service that are ours, kept in l; 1 when it had any
static int find_chars(S2Link *l, IGattDeviceService *svc, const GUID want[7]) {
    IGattDeviceService3 *s3 = NULL;
    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristicsResult *op = NULL;
    IGattCharacteristicsResult *r = NULL;
    __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic *v = NULL;
    GattCommunicationStatus st = GattCommunicationStatus_Unreachable;
    UINT32 n = 0;
    int any = 0;
    if (FAILED(IGattDeviceService_QueryInterface(svc, &IID_IGattDeviceService3, (void **)&s3))) return 0;
    if (SUCCEEDED(IGattDeviceService3_GetCharacteristicsWithCacheModeAsync(s3, BluetoothCacheMode_Uncached, &op)) &&
        wait_async(op, 10000) &&
        SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristicsResult_GetResults(op, &r)) &&
        SUCCEEDED(IGattCharacteristicsResult_get_Status(r, &st)) && st == GattCommunicationStatus_Success &&
        SUCCEEDED(IGattCharacteristicsResult_get_Characteristics(r, &v)) &&
        SUCCEEDED(__FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_get_Size(v, &n)))
        for (UINT32 i = 0; i < n; i++) {
            IGattCharacteristic *c = NULL;
            GUID u;
            if (FAILED(__FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_GetAt(v, i, &c)))
                continue;
            IGattCharacteristic **dst = NULL;
            if (SUCCEEDED(IGattCharacteristic_get_Uuid(c, &u)))
                dst = IsEqualGUID(&u, &want[0]) ? &l->cmd : IsEqualGUID(&u, &want[1]) ? &l->rsp
                    : IsEqualGUID(&u, &want[2]) ? &l->input
                    : IsEqualGUID(&u, &want[3]) || IsEqualGUID(&u, &want[4]) || IsEqualGUID(&u, &want[5]) ? &l->vib : NULL;
            if (dst && !*dst) *dst = c, any = 1;
            else IGattCharacteristic_Release(c);
        }
    if (v) __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_Release(v);
    if (r) IGattCharacteristicsResult_Release(r);
    if (op) __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristicsResult_Release(op);
    IGattDeviceService3_Release(s3);
    return any;
}

// Its GATT services, read from the controller (this is what connects it), and our characteristics in them
static int find_services(S2Link *l, const GUID want[7]) {
    LEDevice3 *d3 = NULL;
    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceServicesResult *op = NULL;
    IGattDeviceServicesResult *r = NULL;
    __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService *v = NULL;
    GattCommunicationStatus st = GattCommunicationStatus_Unreachable;
    UINT32 n = 0;
    if (FAILED(IBluetoothLEDevice_QueryInterface(l->dev, &IID_LEDevice3, (void **)&d3))) return 0;
    if (SUCCEEDED(d3->lpVtbl->GetGattServicesWithCacheModeAsync(d3, BluetoothCacheMode_Uncached, &op)) && wait_async(op, 20000) &&
        SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceServicesResult_GetResults(op, &r)) &&
        SUCCEEDED(IGattDeviceServicesResult_get_Status(r, &st)) && st == GattCommunicationStatus_Success &&
        SUCCEEDED(IGattDeviceServicesResult_get_Services(r, &v)) &&
        SUCCEEDED(__FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_get_Size(v, &n)))
        for (UINT32 i = 0; i < n; i++) {
            IGattDeviceService *s = NULL;
            if (FAILED(__FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_GetAt(v, i, &s)))
                continue;
            if (l->nsvc < MAX_SVC && find_chars(l, s, want)) l->svc[l->nsvc++] = s;
            else IGattDeviceService_Release(s);
        }
    if (v) __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_Release(v);
    if (r) IGattDeviceServicesResult_Release(r);
    if (op) __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceServicesResult_Release(op);
    d3->lpVtbl->Release(d3);
    return st == GattCommunicationStatus_Success;
}

static void connect_one(IBluetoothLEDeviceStatics *devs, const GUID want[7], uint64_t addr, uint16_t pid, int use) {
    rt_log("input: Switch 2 controller %012llX (product 0x%04x) %s, connecting", (unsigned long long)addr, pid,
           use == S2_ADV_PAIR ? "in Sync" : "woke toward this PC");
    S2Link *l = calloc(1, sizeof *l);
    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothLEDevice *op = NULL;
    if (!l) return give_up(addr, "out of memory");
    l->id = next_id++, l->addr = addr, l->pid = pid, l->use = use;
    if (SUCCEEDED(IBluetoothLEDeviceStatics_FromBluetoothAddressAsync(devs, addr, &op)) && wait_async(op, 10000))
        __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothLEDevice_GetResults(op, &l->dev);
    if (op) __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothLEDevice_Release(op);
    if (!l->dev) return free_link(l), give_up(addr, "Windows did not open it");
    Handler *h = handler(&IID___FITypedEventHandler_2_Windows__CDevices__CBluetooth__CBluetoothLEDevice_IInspectable, on_status, l->id, 0);
    if (h) IBluetoothLEDevice_add_ConnectionStatusChanged(l->dev, (void *)h, &l->t_status), h_release(h);
    if (!find_services(l, want)) return free_link(l), give_up(addr, "its services were not read (the connection failed)");
    if (!l->cmd || !l->rsp || !l->input) return free_link(l), give_up(addr, "not the GATT service of a Switch 2 controller");
    if (!subscribe(l->rsp, l->id, 0, &l->t_rsp)) return free_link(l), give_up(addr, "no replies");
    log_conn(l->dev, addr, "before");
    l->fast = prefer_fast(l->dev, addr);
    l->next = links, links = l;
    l->s = switch2_new(l, &ops, pid, use, host);
    busy = l;
    int ok = l->s && switch2_handshake(l->s);
    busy = NULL;
    if (ok && !l->gone) return log_conn(l->dev, addr, "after the handshake");
    drop(l);
    give_up(addr, ok ? "it disconnected during the handshake" : "the handshake failed");
}

// The default adapter's address (host), and whether it does LE
static int find_adapter(void) {
    IBluetoothAdapterStatics *st = NULL;
    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothAdapter *op = NULL;
    IBluetoothAdapter *a = NULL;
    UINT64 addr = 0;
    boolean le = 0;
    if (FAILED(factory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothAdapter, &IID_IBluetoothAdapterStatics, (void **)&st)))
        return 0;
    if (SUCCEEDED(IBluetoothAdapterStatics_GetDefaultAsync(st, &op)) && wait_async(op, 5000))
        __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothAdapter_GetResults(op, &a);
    if (op) __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothAdapter_Release(op);
    IBluetoothAdapterStatics_Release(st);
    if (!a) return 0;
    IBluetoothAdapter_get_BluetoothAddress(a, &addr);
    IBluetoothAdapter_get_IsLowEnergySupported(a, &le);
    IBluetoothAdapter_Release(a);
    for (int i = 0; i < 6; i++) host[i] = (uint8_t)(addr >> 8 * i);
    return le ? 1 : -1;
}

static void bt_main(void *arg) {
    (void)arg;
    static const char *const uuids[6] = {S2_UUID_CMD_WR, S2_UUID_CMD_RSP, S2_UUID_INPUT, S2_UUID_VIB_PRO,
                                         S2_UUID_VIB_JOYCON_L, S2_UUID_VIB_JOYCON_R};
    GUID want[7] = {0};
    for (int i = 0; i < 6; i++) parse_guid(uuids[i], &want[i]);
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return rt_log("input: Switch 2 controllers off: no WinRT");
    InitializeCriticalSection(&q_lock);
    q_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    int le = find_adapter();
    if (!le) return rt_log("input: Switch 2 controllers off: no Bluetooth adapter");
    if (le < 0) return rt_log("input: Switch 2 controllers off: the Bluetooth adapter does not do Bluetooth LE");
    IBluetoothLEDeviceStatics *devs = NULL;
    IInspectable *w = NULL;
    IBluetoothLEAdvertisementWatcher *watcher = NULL;
    HSTRING_HEADER hh;
    HSTRING cls;
    const wchar_t *wname = RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher;
    if (FAILED(factory(RuntimeClass_Windows_Security_Cryptography_CryptographicBuffer, &IID_ICryptographicBufferStatics, (void **)&bytes_to_buffer)) ||
        FAILED(factory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice, &IID_IBluetoothLEDeviceStatics, (void **)&devs)) ||
        FAILED(WindowsCreateStringReference(wname, (UINT32)wcslen(wname), &hh, &cls)) || FAILED(RoActivateInstance(cls, &w)) ||
        FAILED(IInspectable_QueryInterface(w, &IID_IBluetoothLEAdvertisementWatcher, (void **)&watcher)))
        return rt_log("input: Switch 2 controllers off: the Bluetooth LE classes of Windows are missing");
    // Active: the scan response too, as BlueZ's discovery reads it
    IBluetoothLEAdvertisementWatcher_put_ScanningMode(watcher, BluetoothLEScanningMode_Active);
    EventRegistrationToken tok;
    Handler *h = handler(&IID___FITypedEventHandler_2_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementWatcher_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementReceivedEventArgs,
                         on_advert, 0, 0);
    if (!h || FAILED(IBluetoothLEAdvertisementWatcher_add_Received(watcher, (void *)h, &tok)) ||
        FAILED(IBluetoothLEAdvertisementWatcher_Start(watcher)))
        return rt_log("input: Switch 2 controllers off: Bluetooth LE scanning refused (is Bluetooth on?)");
    h_release(h);
    rt_log("input: Switch 2 controllers: looking for them (adapter %02X:%02X:%02X:%02X:%02X:%02X); hold Sync on one to pair it",
           host[5], host[4], host[3], host[2], host[1], host[0]);
    for (uint64_t next_tick = 0;;) {
        pump(5);
        uint64_t now = switch2_now_ns();
        if (now >= next_tick) switch2_tick(), next_tick = now + 15000000;
        if (!npending) continue;
        uint64_t addr = pending[0].addr;
        uint16_t pid = pending[0].pid;
        int use = pending[0].use;
        memmove(pending, pending + 1, sizeof *pending * (size_t)--npending);
        connect_one(devs, want, addr, pid, use);
    }
}

void switch2_start(void) {
    trace = cfg_int("TOTK_SWITCH2") >= 2;
    if (cfg_int("TOTK_SWITCH2") && !plat_thread_spawn(bt_main, NULL, "switch2", 0))
        rt_log("input: Switch 2 controllers off: no thread");
}
