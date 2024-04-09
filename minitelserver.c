#include <stdlib.h>

#include <pjsua-lib/pjsua.h>
#include <spandsp.h>
#include <spandsp/fsk.h>

#include <pthread.h>

#include "minitelserver.h"

//#define DEBUG_RECORD 1

// fsk spec
static fsk_spec_t v23ch1 = {
    /* This is mode 2 of the V.23 spec. Mode 1 (the 600baud mode) is not defined here */
    .name = "V23 ch 1",
    .freq_zero = 1700 + 400,
    .freq_one = 1700 - 400,
    .tx_level = -14, //-14,
    .min_level = -50, //-30,
    .baud_rate = 1200*100
};

static fsk_spec_t v23ch2 = {
    .name = "V23 ch 2",
    .freq_zero = 420 + 30,
    .freq_one = 420 - 30,
    .tx_level = -14, //-14,
    .min_level = -50, //-30,
    .baud_rate = 75*100
};

// Pending calls
struct pending_call {
    void (*func)(void *arg);
    void *arg;
};

static struct pending_data {
    struct pending_call *ptr;
    int pos;
    int size;
    int capacity;
    
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    
    pj_pool_t *pool;
    pj_thread_t *thread;

} pending;

static void clear_pending() {
    pthread_mutex_lock(&pending.mutex);
    
    pending.pos = 0;
    pending.size = 0;
    
    pthread_cond_signal(&pending.cond);
    pthread_mutex_unlock(&pending.mutex);
}

static void add_pending_call(void *func, void *arg) {
    pthread_mutex_lock(&pending.mutex);
    
    if (pending.size >= pending.capacity) {
        pending.capacity += 64;
        pending.ptr = realloc(pending.ptr, sizeof(struct pending_call) * pending.capacity);
        
        if ((pending.size + pending.pos) > pending.capacity) {
            memmove(&pending.ptr[pending.pos + 64], &pending.ptr[pending.pos], pending.capacity - pending.pos);
            pending.pos += 64;
        }
    }
    
    int target = (pending.pos + pending.size) % pending.capacity;
    pending.ptr[target].func = func;
    pending.ptr[target].arg = arg;
    
    pending.size++;
    
    pthread_cond_signal(&pending.cond);
    pthread_mutex_unlock(&pending.mutex);
}

static void get_pending_call(struct pending_call *target) {
    pthread_mutex_lock(&pending.mutex);
    
    while (pending.size == 0) {
        pthread_cond_wait(&pending.cond, &pending.mutex);
    }
    
    *target = pending.ptr[pending.pos];
    pending.pos = (pending.pos + 1) % pending.capacity;
    pending.size--;
    
    pthread_mutex_unlock(&pending.mutex);
}

static minitel_client clients[PJSUA_MAX_CALLS] = {0};

enum minitel_state {
    // carrier
    MINITEL_STATE_CARRIER_2100,
    MINITEL_STATE_CARRIER_PAUSE,
    MINITEL_STATE_CARRIER_1300,
    
    // connecting
    MINITEL_STATE_CONNECTING_1,
    MINITEL_STATE_CONNECTING_2,
    
    // ready
    MINITEL_STATE_READY,
    
    // destroyed
    MINITEL_STATE_DESTROYED,
    
    MINITEL_NUM_STATE
};

struct minitel_client {
    // associated call id
    pjsua_call_id call_id;
    
    // fsk modem
    fsk_tx_state_t *tx;
    fsk_rx_state_t *rx;
    
    // pjsua pool
    pj_pool_t *pool;
    
    // media port and slot 
    pjmedia_port port;
    pjsua_conf_port_id slot;
    
    // current state
    enum minitel_state state;
    
    // samples left for carrier state
    int samples_left;
    
    // current received byte
    uint8_t rbyte_val;
    int rbyte_idx;
    
    // current sent byte
    uint8_t sbyte_val;
    int sbyte_idx;
    
