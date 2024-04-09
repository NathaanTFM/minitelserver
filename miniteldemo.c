#include <stdlib.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pjsua-lib/pjsua.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>

#include "minitelserver.h"

struct sip_creds {
    char *domain;
    char *user;
    char *passwd;
};

#include "accounts.h"

// accounts.h contains an array of sip_creds. Example:
/* static const struct sip_creds accounts[] = {
    {"sip.example.com", "00339xxxxxxxx", "password"},
    {"sip.example.com", "00339xxxxxxxx", "password2"}
}; */

static PyObject * minitelserver_start(PyObject *self, PyObject *args);

static int is_init = 0;
static PyObject *callback = NULL;

static pthread_cond_t cond;
static pthread_mutex_t mutex;

struct pending_call {
    int type;
    
    minitel_client client;
    void *userdata;
    uint8_t byte; 
};

static struct {
    struct pending_call *ptr;
    int pos;
    int size;
    int capacity;

} pending;

static void add_pending_call_internal(struct pending_call *call) {
    if (pending.size >= pending.capacity) {
        pending.capacity += 64;
        pending.ptr = realloc(pending.ptr, sizeof(struct pending_call) * pending.capacity);
        
        if ((pending.size + pending.pos) > pending.capacity) {
            memmove(&pending.ptr[pending.pos + 64], &pending.ptr[pending.pos], pending.capacity - pending.pos);
            pending.pos += 64;
        }
    }
    
    int target = (pending.pos + pending.size) % pending.capacity;
    pending.ptr[target] = *call;
    
    pending.size++;
}

static void add_pending_call(int type, minitel_client client, void *userdata, uint8_t byte) {
    struct pending_call call;
    call.type = type;
    call.client = client;
    call.userdata = userdata;
    call.byte = byte;
    
    pthread_mutex_lock(&mutex);
    add_pending_call_internal(&call);
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}

static int get_pending_call(struct pending_call *target) {
    if (pending.size == 0)
        return 0;
    
    *target = pending.ptr[pending.pos];
    pending.pos = (pending.pos + 1) % pending.capacity;
    pending.size--;
    
    return 1;
}

// Python API
typedef struct {
    PyObject_HEAD
    minitel_client client;
    
} MinitelTransportObject;

struct minitel_python {
    int gone;
    PyObject *obj;
    MinitelTransportObject *transport;
};

static void MinitelTransport_dealloc(PyObject *self) {
    Py_TYPE(self)->tp_free(self);
}

PyObject * MinitelTransport_transmit(PyObject *self, PyObject *args) {
    const char *data;
    Py_ssize_t len;
    
    if (!PyArg_ParseTuple(args, "y#", &data, &len))
        return NULL;
        
    minitel_client client = ((MinitelTransportObject *)self)->client;
    if (!client) {
        Py_RETURN_FALSE;
    }
    
    minitel_transmit(client, (const uint8_t *)data, len);
    Py_RETURN_TRUE;
}

PyObject * MinitelTransport_disconnect(PyObject *self, PyObject *args) {
    int hangup;
    
    if (!PyArg_ParseTuple(args, "p", &hangup))
        return NULL;
    
    minitel_client client = ((MinitelTransportObject *)self)->client;
    if (client) {
        minitel_disconnect(client, hangup);
    }
    Py_RETURN_NONE;
}

