//- Copyright 2015 the Neutrino authors (see AUTHORS).
//- Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "bytestream.hh"

#include "async/promise-inl.hh"
#include "marshal-inl.hh"
#include "rpc.hh"
#include "sync/thread.hh"
#include "test/asserts.hh"
#include "test/unittest.hh"

using namespace plankton;
using namespace plankton::rpc;
using namespace tclib;

TEST(rpc, byte_buffer_simple) {
  ByteBufferStream stream(374);
  ASSERT_TRUE(stream.initialize());
  for (size_t io = 0; io < 374; io++) {
    size_t offset = io * 7;
    for (size_t ii = 0; ii < 373; ii++) {
      byte_t value = static_cast<byte_t>(offset + (5 * ii));
      WriteIop iop(&stream, &value, 1);
      ASSERT_TRUE(iop.execute());
      ASSERT_EQ(1, iop.bytes_written());
    }
    for (size_t ii = 0; ii < 373; ii++) {
      byte_t value = 0;
      ReadIop iop(&stream, &value, 1);
      ASSERT_TRUE(iop.execute());
      ASSERT_EQ(1, iop.bytes_read());
      ASSERT_EQ(value, static_cast<byte_t>(offset + (5 * ii)));
    }
  }
  ASSERT_TRUE(stream.close());
  byte_t value = 0;
  ReadIop iop(&stream, &value, 1);
  ASSERT_TRUE(iop.execute());
  ASSERT_EQ(0, iop.bytes_read());
  ASSERT_TRUE(iop.at_eof());
}