    // send buffer
    struct {
        uint8_t *ptr; // the whole buffer
        size_t capacity; // how many bytes are allocated
        size_t size; // how many bytes are left
        size_t pos; // current position in the buffer
    } sbuf;
    
    void *userdata;
    struct minitel_callbacks cb;
    
    // lock for API functions
    pthread_mutex_t mutex;
    
    // Debug recorder
#ifdef DEBUG_RECORD
    FILE *record_in;
    FILE *record_out;
#endif

    // last carrier update timestamp
    int carrier_state;
    int carrier_samples;
};

// callbacks
struct invoke_connection_made_arg {
    minitel_client client;
    void **userdata;
};

struct invoke_connection_lost_arg {
    minitel_client client;
    void *userdata;
};

struct invoke_byte_received_arg {
    minitel_client client;
    void *userdata;
    uint8_t byte;
};

static void invoke_connection_made(struct invoke_connection_made_arg *arg) {
    if (arg->client->cb.connection_made) {
        arg->client->cb.connection_made(arg->client, arg->userdata);
    }
    free(arg);
}

static void invoke_connection_lost(struct invoke_connection_lost_arg *arg) {
    if (arg->client->cb.connection_lost) {
        arg->client->cb.connection_lost(arg->client, arg->userdata);
    }
    free(arg);
}

static void invoke_byte_received(struct invoke_byte_received_arg *arg) {
    if (arg->client->cb.byte_received) {
        arg->client->cb.byte_received(arg->client, arg->userdata, arg->byte);
    }
    free(arg);
}

static void set_state(minitel_client client, enum minitel_state state) {
    //static int connected_states[MINITEL_NUM_STATE] = {[MINITEL_STATE_CONNECTING_1] = 1, [MINITEL_STATE_CONNECTING_2] = 1, [MINITEL_STATE_READY] = 1};
    
    // cannot switch to same state
    if (client->state == state) {
        fprintf(stderr, "Attempt to switch to same state (%d)\n", state);
        return;
    }
    
    if (client->state == MINITEL_STATE_READY) {
        // We were connected ; connection is now lost
        // Call the "connection lost" callback
        
        // TODO: unlock mutex
        struct invoke_connection_lost_arg *arg = malloc(sizeof(arg));
        arg->client = client;
        arg->userdata = client->userdata;
        add_pending_call(invoke_connection_lost, arg);
    }
    
    //if (connected_states[client->state] && !connected_states[state]) {
        // Switching from "active RX" to inactive RX - reset modem
    //    fsk_tx_restart(client->tx, &v23ch1);
    //    fsk_rx_restart(client->rx, &v23ch2, /*FSK_FRAME_MODE_ASYNC*/FSK_FRAME_MODE_FRAMED);
    //    fsk_rx_set_frame_parameters(client->rx, 7, ASYNC_PARITY_EVEN, 1);
    //    client->carrier_state = 0;
    //    client->carrier_timestamp.u64 = 0; 
    //}
    
    //printf("State transition: %d => %d\n", client->state, state);
    client->state = state;
    
    switch (state) {
        case MINITEL_STATE_CARRIER_2100:
            client->samples_left = (8000 * 2000) / 1000; // 2 seconds
            break;
            
        case MINITEL_STATE_CARRIER_PAUSE:
            client->samples_left = (8000 * 75) / 1000; // 75 milliseconds
            break;
            
        case MINITEL_STATE_CARRIER_1300:
            break;
            
        case MINITEL_STATE_CONNECTING_1:
            client->rbyte_idx = -1;
            client->rbyte_val = 0;
            break;
            
        case MINITEL_STATE_CONNECTING_2:
            break;
            
        case MINITEL_STATE_READY: {
            // reset send byte status
            client->sbyte_idx = -2;
            client->sbyte_val = 0;
            
            // reset buffer
            if (client->sbuf.ptr) {
                client->sbuf.size = client->sbuf.pos = 0;
            }
            
            // announce connected
            // TODO: unlock mutex!
            struct invoke_connection_made_arg *arg = malloc(sizeof(arg));
            arg->client = client;
            arg->userdata = &client->userdata;
            add_pending_call(invoke_connection_made, arg);
            
            break;
            
        }
        
        default:
            ;
    }
}

