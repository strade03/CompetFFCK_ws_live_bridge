#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    m_bridge = new LiveBridge(this);
    m_isRunning = false;
    
    connect(m_bridge, &LiveBridge::logMessage, this, &MainWindow::appendLog);
}

MainWindow::~MainWindow()
{
    if (m_isRunning) m_bridge->stop();
    delete ui;
}

void MainWindow::appendLog(const QString& msg)
{
    ui->txtConsole->appendPlainText(msg);
}

void MainWindow::on_btnStartStop_clicked()
{
    if (m_isRunning) {
        m_bridge->stop();
        m_isRunning = false;
        setRunningState(false);
    } else {
        QString codex = ui->cbCodex->currentText().trimmed();
        if(codex.isEmpty()) {
            QMessageBox::warning(this, "Erreur", "Veuillez sélectionner un Codex !");
            return;
        }

        LiveBridge::Config cfg;
        cfg.dbHost = ui->leDbHost->text(); cfg.dbPort = ui->leDbPort->text().toInt();
        cfg.dbUser = ui->leDbUser->text(); cfg.dbPass = ui->leDbPass->text();
        cfg.dbName = ui->leDbName->text();
        cfg.notifHost = ui->leNotifHost->text(); cfg.notifPort = ui->leNotifPort->text().toInt();
        cfg.wsPort = ui->leWsPort->text().toInt();
        cfg.keyRace = codex;

        ui->txtConsole->clear();
        if (m_bridge->start(cfg)) {
            m_isRunning = true;
            setRunningState(true);
        }
    }
}

void MainWindow::on_btnRefreshCodex_clicked()
{
    LiveBridge::Config cfg;
    cfg.dbHost = ui->leDbHost->text(); cfg.dbPort = ui->leDbPort->text().toInt();
    cfg.dbUser = ui->leDbUser->text(); cfg.dbPass = ui->leDbPass->text();
    cfg.dbName = ui->leDbName->text();

    ui->cbCodex->clear();
    QStringList list = m_bridge->fetchCodex(cfg);
    ui->cbCodex->addItems(list);
}

void MainWindow::setRunningState(bool running)
{
    if (running) {
        ui->btnStartStop->setText("Stop le Serveur Live");
        ui->btnStartStop->setStyleSheet("background-color: #f44336; color: white; font-weight: bold; font-size: 14px;");
        ui->frameConfig->setEnabled(false);
    } else {
        ui->btnStartStop->setText("Lancer le Serveur Live");
        ui->btnStartStop->setStyleSheet("background-color: #4CAF50; color: white; font-weight: bold; font-size: 14px;");
        ui->frameConfig->setEnabled(true);
    }
}
