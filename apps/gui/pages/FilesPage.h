#pragma once
#include "WorkspacePage.h"
class FileListController;
class DownloadController;
class FilesPage final : public WorkspacePage {
  public:
    FilesPage(FileListController *, QWidget *parent = nullptr, DownloadController *download = nullptr);
    QWidget *transferBar() const { return transferBar_; }
  private:
    QWidget *transferBar_ = nullptr;
};
