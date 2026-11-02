#pragma once
#include "WorkspacePage.h"
class WorkspaceStore;
class LogsPage final : public WorkspacePage {
  public:
    LogsPage(WorkspaceStore *, QWidget *parent = nullptr);
};
