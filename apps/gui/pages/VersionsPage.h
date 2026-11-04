#pragma once
#include "WorkspacePage.h"
class WorkspaceStore;
class DownloadController;
class ServerHistoryPanel;
class VersionsPage final : public WorkspacePage {
  public:
    VersionsPage(WorkspaceStore *, QWidget *parent = nullptr, DownloadController *download = nullptr);
    void setServerEndpoint(const QString &host, quint16 port);
  private:
    ServerHistoryPanel *serverHistory_;
};
