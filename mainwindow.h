#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include "livebridge.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_btnStartStop_clicked();
    void on_btnRefreshCodex_clicked();
    void appendLog(const QString& msg);

private:
    Ui::MainWindow *ui;
    LiveBridge *m_bridge;
    bool m_isRunning;
    QComboBox *m_cbFilter;
    
    void setRunningState(bool running);
    void loadSettings();
    void saveSettings();
    QString settingsPath() const;
};

#endif // MAINWINDOW_H