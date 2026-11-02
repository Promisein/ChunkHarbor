#pragma once
#include <QWidget>
class QVBoxLayout;
class WorkspacePage : public QWidget {
  public:
    WorkspacePage(const QString &title, const QString &subtitle, QWidget *parent = nullptr);

  protected:
    QVBoxLayout *layout_;
    void placeholder(const QString &title, const QString &description);
};
