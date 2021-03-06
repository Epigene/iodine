/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#include "iodine_websocket.h"
#include "iodine_core.h"
#include "iodine_http.h"
#include "rb-call.h"
#include "rb-registry.h"
#include <arpa/inet.h>
#include <ruby/io.h>

/* *****************************************************************************
Core helpers and data
*/

static VALUE rWebsocket;      // The Iodine::Http::Websocket class
static VALUE rWebsocketClass; // The Iodine::Http::Websocket class
static ID ws_var_id;          // id for websocket pointer
static ID dup_func_id;        // id for the buffer.dup method

size_t iodine_websocket_max_msg_size = 0;
uint8_t iodine_websocket_timeout = 0;

#define set_uuid(object, request)                                              \
  rb_ivar_set((object), fd_var_id, ULONG2NUM((request)->metadata.fd))

inline static intptr_t get_uuid(VALUE obj) {
  VALUE i = rb_ivar_get(obj, fd_var_id);
  return i != Qnil ? (intptr_t)FIX2ULONG(i) : 0;
}

#define set_ws(object, ws)                                                     \
  rb_ivar_set((object), ws_var_id, ULONG2NUM(((VALUE)(ws))))

inline static ws_s *get_ws(VALUE obj) {
  VALUE i = rb_ivar_get(obj, ws_var_id);
  if (i == Qnil)
    return NULL;
  return (ws_s *)FIX2ULONG(i);
}

#define set_handler(ws, handler) websocket_set_udata((ws), (VALUE)handler)
#define get_handler(ws) ((VALUE)websocket_get_udata((ws_s *)(ws)))

/*******************************************************************************
Buffer management - update to change the way the buffer is handled.
*/
struct buffer_s {
  void *data;
  size_t size;
};

/** returns a buffer_s struct, with a buffer (at least) `size` long. */
struct buffer_s create_ws_buffer(ws_s *owner);

/** returns a buffer_s struct, with a buffer (at least) `size` long. */
struct buffer_s resize_ws_buffer(ws_s *owner, struct buffer_s);

/** releases an existing buffer. */
void free_ws_buffer(ws_s *owner, struct buffer_s);

/** Sets the initial buffer size. (4Kb)*/
#define WS_INITIAL_BUFFER_SIZE 4096

// buffer increments by 4,096 Bytes (4Kb)
#define round_up_buffer_size(size) ((((size) >> 12) + 1) << 12)

struct buffer_args {
  struct buffer_s buffer;
  ws_s *ws;
};

void *ruby_land_buffer(void *_buf) {
#define args ((struct buffer_args *)(_buf))
  if (args->buffer.data == NULL) {
    VALUE rbbuff = rb_str_buf_new(WS_INITIAL_BUFFER_SIZE);
    rb_ivar_set(get_handler(args->ws), buff_var_id, rbbuff);
    rb_str_set_len(rbbuff, 0);
    rb_enc_associate(rbbuff, BinaryEncoding);
    args->buffer.data = RSTRING_PTR(rbbuff);
    args->buffer.size = WS_INITIAL_BUFFER_SIZE;

  } else {
    VALUE rbbuff = rb_ivar_get(get_handler(args->ws), buff_var_id);
    rb_str_modify(rbbuff);
    rb_str_resize(rbbuff, args->buffer.size);
    args->buffer.data = RSTRING_PTR(rbbuff);
    args->buffer.size = rb_str_capacity(rbbuff);
  }
  return NULL;
#undef args
}

struct buffer_s create_ws_buffer(ws_s *owner) {
  struct buffer_args args = {.ws = owner};
  RubyCaller.call_c(ruby_land_buffer, &args);
  return args.buffer;
}

struct buffer_s resize_ws_buffer(ws_s *owner, struct buffer_s buffer) {
  buffer.size = round_up_buffer_size(buffer.size);
  struct buffer_args args = {.ws = owner, .buffer = buffer};
  RubyCaller.call_c(ruby_land_buffer, &args);
  return args.buffer;
}
void free_ws_buffer(ws_s *owner, struct buffer_s buff) {
  (void)(owner);
  (void)(buff);
}

#undef round_up_buffer_size

/* *****************************************************************************
Websocket Ruby API
*/

/** Closes the websocket connection. The connection is only closed after
 * existing data in the outgoing buffer is sent. */
static VALUE iodine_ws_close(VALUE self) {
  ws_s *ws = get_ws(self);
  if (((protocol_s *)ws)->service != WEBSOCKET_ID_STR)
    return Qfalse;
  websocket_close(ws);
  return self;
}

/**
 * Writes data to the websocket.
 *
 * Returns `true` on success or `false if the websocket was closed or an error
 * occurred.
 *
 * `write` will return immediately UNLESS resources are insufficient. If the
 * global `write` buffer is full, `write` will block until a buffer "packet"
 * becomes available and can be assigned to the socket. */
