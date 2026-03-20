#include <iris/ipc.h>
#include <iris/task.h>
#include <stdint.h>

/*
 * IPC — Inter-Process Communication
 *
 * Canal = buffer circular de mensajes (FIFO).
 * Las tareas se comunican enviando y recibiendo mensajes
 * sin compartir memoria directamente.
 *
 * ipc_send    → no bloqueante, falla si el canal está lleno
 * ipc_recv    → bloqueante: cede el CPU hasta que haya un mensaje
 * ipc_try_recv → no bloqueante, retorna IPC_ERR_EMPTY si vacío
 */

static struct ipc_channel channels[IPC_MAX_CHANNELS];
static uint32_t channel_count = 0;
static uint32_t next_seq      = 0;

/* copia de memoria mínima sin libc */
static void mem_copy_ipc(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (len--) *d++ = *s++;
}

static void msg_copy(struct ipc_message *dst, const struct ipc_message *src) {
    mem_copy_ipc(dst, src, sizeof(struct ipc_message));
}

void ipc_init(void) {
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        channels[i].id       = 0;
        channels[i].owner_id = 0;
        channels[i].head     = 0;
        channels[i].tail     = 0;
        channels[i].count    = 0;
        channels[i].reserved = 0;
    }
    channel_count = 0;
    next_seq      = 0;
}

int32_t ipc_channel_create(uint32_t owner_id) {
    if (channel_count >= IPC_MAX_CHANNELS)
        return IPC_ERR_INVALID;

    struct ipc_channel *ch = &channels[channel_count];
    ch->id       = channel_count;
    ch->owner_id = owner_id;
    ch->head     = 0;
    ch->tail     = 0;
    ch->count    = 0;

    return (int32_t)channel_count++;
}

static struct ipc_channel *get_channel(uint32_t channel_id) {
    if (channel_id >= channel_count)
        return 0;
    return &channels[channel_id];
}

int32_t ipc_send(uint32_t channel_id, struct ipc_message *msg) {
    struct ipc_channel *ch = get_channel(channel_id);
    if (!ch) return IPC_ERR_INVALID;
    if (ch->count >= IPC_CHANNEL_CAP) return IPC_ERR_FULL;

    msg->seq = next_seq++;
    msg_copy(&ch->buf[ch->tail], msg);
    ch->tail = (ch->tail + 1) % IPC_CHANNEL_CAP;
    ch->count++;

    return IPC_OK;
}

int32_t ipc_try_recv(uint32_t channel_id, struct ipc_message *out) {
    struct ipc_channel *ch = get_channel(channel_id);
    if (!ch) return IPC_ERR_INVALID;
    if (ch->count == 0) return IPC_ERR_EMPTY;

    msg_copy(out, &ch->buf[ch->head]);
    ch->buf[ch->head].type = IPC_MSG_EMPTY;
    ch->head  = (ch->head + 1) % IPC_CHANNEL_CAP;
    ch->count--;

    return IPC_OK;
}

int32_t ipc_recv(uint32_t channel_id, struct ipc_message *out) {
    /*
     * Bloqueante: cede el CPU hasta que haya un mensaje.
     * En un microkernel real esto bloquearía la tarea en el scheduler.
     * Por ahora usamos busy-yield para no necesitar blocking scheduler.
     */
    int32_t result;
    while ((result = ipc_try_recv(channel_id, out)) == IPC_ERR_EMPTY) {
        task_yield();
    }
    return result;
}

uint32_t ipc_channel_count(uint32_t channel_id) {
    struct ipc_channel *ch = get_channel(channel_id);
    if (!ch) return 0;
    return ch->count;
}
