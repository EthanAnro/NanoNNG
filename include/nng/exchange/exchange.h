#ifndef EXCHANGE_H
#define EXCHANGE_H

#include <stddef.h>
#include "core/nng_impl.h"
#include "nng/supplemental/nanolib/ringbuffer.h"
#include "nng/supplemental/nanolib/conf.h"
#define EXCHANGE_NAME_LEN 100
#define TOPIC_NAME_LEN    100
#define RINGBUFFER_MAX    100

typedef struct exchange_s exchange_t;
struct exchange_s {
	char name[EXCHANGE_NAME_LEN];
	char topic[TOPIC_NAME_LEN];

	ringBuffer_t *rbs[RINGBUFFER_MAX];
	unsigned int rb_count;
};
NNG_DECL int exchange_queue_get_ringBuffer(nni_list *ex_queue,
								  char *rbName, ringBuffer_t **rb);
NNG_DECL int exchange_client_get_msg_by_key(void *arg, uint32_t key, nni_msg **msg);
NNG_DECL int exchange_client_get_msgs_by_key(void *arg, uint32_t key, uint32_t count, nni_list **list);

NNG_DECL int exchange_init(exchange_t **ex, char *name, char *topic,
 				  unsigned int *rbsCaps, char **rbsName, unsigned int rbsCount);
NNG_DECL int exchange_add_rb(exchange_t *ex, ringBuffer_t *rb);
NNG_DECL int exchange_release(exchange_t *ex);
NNG_DECL int exchange_handle_msg(exchange_t *ex, int key, void *msg);
NNG_DECL int exchange_get_ringBuffer(exchange_t *ex, char *rbName, ringBuffer_t **rb);

#endif