static void carrier_lost(minitel_client client) {
    client->carrier_state = 0;
    client->carrier_samples = 0;
    
    // if we're in a connected state, then we need to switch back to the carrier state
    if (client->state == MINITEL_STATE_CONNECTING_1 || client->state == MINITEL_STATE_CONNECTING_2 || client->state == MINITEL_STATE_READY) {
        set_state(client, MINITEL_STATE_CARRIER_1300);
    }
    
    //switch (client->state) {
    //    case MINITEL_STATE_CARRIER_2100:
    //        set_state(client, MINITEL_STATE_CARRIER_PAUSE);
    //        break;
    //        
    //    case MINITEL_STATE_CARRIER_1300:
    //        break;
    //        
    //    default:
    //        set_state(client, MINITEL_STATE_CARRIER_1300);
    //}
}

static void carrier_down(minitel_client client) {
    //printf("Carrier is down.\n");
    
    // carrier down    
    if (client->carrier_state == 1) {
        client->carrier_state = 2; // transition state
        client->carrier_samples = 500*8; // allowing 500ms to go back up
        
        // It might not be relevant, but if a byte was being sent,
        // then put it back to original state
        if (client->sbyte_idx != -2) {
            client->sbyte_idx = -1;
        }
    }
}


static void carrier_up(minitel_client client) {
    //printf("Carrier is up.\n");
    
    if (client->carrier_state == 0) {
        client->carrier_state = 1;
        client->carrier_samples = 0;

        switch (client->state) {
            case MINITEL_STATE_CARRIER_1300:
            case MINITEL_STATE_CARRIER_2100:
            case MINITEL_STATE_CARRIER_PAUSE:
                set_state(client, MINITEL_STATE_CONNECTING_1);
                break;
                
            default:
                ;
        }

    } else if (client->carrier_state == 2) {
        client->carrier_state = 1;
        client->carrier_samples = 0;
    }
}

static void byte_received(minitel_client client, uint8_t byte) {
    printf("byte received [%i] [%02X]\n", client->state, byte);
    switch (client->state) {
        case MINITEL_STATE_CONNECTING_1:
            if (byte == 0x13)
                set_state(client, MINITEL_STATE_CONNECTING_2);
            else
                set_state(client, MINITEL_STATE_CARRIER_PAUSE);
            
            break;
        
        case MINITEL_STATE_CONNECTING_2:
            if (byte == 0x53)
                set_state(client, MINITEL_STATE_READY);
            else
                set_state(client, MINITEL_STATE_CARRIER_PAUSE);
            
            break;
        
        case MINITEL_STATE_READY:
            struct invoke_byte_received_arg *arg = malloc(sizeof(arg));
            arg->client = client;
            arg->userdata = client->userdata;
            arg->byte = byte;
            add_pending_call(invoke_byte_received, arg);
            
            break;
            
        default:
            ;
    }
}

// get next byte from sbuf
static void flush_byte(minitel_client client) {
    // pas dans le bon état, on flush pas
    if (client->state != MINITEL_STATE_READY) {
        //printf("flush_byte aborted: not ready\n");
        return;
    }
    
    // pas de sbuf, pas d'octet disponible
    if (client->sbuf.ptr == NULL) {
        //printf("flush_byte aborted: null buffer\n");
        return;
    }
    
    // size == 0, pas d'octet dispo non plus
    if (client->sbuf.size == 0) {
        //printf("flush_byte aborted: sbuf size is 0\n");
        return;
    }
    
    // si on est en train d'écrire un octet, on avance pas
    if (client->sbyte_idx != -2) {
        //printf("flush_byte aborted: sbyte idx is not -2\n");
        return;
    }
    
    // on lit l'octet à la position pos
    uint8_t byte = client->sbuf.ptr[client->sbuf.pos];
    client->sbuf.pos = (client->sbuf.pos + 1) % client->sbuf.capacity;
    client->sbuf.size--;
    
    // on commence à écrire l'octet
    client->sbyte_val = byte & 0x7F;
    client->sbyte_idx = -1;
    
    //printf("now writing %02X\n", client->sbyte_val);
}