TEST(rpc, byte_buffer_delayed_eof) {
  // Check that if we close the stream before the contents have all been read
  // those contents are still available to be read before the stream eofs.
  ByteBufferStream stream(374);
  ASSERT_TRUE(stream.initialize());
  byte_t buf[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  WriteIop write(&stream, buf, 10);
  ASSERT_TRUE(write.execute());
  ASSERT_EQ(10, write.bytes_written());
  ASSERT_TRUE(stream.close());
  memset(buf, 0, 10);
  ReadIop read(&stream, buf, 10);
  ASSERT_TRUE(read.execute());
  ASSERT_EQ(10, read.bytes_read());
  ASSERT_FALSE(read.at_eof());
  read.recycle();
  ASSERT_TRUE(read.execute());
  ASSERT_EQ(0, read.bytes_read());
  ASSERT_TRUE(read.at_eof());
  read.recycle();
  ASSERT_TRUE(read.execute());
  ASSERT_EQ(0, read.bytes_read());
  ASSERT_TRUE(read.at_eof());
}

class Slice {
public:
  Slice(ByteBufferStream *nexus, NativeSemaphore *lets_go, Slice **slices, uint32_t index);
  void start();
  void join();
  static const uint32_t kSliceCount = 16;
  static const uint32_t kStepCount = 1600;
private:
  opaque_t run_producer();
  opaque_t run_distributer();
  opaque_t run_validator();
  byte_t get_value(size_t step) { return static_cast<byte_t>((index_ << 4) + (step & 0xF)); }
  size_t get_origin(byte_t value) { return value >> 4; }
  size_t get_step(byte_t value) { return value & 0xF; }
  ByteBufferStream *nexus_;
  NativeSemaphore *lets_go_;
  ByteBufferStream stream_;
  Slice **slices_;
  uint32_t index_;
  NativeThread producer_;
  NativeThread distributer_;
  NativeThread validator_;
};

Slice::Slice(ByteBufferStream *nexus, NativeSemaphore *lets_go, Slice **slices, uint32_t index)
  : nexus_(nexus)
  , lets_go_(lets_go)
  , stream_(57 + index)
  , slices_(slices)
  , index_(index) {
  ASSERT_TRUE(stream_.initialize());
  producer_ = new_callback(&Slice::run_producer, this);
  distributer_ = new_callback(&Slice::run_distributer, this);
  validator_ = new_callback(&Slice::run_validator, this);
}

void Slice::start() {
  validator_.start();
  distributer_.start();
  producer_.start();
}

void Slice::join() {
  ASSERT_TRUE(validator_.join(NULL));
  ASSERT_TRUE(distributer_.join(NULL));
  ASSERT_TRUE(producer_.join(NULL));
}

opaque_t Slice::run_producer() {
  lets_go_->acquire();
  for (size_t i = 0; i < kStepCount; i++) {
    byte_t value = get_value(i);
    WriteIop iop(nexus_, &value, 1);
    ASSERT_TRUE(iop.execute());
  }
  return o0();
}

opaque_t Slice::run_distributer() {
  for (size_t i = 0; i < kStepCount; i++) {
    byte_t value = 0;
    ReadIop read_iop(nexus_, &value, 1);
    ASSERT_TRUE(read_iop.execute());
    size_t origin = get_origin(value);
    WriteIop write_iop(&slices_[origin]->stream_, &value, 1);
    ASSERT_TRUE(write_iop.execute());
  }
  return o0();
}

opaque_t Slice::run_validator() {
  size_t counts[kSliceCount];
  for (size_t i = 0; i < kSliceCount; i++)
    counts[i] = 0;
  for (size_t i = 0; i < kStepCount; i++) {
    byte_t value = 0;
    ReadIop iop(&stream_, &value, 1);
    iop.execute();
    size_t origin = get_origin(value);
    ASSERT_EQ(index_, origin);
    size_t step = get_step(value);
    counts[step]++;
  }
  for (size_t i = 0; i < kSliceCount; i++)
    ASSERT_EQ(kStepCount / kSliceCount, counts[i]);
  return o0();
}

TEST(rpc, byte_buffer_concurrent) {
  // This is a bit intricate. It works like this. There's N producers all
  // writing concurrently to the same stream, the nexus. Then there's N threads,
  // the distributers, reading values back out from the nexus. Each value is
  // tagged with which produces wrote it, the distributer writes values from
  // producer i to stream i. Each of these N streams have a thread reading
  // values out and checking that they all came from producer i and that the
  // payload is as expected. That's it. A lot going on.
  ByteBufferStream nexus(41);
  ASSERT_TRUE(nexus.initialize());
  Slice *slices[Slice::kSliceCount];
  NativeSemaphore lets_go(0);
  ASSERT_TRUE(lets_go.initialize());
  for (uint32_t i = 0; i < Slice::kSliceCount; i++)
    slices[i] = new Slice(&nexus, &lets_go, slices, i);
  for (uint32_t i = 0; i < Slice::kSliceCount; i++)
    slices[i]->start();
  for (uint32_t i = 0; i < Slice::kSliceCount; i++)
    lets_go.release();
  for (uint32_t i = 0; i < Slice::kSliceCount; i++)
    slices[i]->join();
  for (uint32_t i = 0; i < Slice::kSliceCount; i++)
    delete slices[i];
}

static void handle_request(MessageSocket::ResponseCallback *callback_out,
    IncomingRequest *request, MessageSocket::ResponseCallback callback) {
  ASSERT_TRUE(request->subject() == Variant("test_subject"));
  ASSERT_TRUE(request->selector() == Variant("test_selector"));
  ASSERT_TRUE(request->arguments() == Variant("test_arguments"));
  *callback_out = callback;
}

// An rpc channel that uses the same buffer for requests and responses.
class SharedRpcChannel {
public:
  SharedRpcChannel(MessageSocket::RequestCallback handler);
  bool process_next_instruction();
  MessageSocket *operator->() { return channel()->socket(); }
  StreamServiceConnector *channel() { return &channel_; }
  bool close() { return bytes_.close(); }
private:
  ByteBufferStream bytes_;
  StreamServiceConnector channel_;
};

SharedRpcChannel::SharedRpcChannel(MessageSocket::RequestCallback handler)
  : bytes_(1024)
  , channel_(&bytes_, &bytes_) {
  ASSERT_TRUE(bytes_.initialize());
  ASSERT_TRUE(channel_.init(handler));
}

bool SharedRpcChannel::process_next_instruction() {
  return channel()->input()->process_next_instruction(NULL);
}

TEST(rpc, roundtrip) {
  MessageSocket::ResponseCallback on_response;
  SharedRpcChannel channel(new_callback(handle_request, &on_response));
  OutgoingRequest request("test_subject", "test_selector");
  request.set_arguments("test_arguments");
  IncomingResponse incoming = channel->send_request(&request);
  ASSERT_FALSE(incoming->is_settled());
  while (on_response.is_empty())
    ASSERT_TRUE(channel.process_next_instruction());
  ASSERT_FALSE(incoming->is_settled());
  on_response(OutgoingResponse::success(Variant::integer(18)));
  while (!incoming->is_settled())
    ASSERT_TRUE(channel.process_next_instruction());
  ASSERT_TRUE(Variant::integer(18) == incoming->peek_value(Variant::null()));
}

class EchoService : public plankton::rpc::Service {
public:
  void echo(RequestData *data, ResponseCallback response);
  void ping(RequestData *data, ResponseCallback response);
  void fallback(RequestData *data, ResponseCallback response);
  EchoService();
  size_t fallback_count;
};

EchoService::EchoService()
  : fallback_count(0) {
  register_method("echo", tclib::new_callback(&EchoService::echo, this));
  register_method("ping", tclib::new_callback(&EchoService::ping, this));
  set_fallback(tclib::new_callback(&EchoService::fallback, this));
}

void EchoService::echo(RequestData *data, ResponseCallback callback) {
  callback(OutgoingResponse::success(data->argument(0)));
}

void EchoService::ping(RequestData *data, ResponseCallback callback) {
  callback(OutgoingResponse::success("pong"));
}

void EchoService::fallback(RequestData *data, ResponseCallback callback) {
  fallback_count++;
  callback(OutgoingResponse::success("you sunk my battleship"));
}

TEST(rpc, service) {
  EchoService echo;
  SharedRpcChannel channel(echo.handler());
  Variant args[1] = {43};
  OutgoingRequest req0(Variant::null(), "echo", 1, args);
  IncomingResponse inc0 = channel->send_request(&req0);
  OutgoingRequest req1(Variant::null(), "echo");
  IncomingResponse inc1 = channel->send_request(&req1);
  OutgoingRequest req2(Variant::null(), "ping");
  IncomingResponse inc2 = channel->send_request(&req2);
  OutgoingRequest req3(Variant::null(), "foobeliboo");
  IncomingResponse inc3 = channel->send_request(&req3);
  while (!inc3->is_settled())
    channel.process_next_instruction();
  ASSERT_EQ(43, inc0->peek_value(Variant::null()).integer_value());
  ASSERT_TRUE(inc1->peek_value(10).is_null());
  ASSERT_TRUE(Variant::string("pong") == inc2->peek_value(10));
  ASSERT_EQ(1, echo.fallback_count);
  ASSERT_TRUE(Variant::string("you sunk my battleship") == inc3->peek_value(Variant::null()));
}

static opaque_t run_client(ByteBufferStream *down, ByteBufferStream *up) {
  EchoService echo;
  StreamServiceConnector client(down, up);
  ASSERT_TRUE(client.init(echo.handler()));
  ASSERT_TRUE(client.process_all_messages());
  ASSERT_TRUE(up->close());
  return o0();
}

TEST(rpc, async_service) {
  ByteBufferStream down(1024);
  ASSERT_TRUE(down.initialize());
  ByteBufferStream up(1024);
  ASSERT_TRUE(up.initialize());
  StreamServiceConnector server(&up, &down);
  NativeThread client(new_callback(run_client, &down, &up));
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(server.init(empty_callback()));

  Variant arg = 54;
  OutgoingRequest req(Variant::null(), "echo", 1, &arg);
  IncomingResponse inc = server.socket()->send_request(&req);
  ASSERT_TRUE(down.close());
  ASSERT_TRUE(server.process_all_messages());
  ASSERT_TRUE(client.join(NULL));
  ASSERT_TRUE(inc->is_fulfilled());
  ASSERT_EQ(54, inc->peek_value(Variant::null()).integer_value());
}
