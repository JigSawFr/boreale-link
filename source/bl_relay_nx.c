// Copyright (C) 2026 JigSawFr.  GPLv3-or-later (see LICENSE).
#include "bl_relay_nx.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

static Service s_srv;
static bool    s_open;

// Atmosphère's sm answers whether a service is registered (command 65100, as
// libstratosphere's smAtmosphereHasService).
static bool has_service(const char *name)
{
    const SmServiceName n = smEncodeName(name);
    u8 has = 0;
    const Result rc = hosversionAtLeast(12, 0, 0) ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, n, has)
                                                  : serviceDispatchInOut(smGetServiceSession(), 65100, n, has);
    return R_SUCCEEDED(rc) && has;
}

// A call that failed because the agent went away ends the session.
static Result checked(Result rc)
{
    if (R_FAILED(rc) && R_MODULE(rc) == Module_Kernel) bl_relay_close();
    return rc;
}

bool bl_relay_available(void)
{
    return s_open || has_service(BL_RELAY_SERVICE);
}

bool bl_relay_open(const char *module, const char *version, BlRelayHelloReply *reply, uint32_t *result)
{
    BlRelayHelloReply local;
    BlRelayHelloReply *r = reply ? reply : &local;
    memset(r, 0, sizeof(*r));
    if (result) *result = 0;
    if (!bl_relay_module_ok(module)) return false;
    if (!s_open) {
        if (!has_service(BL_RELAY_SERVICE)) return false;
        const Result rc = smGetService(&s_srv, BL_RELAY_SERVICE);
        if (result) *result = rc;
        if (R_FAILED(rc)) return false;
        s_open = true;
    }
    BlRelayHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.protocol = BL_RELAY_PROTOCOL;
    snprintf(hello.module, sizeof(hello.module), "%s", module);
    snprintf(hello.version, sizeof(hello.version), "%s", version ? version : "");
    const Result rc = checked(serviceDispatchInOut(&s_srv, BlRelayCmd_Hello, hello, *r));
    if (result) *result = rc;
    if (R_FAILED(rc) || !r->accepted) {
        bl_relay_close();   // another protocol, a refused module, no room: nothing more to say
        return false;
    }
    return true;
}

bool bl_relay_connected(void)
{
    return s_open;
}

void bl_relay_close(void)
{
    if (!s_open) return;
    serviceDispatch(&s_srv, BlRelayCmd_Close);   // the agent frees the session either way
    serviceClose(&s_srv);
    s_open = false;
}

uint32_t bl_relay_publish(const char *sub, const void *payload, size_t len, bool retained)
{
    if (!s_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    uint8_t buf[1 + BL_RELAY_SUB_MAX + BL_RELAY_PAYLOAD_MAX];
    const size_t n = bl_relay_encode_publish(sub, payload, len, retained, buf, sizeof(buf));
    if (!n) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return checked(serviceDispatch(&s_srv, BlRelayCmd_Publish, .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                                   .buffers = { { buf, n } }));
}

uint32_t bl_relay_pop_order(BlRelayInOrder *out, bool *got)
{
    memset(out, 0, sizeof(*out));
    *got = false;
    if (!s_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    u32 n = 0;
    const Result rc = checked(serviceDispatchOut(&s_srv, BlRelayCmd_PopOrder, n,
                                                 .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
                                                 .buffers = { { out->buf, sizeof(out->buf) } }));
    if (R_FAILED(rc) || n == 0) return rc;
    // The agent's answer is read like anything from another process: what does not parse is dropped.
    if (n <= sizeof(out->buf) &&
        bl_relay_decode_order(out->buf, n, &out->sub, &out->payload, &out->payload_len, &out->retained))
        *got = true;
    return rc;
}

uint32_t bl_relay_status(BlRelayStatus *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return checked(serviceDispatchOut(&s_srv, BlRelayCmd_Status, *out));
}

// A session for one control call: no module, no Hello.
static Result control_open(Service *srv)
{
    if (!has_service(BL_RELAY_SERVICE)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return smGetService(srv, BL_RELAY_SERVICE);
}

uint32_t bl_relay_info(BlRelayInfo *out)
{
    memset(out, 0, sizeof(*out));
    Service srv;
    Result rc = control_open(&srv);
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatchOut(&srv, BlRelayCmd_Info, *out);
    serviceClose(&srv);
    return rc;
}

uint32_t bl_relay_shutdown(void)
{
    Service srv;
    Result rc = control_open(&srv);
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatch(&srv, BlRelayCmd_Shutdown);
    serviceClose(&srv);
    return rc;
}

uint32_t bl_relay_resume(void)
{
    Service srv;
    Result rc = control_open(&srv);
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatch(&srv, BlRelayCmd_Resume);
    serviceClose(&srv);
    return rc;
}

#else   // not the console: no agent

bool bl_relay_available(void)
{
    return false;
}

bool bl_relay_open(const char *module, const char *version, BlRelayHelloReply *reply, uint32_t *result)
{
    (void)module;
    (void)version;
    if (reply) memset(reply, 0, sizeof(*reply));
    if (result) *result = 0;
    return false;
}

bool bl_relay_connected(void)
{
    return false;
}

void bl_relay_close(void) {}

uint32_t bl_relay_publish(const char *sub, const void *payload, size_t len, bool retained)
{
    (void)sub;
    (void)payload;
    (void)len;
    (void)retained;
    return 1;
}

uint32_t bl_relay_pop_order(BlRelayInOrder *out, bool *got)
{
    memset(out, 0, sizeof(*out));
    *got = false;
    return 1;
}

uint32_t bl_relay_status(BlRelayStatus *out)
{
    memset(out, 0, sizeof(*out));
    return 1;
}

uint32_t bl_relay_info(BlRelayInfo *out)
{
    memset(out, 0, sizeof(*out));
    return 1;
}

uint32_t bl_relay_shutdown(void)
{
    return 1;
}

uint32_t bl_relay_resume(void)
{
    return 1;
}

#endif
