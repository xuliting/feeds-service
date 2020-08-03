#ifndef __TSX_H__
#define __TSX_H__

#include "rpc.h"

typedef struct FeedsClientTransactionDispatcher FeedsClientTransactionDispatcher;

typedef struct {
    Req *req;
    Marshalled *(*marshal)(const Req *);
    int (*unmarshal)(Resp **, ErrResp **);
} FeedsTransaction;

int tsx_dispatch(FeedsClientTransactionDispatcher *fctd, const char *addr,
                 const FeedsTransaction *tsx, void (*cb)(Resp *, ErrResp *, void *),
                 void *ctx);

#endif //__TSX_H__
