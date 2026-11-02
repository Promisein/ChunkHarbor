#pragma once
#include "WorkspacePage.h"
class TransferController;
class SettingsPage;
class BackupPage final : public WorkspacePage {
  public:
    BackupPage(TransferController *, SettingsPage *, QWidget *parent = nullptr);
};