static PyMethodDef MinitelTransport_methods[] = {
    {"transmit", (PyCFunction)MinitelTransport_transmit, METH_VARARGS, NULL},
    {"disconnect", (PyCFunction)MinitelTransport_disconnect, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject MinitelTransportType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "minitelserver.MinitelTransport",
    .tp_basicsize = sizeof(MinitelTransportObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_methods = MinitelTransport_methods,
    .tp_dealloc = MinitelTransport_dealloc
};
/*
static void minitelserver_free(void *arg) {
    pjsua_destroy();
}
*/
static PyMethodDef module_methods[] = {
    {"start", minitelserver_start, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "minitelserver",
    .m_doc = NULL,
    .m_size = -1,
    .m_methods = module_methods,
    //.m_free = minitelserver_free
};

PyMODINIT_FUNC PyInit_minitelserver(void) {
    if (PyType_Ready(&MinitelTransportType) < 0)
        return NULL;
    
    PyObject *m = PyModule_Create(&module);
    if (!m) {
        return NULL;
    }
    
    Py_INCREF(&MinitelTransportType);
    if (PyModule_AddObject(m, "MinitelTransport", (PyObject *)&MinitelTransportType) < 0) {
        Py_DECREF(&MinitelTransportType);
        Py_DECREF(m);
        return NULL;
    }
    
    return m;
}

// Minitel API
static void connection_made(minitel_client client, void **userdata) {
    struct minitel_python *py = (struct minitel_python *)malloc(sizeof(struct minitel_python));
    py->gone = 0;
    py->obj = NULL;
    
    *userdata = (void *)py;
    add_pending_call(1, client, *userdata, 0);
}

static int connection_made_py(minitel_client client, void *userdata) {
    struct minitel_python *py = (struct minitel_python *)userdata;
    
    // give up already, we're too late
    if (py->gone)
        return 1;
    
    if (!callback) {
        fprintf(stderr, "no callback -- disconnecting!\n");
        minitel_disconnect(client, 1);
        return 0;
    }
    
    MinitelTransportObject *transport = (MinitelTransportObject *)PyType_GenericNew(&MinitelTransportType, NULL, NULL);
    
    if (transport == NULL) {
        fprintf(stderr, "error generating transport -- disconnecting!\n");
        minitel_disconnect(client, 1);
        return 0;
    }
        
    // give the client to the transport
    transport->client = client;
    //transport->py = py;
    
    // call the callback function - this should give us an instance
    PyObject *ret = PyObject_CallOneArg(callback, (PyObject *)transport);
    //Py_DECREF(transport);   
    
    if (ret == NULL) {
        fprintf(stderr, "error calling callback -- disconnecting!\n");
        minitel_disconnect(client, 1);
        return 0;
    }

    // We stole the reference
    py->obj = ret;
    py->transport = transport;
    return 1;
}

static void connection_lost(minitel_client client, void *userdata) {
    struct minitel_python *py = (struct minitel_python *)userdata;
    py->gone = 1;
    if (py->transport)
        py->transport->client = NULL;
    
    add_pending_call(2, client, userdata, 0);
}

static int connection_lost_py(minitel_client client, void *userdata) {
    struct minitel_python *py = (struct minitel_python *)userdata;
    
    int status = 1;
    
    if (py->obj != NULL) {
        // Call connection_lost
        PyObject *ptype, *pvalue, *ptraceback;
            
        PyObject *ret = PyObject_CallMethod(py->obj, "connection_lost", NULL);
        if (ret != NULL) {
            Py_DECREF(ret);
        } else {
            status = 0;
            PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        }
        Py_DECREF(py->obj);
        
        if (status == 0) {
            PyErr_Restore(ptype, pvalue, ptraceback);
        }
    }
    
    py->obj = NULL;
    free(py);
    
    return status;
}

static void byte_received(minitel_client client, void *userdata, uint8_t byte) {
    add_pending_call(3, client, userdata, byte);
}

static int byte_received_py(minitel_client client, void *userdata, uint8_t byte) {
    struct minitel_python *py = (struct minitel_python *)userdata;
    
    if (py->gone)
        return 1;
    
    PyObject *ret = PyObject_CallMethod(py->obj, "byte_received", "B", byte);
    if (ret == NULL) {
        return 0;
    }
    
    Py_DECREF(ret);
    return 1;
}

// PJSUA
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata) {
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);

    printf("Incoming call from %.*s!!\n", (int)ci.remote_info.slen, ci.remote_info.ptr);
    pjsua_call_answer(call_id, 200, NULL, NULL);
}

/* Callback called by the library when call's state has changed */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
    pjsua_call_info ci;

    PJ_UNUSED_ARG(e);

    pjsua_call_get_info(call_id, &ci);
    printf("Call %d state=%.*s\n", call_id, (int)ci.state_text.slen, ci.state_text.ptr);
    
    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        // if we're currently on a minitel call, stop it
        minitel_stop(call_id);
    }
}

/* Callback called by the library when call's media state has changed */
static void on_call_media_state(pjsua_call_id call_id) {
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);
    
    printf("Call %d media state %d\n", call_id, ci.media_status);

    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
        struct minitel_callbacks cb;
        cb.connection_made = connection_made;
        cb.connection_lost = connection_lost;
        cb.byte_received = byte_received;
        
        int status = minitel_start(call_id, &cb);
        printf("status %d\n", status);
        
    } else if (ci.media_status == PJSUA_CALL_MEDIA_LOCAL_HOLD || ci.media_status == PJSUA_CALL_MEDIA_REMOTE_HOLD) {
        pjsua_call_hangup(call_id, 0, NULL, NULL);
        
    }
}

/* Display error and exit application */
static void error_exit(const char *title, pj_status_t status) {
    pjsua_perror("minitel", title, status);
    pjsua_destroy();
    exit(1);
}

/*int main(int argc, char *argv[]) {
    printf("Minitel Server\n");
    
    if (argc != 3) {
        fprintf(stderr, "Usage: %s HOST PORT\n", argv[0]);
        return 1;
    }*/
    

