#pragma once
#include "WorkspacePage.h"
class TransferController;
class WorkspaceStore;
class QtTcpTransport;
class DashboardPage final : public WorkspacePage {
  public:
    DashboardPage(TransferController *, WorkspaceStore *, QtTcpTransport *,
                  QWidget *parent = nullptr);
};
