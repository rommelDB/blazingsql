#pragma once

#include <memory>

#include "blazingdb/transport/Address.h"

namespace blazingdb {
namespace transport {

/// \brief A Node is the representation of a RAL component used in the transport
/// process.
class Node {
public:
  // TODO define clear constructors
  Node(const std::shared_ptr<Address>& address, bool isAvailable = true);

  bool operator==(const Node& rhs) const;
  bool operator!=(const Node& rhs) const;

  std::shared_ptr<Address> address() const noexcept;

  /// Note: Not implemented yet
  bool isAvailable() const;

  /// Note: Not implemented yet
  void setAvailable(bool available);

  void print() const;

  static std::shared_ptr<blazingdb::transport::Node> Make(
      const std::shared_ptr<Address>& address);

protected:
  std::shared_ptr<Address> address_;
  bool isAvailable_{false};
};

}  // namespace transport
}  // namespace blazingdb