static int modem_get_bit(void *ptr) {
    minitel_client client = (minitel_client)ptr;
    
    if (client->state == MINITEL_STATE_CARRIER_1300) {
        return 1;
        
    } else if (client->state == MINITEL_STATE_CARRIER_2100) {
        return 0;
        
    } else if (client->state == MINITEL_STATE_READY) {
        // Don't do anything if carrier isn't up
        if (client->carrier_state != 1) {
            return 1;
        }
        
        int ret;
        //if (client->sbyte_idx != -2)
        //    printf("[%d] byte %02x\n", client->sbyte_idx, client->sbyte_val);
        
        switch (client->sbyte_idx) {
            case -2:
                // we're not transferring anything
                ret = 1;
                break;
                
            case -1:
                // we're starting a byte transfer
                ret = 0;
                client->sbyte_idx++;
                break;
                
            case 0: case 1: case 2: case 3:
            case 4: case 5: case 6:
                // we're writing a bit
                ret = (client->sbyte_val >> (client->sbyte_idx)) & 1;
                client->sbyte_idx++;
                break;
                
            case 7:
                // we're writing the parity bit                
                int parity = 0;
                for (int i = 0; i < 7; i++) {
                    parity += (client->sbyte_val >> i) & 1;
                }
                
                ret = parity & 1;
                client->sbyte_idx++;
                break;
                
            case 8:
                // we're writing the close bit
                ret = 1;
                client->sbyte_idx = -2;
                
                // maybe we can find another byte?
                flush_byte(client);
                break;
                
            default:
                ret = 1;
                fprintf(stderr, "Call %d unknown sent byte index %d\n", client->call_id, client->sbyte_idx);
        }
        
        return ret;
        
    } else {
        return 1;
    }
}

/*
static void modem_put_bit(void *ptr, int bit) {
    minitel_client client = (minitel_client)ptr;
    
    if (bit < 0) {
        if (bit == SIG_STATUS_CARRIER_DOWN) {
            // We lost our connection
            carrier_down(client);
            
        } else if (bit == SIG_STATUS_CARRIER_UP) {
            carrier_up(client);
        }
        
    } else {
        if (client->state == MINITEL_STATE_CONNECTING_1 ||
                client->state == MINITEL_STATE_CONNECTING_2 ||
                client->state == MINITEL_STATE_READY) {
            switch (client->rbyte_idx) {
                case -1:
                    if (bit == 0) {
                        client->rbyte_idx = 0;
                        client->rbyte_val = 0;
                    }
                    break;
                    
                case 0: case 1: case 2: case 3:
                case 4: case 5: case 6:
                    client->rbyte_val |= (bit << (client->rbyte_idx));
                    client->rbyte_idx++;
                    break;
                    
                case 7:
                    int parity = bit;
                    for (int i = 0; i < 7; i++) {
                        parity += (client->rbyte_val >> i) & 1;
                    }
                    
                    if ((parity & 1) == 0) {
                        client->rbyte_idx++;
                        
                    } else {
                        client->rbyte_idx = -1;
                        fprintf(stderr, "Call %d error with parity bit! (%02X)\n", client->call_id, client->rbyte_val);
                    }
                    
                    break;
                    
                case 8:
                    if (bit == 1) {
                        byte_received(client, client->rbyte_val);
                        
                    } else {
                        fprintf(stderr, "Call %d error with stop bit! (%02X)\n", client->call_id, client->rbyte_val);
                    }
                    
                    client->rbyte_idx = -1;
                    break;
            
                default:
                    fprintf(stderr, "Call %d unknown received byte index %d\n", client->call_id, client->rbyte_idx);
            }
        }
    }
}*/

