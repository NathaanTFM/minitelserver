// MINITEL SERVER
// Public API

#ifndef MINITEL_SERVER_H
#define MINITEL_SERVER_H

#include <stdint.h>
#include <stddef.h>

// PJSUA is a dependency no matter what
#include <pjsua-lib/pjsua.h>

// Opaque structure for a minitel client
typedef struct minitel_client *minitel_client;

// Called when a minitel client has been connected and is ready
// Parameters:
//      userdata: set to your own pointer; will be used for every next callback
typedef void (*connection_made_cb)(minitel_client, void **userdata);

// Called when a minitel client has been lost
// The client instance should NOT be used
typedef void (*connection_lost_cb)(minitel_client, void *userdata);

// Called when a minitel client has transmitted a single byte
typedef void (*byte_received_cb)(minitel_client, void *userdata, uint8_t byte);

struct minitel_callbacks {    
    // Minitel related callbacks
    connection_made_cb  connection_made;
    connection_lost_cb  connection_lost;
    byte_received_cb    byte_received;
};

// Start a minitel communication for the given pjsua call
extern int minitel_start(pjsua_call_id call_id, struct minitel_callbacks *cb);

// Stop a minitel communication for the given pjsua call
extern void minitel_stop(pjsua_call_id call_id);

// Send bytes to the minitel client
extern void minitel_transmit(minitel_client, const uint8_t *data, size_t length);

// Disconnect the client. Connection lost *will* be called, and the minitel_client won't be usable
extern void minitel_disconnect(minitel_client, int hangup);

// Get the call ID for a given minitel_client
extern pjsua_call_id minitel_get_call_id(minitel_client);

// Create worker thread
extern void minitel_init();

#endif