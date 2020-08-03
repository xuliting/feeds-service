#include <ela_carrier.h>
#include <crystal.h>

#include "tsx.h"
#include "feeds_client.h"

typedef struct {
    list_entry_t le;
    char addr[ELA_MAX_ADDRESS_LEN + 1];
} PendingFriendRequest;

typedef struct {

} Transaction;

struct FeedsClientTransactionDispatcher {
    ElaCarrier *carrier;
    list_t *pending_frs;
};

static
PendingFriendRequest *pending_freq_create(const char *addr)
{
    PendingFriendRequest *fr = rc_zalloc(sizeof(*fr), NULL);
    if (!fr)
        return NULL;

    strcpy(fr->addr, addr);
    fr->le.data = fr;

    return fr;
}

int tsx_dispatch(FeedsClientTransactionDispatcher *fctd, const char *addr,
                 const FeedsTransaction *tsx, void (*cb)(Resp *, ErrResp *, void *),
                 void *ctx)
{
    char node_id[ELA_MAX_ID_LEN + 1];
    int rc;

    ela_get_id_by_address(addr, node_id, sizeof(node_id));
    if (!ela_is_friend(fctd->carrier, node_id)) {
        if (ela_is_ready(fctd->carrier)) {
            rc = ela_add_friend(fctd->carrier, addr, "hello");
            if (rc < 0)
                return -1;
        } else {
            PendingFriendRequest *fr = pending_freq_create(addr);
            if (!fr)
                return -1;
            list_push_tail(fctd->pending_frs, &fr->le);
        }
    }

}

