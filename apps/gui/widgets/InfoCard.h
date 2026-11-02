#pragma once
#include <QFrame>
class QVBoxLayout;
class QLabel;
class InfoCard : public QFrame {
  public:
    explicit InfoCard(const QString &title, QWidget *parent = nullptr);
    void setSubtitle(const QString &text);
    QVBoxLayout *body() const { return body_; }

  private:
    QVBoxLayout *body_;
    QLabel *subtitle_ = nullptr;
};
