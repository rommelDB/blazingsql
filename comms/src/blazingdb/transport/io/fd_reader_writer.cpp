#include "blazingdb/transport/io/fd_reader_writer.h"
#include <netinet/in.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <queue>
#include <thread>
#include <zmq.hpp>
#include "blazingdb/transport/ColumnTransport.h"

namespace blazingdb {
namespace transport {
namespace io {

size_t readFromSocket(void* fileDescriptor, char* buf, size_t nbyte) {
  zmq::socket_t* socket = (zmq::socket_t*)fileDescriptor;
  zmq::message_t msg;

  socket->recv(msg);
  memcpy(buf, msg.data(), nbyte);

  return nbyte;
}

size_t writeToSocket(void* fileDescriptor, const char* buf, size_t nbyte, bool more) {
  zmq::socket_t* socket = (zmq::socket_t*)fileDescriptor;
  zmq::message_t message(nbyte);

  memcpy(message.data(), buf, nbyte);
  socket->send(message, more ? zmq::send_flags::sndmore : zmq::send_flags::none);

  return nbyte;
}

}  // namespace io
}  // namespace transport
}  // namespace blazingdb
