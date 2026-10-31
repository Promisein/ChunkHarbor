#pragma once
#include "chunkharbor/net/TcpConnection.h"
namespace chunkharbor {
class UploadStorage;
class DiskExecutor;
} // namespace chunkharbor
// Storage and executor must outlive all connections and pending completions.
chunkharbor::TcpConnection::MessageCallback makeUploadHandler(chunkharbor::UploadStorage &storage,
                                                              chunkharbor::DiskExecutor &executor,
                                                              chunkharbor::EventLoop &loop);
