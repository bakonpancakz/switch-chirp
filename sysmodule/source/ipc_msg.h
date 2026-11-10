// Reading a CMIF request and writing its reply, in a message buffer (the
// thread's TLS on the Switch; a plain buffer in test/ipc_test.c)
#pragma once
#include <string.h>

#include <switch/result.h>
#include <switch/sf/cmif.h>
#include <switch/sf/hipc.h>
#include <switch/types.h>

// The request's raw data (after its CMIF header)
static inline const void* ipcInData(void* msg, const HipcParsedRequest* req) {
    const CmifInHeader* h = (const CmifInHeader*)cmifGetAlignedDataStart(req->data.data_words, msg);
    return h + 1;
}

static inline u32 ipcInU32(void* msg, const HipcParsedRequest* req) {
    u32 v = 0;
    memcpy(&v, ipcInData(msg, req), sizeof(v));
    return v;
}

static inline void ipcReply(void* msg, Result rc, const void* data, u32 size) {
    HipcRequest r = hipcMakeRequestInline(msg, .type = 0, .num_data_words = (u32)((16 + sizeof(CmifOutHeader) + size + 3) / 4), );
    CmifOutHeader* h = (CmifOutHeader*)cmifGetAlignedDataStart(r.data_words, msg);
    h->magic = CMIF_OUT_HEADER_MAGIC;
    h->version = 0;
    h->result = rc;
    h->token = 0;
    if (size) memcpy(h + 1, data, size);
}