static VALUE iodine_ws_write(VALUE self, VALUE data) {
  Check_Type(data, T_STRING);
  ws_s *ws = get_ws(self);
  // if ((void *)ws == (void *)0x04 || (void *)data == (void *)0x04 ||
  //     RSTRING_PTR(data) == (void *)0x04)
  //   fprintf(stderr, "iodine_ws_write: self = %p ; data = %p\n"
  //                   "\t\tString ptr: %p, String length: %lu\n",
  //           (void *)ws, (void *)data, RSTRING_PTR(data), RSTRING_LEN(data));
  if (!ws || ((protocol_s *)ws)->service != WEBSOCKET_ID_STR)
    return Qfalse;
  websocket_write(ws, RSTRING_PTR(data), RSTRING_LEN(data),
                  rb_enc_get(data) == UTF8Encoding);
  return Qtrue;
}

/** Returns the number of active websocket connections (including connections
 * that are in the process of closing down). */
static VALUE iodine_ws_count(VALUE self) {
  ws_s *ws = get_ws(self);
  return LONG2FIX(websocket_count(ws));
}

/**
Returns a weak indication as to the state of the socket's buffer. If the server
has data in the buffer that wasn't written to the socket, `has_pending?` will
return `true`, otherwise `false` will be returned.
*/
static VALUE iodine_ws_has_pending(VALUE self) {
  intptr_t uuid = get_uuid(self);
  return sock_packets_pending(uuid) ? Qtrue : Qfalse;
}

/**
Returns a connection's UUID which is valid for **this process** (not a machine
or internet unique value).

This can be used together with a true process wide UUID to uniquely identify a
connection across the internet.
*/
static VALUE iodine_ws_uuid(VALUE self) {
  intptr_t uuid = get_uuid(self);
  return LONG2FIX(uuid);
}

/* *****************************************************************************
Websocket defer
*/

static void iodine_perform_defer(intptr_t uuid, protocol_s *protocol,
                                 void *arg) {
  (void)(uuid);
  VALUE obj = protocol->service == WEBSOCKET_ID_STR
                  ? get_handler(protocol)
                  : dyn_prot(protocol)->handler;
  RubyCaller.call2((VALUE)arg, call_proc_id, 1, &obj);
  Registry.remove((VALUE)arg);
}

static void iodine_defer_fallback(intptr_t uuid, void *arg) {
  (void)(uuid);
  Registry.remove((VALUE)arg);
}

/**
Schedules a block of code to execute at a later time, **if** the connection is
still
open and while preventing concurent code from running for the same connection
object.

An optional `uuid` argument can be passed along, so that the block of code will
run for the requested connection rather then this connection.

**Careful**: as this might cause this connection's object to run code
concurrently when data owned by this connection is accessed from within the
block of code.

On success returns the block, otherwise (connection invalid) returns `false`. A
sucessful event registration doesn't guaranty that the block will be called (the
connection might close between the event registration and the execution).
*/
static VALUE iodine_defer(int argc, VALUE *argv, VALUE self) {
  intptr_t fd;
  // check arguments.
  if (argc > 1)
    rb_raise(rb_eArgError, "this function expects no more then 1 (optional) "
                           "argument.");
  else if (argc == 1) {
    Check_Type(*argv, T_FIXNUM);
    fd = FIX2LONG(*argv);
    if (!sock_isvalid(fd))
      return Qfalse;
  } else
    fd = iodine_get_fd(self);
  // requires a block to be passed
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qfalse;
  Registry.add(block);

  server_task(fd, iodine_perform_defer, (void *)block, iodine_defer_fallback);
  return block;
}

/* *****************************************************************************
Websocket Multi-Write
*/

static uint8_t iodine_ws_if_callback(ws_s *ws, void *block) {
  VALUE handler = get_handler(ws);
  uint8_t ret = 0;
  if (handler)
    ret = RubyCaller.call2((VALUE)block, call_proc_id, 1, &handler);
  return ret && ret != Qnil && ret != Qfalse;
}

/**
 * Writes data to all the Websocket connections sharing the same process
 * (worker) except `self`.
 *
 * If a block is given, it will be passed each Websocket connection in turn
 * (much like `each`) and send the data only if the block returns a "truthy"
 * value (i.e. NOT `false` or `nil`).
 *
 * See both {#write} and {#each} for more details.
 */