static void modem_put_bit(void *ptr, int bit) {
    minitel_client client = (minitel_client)ptr;
    
    if (bit < 0) {
        if (bit == SIG_STATUS_CARRIER_DOWN) {
            // We lost our connection
            carrier_down(client);
            
        } else if (bit == SIG_STATUS_CARRIER_UP) {
            carrier_up(client);
        }
        return;
    }
    if (client->state == MINITEL_STATE_CONNECTING_1 ||
            client->state == MINITEL_STATE_CONNECTING_2 ||
            client->state == MINITEL_STATE_READY) {
        byte_received(client, bit & 0x7F);
    }
}

static void lock_client(minitel_client client) {
    pthread_mutex_lock(&client->mutex);
}

static void unlock_client(minitel_client client) {
    pthread_mutex_unlock(&client->mutex);
}

static void no_more_samples(minitel_client client) {
    switch (client->state) {
        case MINITEL_STATE_CARRIER_2100:
            set_state(client, MINITEL_STATE_CARRIER_PAUSE);
            break;
            
        case MINITEL_STATE_CARRIER_PAUSE:
            set_state(client, MINITEL_STATE_CARRIER_1300);
            break;
            
        case MINITEL_STATE_CARRIER_1300:
            //set_state(client, MINITEL_STATE_CARRIER_1300);
            break;
        
        default:
            ;
    }
}

static int generate_samples(minitel_client client, int count, int16_t *buf) {
    // calculate max count
    if (client->samples_left > 0 && count > client->samples_left) {
        count = client->samples_left;
    }
    
    // generate samples
    if (client->state == MINITEL_STATE_CARRIER_PAUSE) {
        memset(buf, 0, count * 2);
        
    } else {
        count = fsk_tx(client->tx, buf, count);
    }
    
    // we have generated count samples
    if (client->samples_left > 0) {
        client->samples_left -= count;
        
        if (client->samples_left <= 0) {
            no_more_samples(client);
        }
    }
    
    return count;
}

static pj_status_t modem_get_frame(pjmedia_port *port, pjmedia_frame *frame) {
    minitel_client client = (minitel_client)port->port_data.pdata;
    pj_size_t count = frame->size / 2;
    pj_int16_t *buf = frame->buf;
    
    // if we have no client, there's nothing we can do
    if (!client)
        return PJ_SUCCESS;
    
    lock_client(client);
    
    while (count > 0) {
        // ask to generate "count" samples
        int len = generate_samples(client, count, buf);
        count -= len;
        buf += len;
    }
    
    // So we generated some samples. Put them in the client
    unlock_client(client);
    
#ifdef DEBUG_RECORD
    fwrite(frame->buf, 1, frame->size, client->record_out);
#endif

    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}

static pj_status_t modem_put_frame(pjmedia_port *port, pjmedia_frame *frame) {
    minitel_client client = (minitel_client)port->port_data.pdata;
    
    // if we have no client, there's nothing we can do
    if (!client)
        return PJ_SUCCESS;
    
    lock_client(client);
    
    if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO) {
        pj_size_t count = frame->size / 2;
        pj_int16_t *buf = frame->buf;

#ifdef DEBUG_RECORD
        fwrite(buf, 2, count, client->record_in);
        fflush(client->record_in);
#endif

        while (count > 0) {
            int len = count;
            
            if (client->carrier_samples > 0) {
                //printf("Carrier samples = %i\n", client->carrier_samples);
                if (len > client->carrier_samples)
                    len = client->carrier_samples;
                
                len -= fsk_rx(client->rx, buf, len);
                
                // We have this weird condition where carrier_samples
                // could be reset but we'd still consider theses samples to the "timer" ;
                // It's unlikely to happen and will cause no harm, but we might want
                // to handle that anyway (TODO?)
                client->carrier_samples -= len;
                
                if (client->carrier_state == 2 && client->carrier_samples == 0) {
                    //printf("Considered lost\n");
                    carrier_lost(client);
                }
                
            } else {
                len -= fsk_rx(client->rx, buf, len);
            }
            
            count -= len;
            buf += len;
        }
    }
    
    unlock_client(client);
    
    return PJ_SUCCESS;
}

