#pragma once
#include "chunkharbor/net/TcpConnection.h"
void echoFrames(const chunkharbor::TcpConnection::Ptr& connection, chunkharbor::Buffer& input);