static int server_loop() {
    PyThreadState *thread = PyEval_SaveThread();
    
    struct pending_call call;
    struct timespec ts;
    
    for (;;) {
        pthread_mutex_lock(&mutex);
        if (!get_pending_call(&call)) {
            for (;;) {            
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1;
                pthread_cond_timedwait(&cond, &mutex, &ts);
                
                if (get_pending_call(&call))
                    break;
                
                PyEval_RestoreThread(thread);
                
                if (PyErr_CheckSignals() < 0)
                    return 0;
                
                thread = PyEval_SaveThread();
                
            }
        }
        pthread_mutex_unlock(&mutex);
        
        // we can handle it here
        PyEval_RestoreThread(thread);
        
        printf("handle call type %d\n", call.type);
        
        switch (call.type) {
            case 1:
                // connection made
                if (!connection_made_py(call.client, call.userdata))
                    return 0;
                
                break;
                
            case 2:
                // connection lost
                if (!connection_lost_py(call.client, call.userdata))
                    return 0;
                
                break;
                
            case 3:
                // byte received
                if (!byte_received_py(call.client, call.userdata, call.byte))
                    return 0;
                
                break;
        }
        
        thread = PyEval_SaveThread();
    }
}
    
static PyObject * minitelserver_start(PyObject *self, PyObject *args) {
    if (is_init) {
        Py_RETURN_FALSE;
    }
    is_init = 1;
    
    if (!PyArg_ParseTuple(args, "O", &callback)) {
        return NULL;
    }
    
    Py_INCREF(callback);
    
    // initializing thread and mutex
    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&mutex, NULL);
    
    // initialize pending calls
    pending.pos = 0;
    pending.size = 0;
    pending.capacity = 0;
    pending.ptr = NULL;
    
    pjsua_acc_id acc_id;
    pj_status_t status;

    /* Create pjsua first! */
    status = pjsua_create();
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);
    
    pj_caching_pool cp;
    pj_caching_pool_init(&cp, &pj_pool_factory_default_policy, 0);
    
    /* Init pjsua */
    {
        pjsua_config cfg;
        pjsua_logging_config log_cfg;
        pjsua_media_config media_cfg;
        
        pjsua_config_default(&cfg);
        cfg.max_calls = PJSUA_MAX_CALLS;
        cfg.cb.on_incoming_call = &on_incoming_call;
        cfg.cb.on_call_media_state = &on_call_media_state;
        cfg.cb.on_call_state = &on_call_state;

        pjsua_logging_config_default(&log_cfg);
        log_cfg.level = 0;//4;
        log_cfg.console_level = 0;//4;
        //log_cfg.log_filename = pj_str("log.txt");
        
        pjsua_media_config_default(&media_cfg);
        media_cfg.no_vad = 1; // we are not voice!
        
        status = pjsua_init(&cfg, &log_cfg, &media_cfg);
        if (status != PJ_SUCCESS) error_exit("Error in pjsua_init()", status);
    }

    pjsua_set_null_snd_dev();

    /* Add UDP transport. */
    {
        pjsua_transport_config cfg;

        pjsua_transport_config_default(&cfg);
        cfg.port = 5060;
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
        if (status != PJ_SUCCESS) error_exit("Error creating transport", status);
    }
    
    // Start minitel pending calls 
    minitel_init();

    /* Initialization is done, now start pjsua */
    status = pjsua_start();
    
    if (status != PJ_SUCCESS) error_exit("Error starting pjsua", status);

    /* Register to SIP server by creating SIP account. */
    {
        char buf_id[128];
        char buf_reg_uri[128];
        
        for (size_t i = 0; i < sizeof(accounts) / sizeof(struct sip_creds); i++) {
            const struct sip_creds *cred = &accounts[i];
            
            snprintf(buf_id, sizeof(buf_id), "sip:%s@%s", cred->user, cred->domain);
            snprintf(buf_reg_uri, sizeof(buf_reg_uri), "sip:%s", cred->domain);
            
            pjsua_acc_config cfg;

            pjsua_acc_config_default(&cfg);
            cfg.id = pj_str(buf_id);
            cfg.reg_uri = pj_str(buf_reg_uri);
            cfg.cred_count = 1;
            cfg.cred_info[0].realm = pj_str(cred->domain);
            cfg.cred_info[0].scheme = pj_str("digest");
            cfg.cred_info[0].username = pj_str(cred->user);
            cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
            cfg.cred_info[0].data = pj_str(cred->passwd);

            status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
            printf("Created account %s: %d\n", cred->user, status);
            if (status != PJ_SUCCESS) error_exit("Error adding account", status);
        }
    }
    
    // start pending calls
    int loop_ok = server_loop();
    
    pjsua_destroy();
    
    if (loop_ok) {
        Py_RETURN_NONE;
        
    } else {
        return NULL;
    }
}