#pragma once
#include "WorkspacePage.h"
class WorkspaceStore;
class VersionsPage final : public WorkspacePage {
  public:
    VersionsPage(WorkspaceStore *, QWidget *parent = nullptr);
};