/*static void reset_client(minitel_client client) {
    // reset the state of a client 
    fsk_tx_restart(client->tx, &preset_fsk_specs[FSK_V23CH1]);
    fsk_rx_restart(client->rx, &preset_fsk_specs[FSK_V23CH2], FSK_FRAME_MODE_ASYNC);
    
    // change state back to carrier
    set_state(client, MINITEL_STATE_CARRIER_1300);
    
    // clear sbuf
    client->sbuf.ptr = NULL;
}*/

static void disconnect_client(minitel_client client, pjsua_call_info *call_info) {
    pjsua_call_info ci;
    
    // Check if we have a call id
    if (client->call_id == PJSUA_INVALID_ID) 
        return;
    
    if (call_info == NULL) {
        // Get call info if it wasn't given to us
        call_info = &ci;
        
        if (pjsua_call_get_info(client->call_id, &ci) != PJ_SUCCESS)
            return;
        
    } else {
        // Probably will never happen, but you never know
        if (call_info->id != client->call_id)
            return;
    }
    
    // Disconnect conf port
    if (call_info->conf_slot != PJSUA_INVALID_ID) {
        pjsua_conf_disconnect(client->slot, call_info->conf_slot);
        pjsua_conf_disconnect(call_info->conf_slot, client->slot);
    }
}

// TODO: should the destroy callback be used?
// It's unclear how it exactly works and the client shouldn't free it by himself
static void destroy_client(minitel_client client) {    
    // Remove the slot
    pjsua_conf_remove_port(client->slot);
    
    // Destroy the port
    client->port.port_data.pdata = NULL;
    pjmedia_port_destroy(&client->port);
    
    // Free fx and tx modems
    fsk_rx_free(client->rx);
    fsk_tx_free(client->tx);
    
    // Free the buf
    if (client->sbuf.ptr) {
        free(client->sbuf.ptr);
    }
    
#ifdef DEBUG_RECORD
    fclose(client->record_in);
    fclose(client->record_out);
#endif

    pthread_mutex_destroy(&client->mutex);
    
    // Free the pool (will also destroy the client)
    pj_pool_release(client->pool);
}

static minitel_client create_client() {
    // create our pool for allocation
    pj_pool_t *pool = pjsua_pool_create("virtmodem", 4000, 4000);
    
    // create our client
    minitel_client client = pj_pool_zalloc(pool, sizeof(struct minitel_client));
    client->call_id = PJSUA_INVALID_ID;
    
    // fsk modem
    client->tx = fsk_tx_init(NULL, &v23ch1, &modem_get_bit, (void *)client);    
    client->rx = fsk_rx_init(NULL, &v23ch2, FSK_FRAME_MODE_FRAMED, &modem_put_bit, (void *)client);
    fsk_rx_set_frame_parameters(client->rx, 7, ASYNC_PARITY_EVEN, 1);
    
    // pool
    client->pool = pool;
    
    // initialize port
    pj_str_t name = pj_str("virtmodem");
    pjmedia_port *port = &client->port;
    pjmedia_port_info_init(&port->info, &name, PJMEDIA_SIG_CLASS_PORT_AUD('m', 't'), 8000, 1, 16, 200);
    port->get_frame = &modem_get_frame;
    port->put_frame = &modem_put_frame;
    port->port_data.pdata = (void *)client;
    pjsua_conf_add_port(pool, port, &client->slot);
    
    // set state
    client->state = MINITEL_STATE_DESTROYED;
    set_state(client, MINITEL_STATE_CARRIER_2100);
    
    // clear buffer
    client->sbuf.ptr = NULL;
    
    // clear callbacks
    memset(&client->cb, 0, sizeof(client->cb));
    
    // create lock
    pthread_mutex_init(&client->mutex, NULL);
    
#ifdef DEBUG_RECORD
    // recording
    char filename[64];
    time_t ts = time(NULL);
    
    sprintf(filename, "samples_%ld_in.bin", ts);
    client->record_in = fopen(filename, "wb");
    
    sprintf(filename, "samples_%ld_out.bin", ts);
    client->record_out = fopen(filename, "wb");
#endif

    // carrier state state (0=down, 1=up, 2=up-to-down)
    client->carrier_state = 0;
    client->carrier_samples = 0;
    
    return client;
}