static VALUE iodine_ws_multiwrite(VALUE self, VALUE data) {
  Check_Type(data, T_STRING);
  ws_s *ws = get_ws(self);
  // if ((void *)ws == (void *)0x04 || (void *)data == (void *)0x04 ||
  //     RSTRING_PTR(data) == (void *)0x04)
  //   fprintf(stderr, "iodine_ws_write: self = %p ; data = %p\n"
  //                   "\t\tString ptr: %p, String length: %lu\n",
  //           (void *)ws, (void *)data, RSTRING_PTR(data), RSTRING_LEN(data));
  if (!ws || ((protocol_s *)ws)->service != WEBSOCKET_ID_STR)
    ws = NULL;

  VALUE block = Qnil;
  if (rb_block_given_p())
    block = rb_block_proc();
  websocket_write_each(ws, RSTRING_PTR(data), RSTRING_LEN(data),
                       rb_enc_get(data) == UTF8Encoding, 0,
                       ((block == Qnil) ? NULL : iodine_ws_if_callback),
                       (void *)block);
  return Qtrue;
}

/* *****************************************************************************
Websocket task performance
*/

static void iodine_ws_perform_each_task(intptr_t fd, protocol_s *protocol,
                                        void *data) {
  (void)(fd);
  VALUE handler = get_handler(protocol);
  if (handler)
    RubyCaller.call2((VALUE)data, call_proc_id, 1, &handler);
}
static void iodine_ws_finish_each_task(intptr_t fd, protocol_s *protocol,
                                       void *data) {
  (void)(fd);
  (void)(protocol);
  Registry.remove((VALUE)data);
}

inline static void iodine_ws_run_each(intptr_t origin, VALUE block) {
  server_each(origin, WEBSOCKET_ID_STR, iodine_ws_perform_each_task,
              (void *)block, iodine_ws_finish_each_task);
}

/** Performs a block of code for each websocket connection. The function returns
the block of code.

The block of code should accept a single variable which is the websocket
connection.

i.e.:

      def on_message data
        msg = data.dup; # data will be overwritten once the function exists.
        each {|ws| ws.write msg}
      end


The block of code will be executed asynchronously, to avoid having two blocks
of code running at the same time and minimizing race conditions when using
multilple threads.
 */
static VALUE iodine_ws_each(VALUE self) {
  // requires a block to be passed
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qnil;
  Registry.add(block);
  intptr_t fd = get_uuid(self);
  iodine_ws_run_each(fd, block);
  return block;
}

/**
Runs the required block for each dynamic protocol connection.

Tasks will be performed asynchronously, within each connections lock, so no
connection will have
more then one task being performed at the same time (similar to {#defer}).

Also, unlike {Iodine.run}, the block will **not** be called unless the
connection remains open at the time it's execution is scheduled.

Always returns `self`.
*/
static VALUE iodine_ws_class_each(VALUE self) {
  // requires a block to be passed
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qfalse;
  Registry.add(block);
  iodine_ws_run_each(-1, block);
  return self;
}

/**
Schedules a block of code to run for the specified connection at a later time,
(**if** the connection is open) and while preventing concurent code from running
for the same connection object.

The block of code will receive the connection's object. i.e.

    Iodine::Websocket.defer(uuid) {|ws| ws.write "I'm doing this" }

On success returns the block, otherwise (connection invalid) returns `false`. A
sucessful event registration doesn't guaranty that the block will be called (the
connection might close between the event registration and the execution).
*/
static VALUE iodine_class_defer(VALUE self, VALUE ws_uuid) {
  (void)(self);
  intptr_t fd = FIX2LONG(ws_uuid);
  if (!sock_isvalid(fd))
    return Qfalse;
  // requires a block to be passed
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qfalse;
  Registry.add(block);

  server_task(fd, iodine_perform_defer, (void *)block, iodine_defer_fallback);
  return block;
}

//////////////////////////////////////
// Protocol functions
void ws_on_open(ws_s *ws) {
  VALUE handler = get_handler(ws);
  if (!handler)
    return;
  set_ws(handler, ws);
  RubyCaller.call(handler, on_open_func_id);
}
void ws_on_close(ws_s *ws) {
  VALUE handler = get_handler(ws);
  if (!handler)
    return;
  RubyCaller.call(handler, on_close_func_id);
  Registry.remove(handler);
}
void ws_on_shutdown(ws_s *ws) {
  VALUE handler = get_handler(ws);
  if (!handler)
    return;
  RubyCaller.call(handler, on_shutdown_func_id);
}
void ws_on_ready(ws_s *ws) {
  VALUE handler = get_handler(ws);
  if (!handler)
    return;
  RubyCaller.call(handler, on_ready_func_id);
}
void ws_on_data(ws_s *ws, char *data, size_t length, uint8_t is_text) {
  (void)(data);
  VALUE handler = get_handler(ws);
  if (!handler)
    return;
  VALUE buffer = rb_ivar_get(handler, buff_var_id);
  if (is_text)
    rb_enc_associate(buffer, UTF8Encoding);
  else
    rb_enc_associate(buffer, BinaryEncoding);
  rb_str_set_len(buffer, length);
  RubyCaller.call2(handler, on_message_func_id, 1, &buffer);
}

