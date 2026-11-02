#pragma once
#include "WorkspacePage.h"
class FileListController;
class FilesPage final : public WorkspacePage {
  public:
    FilesPage(FileListController *, QWidget *parent = nullptr);
};