// Start a minitel communication for the given pjsua call
// Returns non-zero on success
int minitel_start(pjsua_call_id call_id, struct minitel_callbacks *cb) {
    // get call info
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        return 2;
    }
    
    // check media status
    if (ci.media_status != PJSUA_CALL_MEDIA_ACTIVE) {
        return 1;
    }
    
    // Does the client already exist for our call_id?
    if (clients[call_id]) {
        // The lock will be destroyed
        lock_client(clients[call_id]);
        
        if (clients[call_id]->state != MINITEL_STATE_DESTROYED) {        
            // We need to free the client
            disconnect_client(clients[call_id], &ci);
            set_state(clients[call_id], MINITEL_STATE_DESTROYED);
            unlock_client(clients[call_id]);
            
            add_pending_call(destroy_client, clients[call_id]);
            clients[call_id] = NULL;
            
        } else {
            unlock_client(clients[call_id]);
        }
    }
    
    // Create our new client
    minitel_client client = create_client();
    if (!client)
        return 3;
    
    // Assign call id to client
    client->call_id = call_id;
    memcpy(&client->cb, cb, sizeof(client->cb));
    
    // Store the client
    lock_client(client);
    clients[call_id] = client;
    
    // Connect our client to the modem
    pjsua_conf_connect(ci.conf_slot, client->slot);
    pjsua_conf_connect(client->slot, ci.conf_slot);
    
    unlock_client(client);
    
    // Seems like we're started!
    return 0;
}

// Stop a minitel communication for the given pjsua call
void minitel_stop(pjsua_call_id call_id) {
    // Ignore if the connection doesn't exist
    if (!clients[call_id])
        return;
    
    lock_client(clients[call_id]);
    
    if (clients[call_id]->state == MINITEL_STATE_DESTROYED) {
        unlock_client(clients[call_id]);
        return;
    }
    
    // The connection probably exists then
    minitel_client client = clients[call_id];
    
    // Special state for destroying
    set_state(client, MINITEL_STATE_DESTROYED);
    
    unlock_client(clients[call_id]);
    
    // Then destroy it
    disconnect_client(client, NULL);
    add_pending_call(destroy_client, client);
    clients[call_id] = NULL;
}

