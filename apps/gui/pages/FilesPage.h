#pragma once
#include "WorkspacePage.h"
class WorkspaceStore;
class FilesPage final : public WorkspacePage {
  public:
    FilesPage(WorkspaceStore *, QWidget *parent = nullptr);
};