//////////////////////////////////////
// Protocol constructor

void iodine_websocket_upgrade(http_request_s *request,
                              http_response_s *response, VALUE handler) {
  // make sure we have a valid handler, with the Websocket Protocol mixin.
  if (handler == Qnil || handler == Qfalse) {
    response->status = 400;
    http_response_finish(response);
    return;
  }
  if (TYPE(handler) == T_CLASS) {
    // include the Protocol module
    rb_include_module(handler, rWebsocket);
    rb_extend_object(handler, rWebsocketClass);
    handler = RubyCaller.call(handler, new_func_id);
    // check that we created a handler
  } else {
    // include the Protocol module in the object's class
    VALUE p_class = rb_obj_class(handler);
    rb_include_module(p_class, rWebsocket);
    rb_extend_object(p_class, rWebsocketClass);
  }
  // add the handler to the registry
  Registry.add(handler);
  // set the UUID for the connection
  set_uuid(handler, request);
  // send upgrade response and set new protocol
  websocket_upgrade(.request = request, .response = response,
                    .udata = (void *)handler, .on_close = ws_on_close,
                    .on_open = ws_on_open, .on_shutdown = ws_on_shutdown,
                    .on_ready = ws_on_ready, .on_message = ws_on_data,
                    .max_msg_size = iodine_websocket_max_msg_size,
                    .timeout = iodine_websocket_timeout);
}

//////////////
// Empty callbacks for default implementations.

/**  Please implement your own callback for this event.
 */
static VALUE empty_func(VALUE self) {
  (void)(self);
  return Qnil;
}
// /* The `on_message(data)` callback is the main method for any websocket
// implementation. It is the only required callback for a websocket handler
// (without this handler, errors will occur).
//
// <b>NOTICE</b>: the data passed to the `on_message` callback is the actual
// recycble network buffer, not a copy! <b>Use `data.dup` before moving the data
// out of the function's scope</b> to prevent data corruption (i.e. when
// using the data within an `each` block). For example (broadcasting):
//
//       def on_message data
//         msg = data.dup; # data will be overwritten once the function exists.
//         each {|ws| ws.write msg}
//       end
//
// Please override this method and implement your own callback.
// */
// static VALUE def_dyn_message(VALUE self, VALUE data) {
//   fprintf(stderr,
//           "WARNING: websocket handler on_message override missing or "
//           "bypassed.\n");
//   return Qnil;
// }

/////////////////////////////
// initialize the class and the whole of the Iodine/http library
void Init_iodine_websocket(void) {
  // get IDs and data that's used often
  ws_var_id = rb_intern("iodine_ws_ptr"); // when upgrading
  dup_func_id = rb_intern("dup");         // when upgrading

  // the Ruby websockets protocol class.
  rWebsocket = rb_define_module_under(Iodine, "Websocket");
  if (rWebsocket == Qfalse)
    fprintf(stderr, "WTF?!\n"), exit(-1);
  // // callbacks and handlers
  rb_define_method(rWebsocket, "on_open", empty_func, 0);
  // rb_define_method(rWebsocket, "on_message", def_dyn_message, 1);
  rb_define_method(rWebsocket, "on_shutdown", empty_func, 0);
  rb_define_method(rWebsocket, "on_close", empty_func, 0);
  rb_define_method(rWebsocket, "on_ready", empty_func, 0);
  rb_define_method(rWebsocket, "write", iodine_ws_write, 1);
  rb_define_method(rWebsocket, "each_write", iodine_ws_multiwrite, 1);
  rb_define_method(rWebsocket, "close", iodine_ws_close, 0);

  rb_define_method(rWebsocket, "uuid", iodine_ws_uuid, 0);
  rb_define_method(rWebsocket, "conn_id", iodine_ws_uuid, 0);
  rb_define_method(rWebsocket, "has_pending?", iodine_ws_has_pending, 0);
  rb_define_method(rWebsocket, "defer", iodine_defer, -1);
  rb_define_method(rWebsocket, "each", iodine_ws_each, 0);
  rb_define_method(rWebsocket, "count", iodine_ws_count, 0);

  rb_define_singleton_method(rWebsocket, "each", iodine_ws_class_each, 0);
  rb_define_singleton_method(rWebsocket, "defer", iodine_class_defer, 1);
  rb_define_singleton_method(rWebsocket, "each_write", iodine_ws_multiwrite, 1);

  rWebsocketClass = rb_define_module_under(IodineBase, "WebsocketClass");
  rb_define_method(rWebsocketClass, "each", iodine_ws_class_each, 0);
  rb_define_method(rWebsocketClass, "defer", iodine_class_defer, 1);
}