// Send bytes to the minitel client
void minitel_transmit(minitel_client client, const uint8_t *data, size_t length) {
    // Don't care about zero-length
    if (length <= 0)
        return;
    
    // We have a circular buffer to fill..
    lock_client(client);
    if (client->state != MINITEL_STATE_READY) {
        unlock_client(client);
        return;
    }
    
    // If ptr is NULL, then we have no buffer ; allocate a new one
    if (client->sbuf.ptr == NULL) {
        // We allocate chunks of 2048 bytes
        size_t alloc_size = (((length - 1) >> 11) + 1) << 11;
        client->sbuf.ptr = malloc(alloc_size);
        client->sbuf.capacity = alloc_size;
        client->sbuf.size = length;
        client->sbuf.pos = 0;
        
        memcpy(client->sbuf.ptr, data, length);
        
    } else {    
        // Remaining size is capacity - size ; check if it fits
        if (length > client->sbuf.capacity - client->sbuf.size) {
            // It doesn't fit; we need to realloc.
            
            // We can realloc our buffer then move the end buf to the new end
            // Here how it goes
            // [4567   123]  capacity=10      [4567        123]  capacity=10+5
            //         ^     size=7       =>               ^     size=7
            //         pos   pos=7                         pos   pos=7+5
            
            // If the buffer is continuous, we just have to realloc
            // [  12345   ]  capacity=10
            //    ^          size=5
            //    pos        pos=2
            
            // First, realloc
            size_t alloc_size = (((client->sbuf.capacity + length - 1) >> 11) + 1) << 11;
            client->sbuf.ptr = realloc(client->sbuf.ptr, alloc_size);
            
            if ((client->sbuf.size + client->sbuf.pos) > client->sbuf.capacity) {
                // We need to move the data on the right side of the buffer
                size_t diff_size = alloc_size - client->sbuf.capacity;
                
                memmove(client->sbuf.ptr + client->sbuf.pos + diff_size,
                        client->sbuf.ptr + client->sbuf.pos,
                        client->sbuf.capacity - client->sbuf.pos);
                        
                client->sbuf.pos += diff_size;
            }
            
            client->sbuf.capacity = alloc_size;
        }
        
        // We have enough space now, so we can write our data
        
        // Total remaining size after pos (free and used)
        size_t target = (client->sbuf.pos + client->sbuf.size) % client->sbuf.capacity;
        
        // The target might not be continuous.
        // write 678 to [   12345  ]  capacity=10 pos=3 size=5 target=8 length=3 - doesn't fit
        //                            because we have 2 bytes available. capacity - target = 2
        
        size_t avail = client->sbuf.capacity - target;
        if (avail < length) {
            memcpy(client->sbuf.ptr + target, data, avail);
            memcpy(client->sbuf.ptr, data + avail, length - avail);
            
        } else {
            // it fits. nailed it.
            memcpy(client->sbuf.ptr + target, data, length);
        }
        
        client->sbuf.size += length;
    }
    
    // try to flush
    flush_byte(client);
    unlock_client(client);
}

// Disconnect the client. Connection lost *will* be called, and the minitel_client won't be usable
void minitel_disconnect(minitel_client client, int destroy) {
    // Lock will be destroyed
    lock_client(client);
    
    // ID is invalid
    if (client->call_id == PJSUA_INVALID_ID) {
        unlock_client(client);
        return;
    }
    
    // Client is already destroyed
    if (client->state == MINITEL_STATE_DESTROYED) {
        unlock_client(client);
        return;
    }
    
    set_state(client, MINITEL_STATE_DESTROYED);
    unlock_client(client);
    
    pjsua_call_id call_id = client->call_id;
    
    // Switch to destroyed state
    if (destroy) {
        set_state(client, MINITEL_STATE_DESTROYED);
    
        disconnect_client(client, NULL);
        add_pending_call(destroy_client, client);
        clients[call_id] = NULL;
        
    } else {
        // Do not destroy: go back to initial state
        set_state(client, MINITEL_STATE_CARRIER_PAUSE);
    }
}

// Get the call ID for a given minitel_client
pjsua_call_id minitel_get_call_id(minitel_client client) {
    return client->call_id;
}

static int minitel_worker(void *arg) {
    struct pending_call call;
    for (;;) {
        get_pending_call(&call);
        if (call.func == NULL) {
            break;
        }
        
        call.func(call.arg);
    }
    
    return 0;
}

// Create worker thread
void minitel_init() {
    pending.pool = pjsua_pool_create("minitel", 4000, 4000);
    
    pending.ptr = NULL;
    pending.capacity = 0;
    pending.pos = 0;
    pending.size = 0;
    
    pthread_mutex_init(&pending.mutex, NULL);
    pthread_cond_init(&pending.cond, NULL);
    
    pj_thread_create(pending.pool, "minitel", minitel_worker, NULL, PJ_THREAD_DEFAULT_STACK_SIZE, 0, &pending.thread);
}
