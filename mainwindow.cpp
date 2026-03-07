#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QSettings>
#include <QCoreApplication>
#include <QFileInfo>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    m_bridge = new LiveBridge(this);
    m_isRunning = false;
    
    connect(m_bridge, &LiveBridge::logMessage, this, &MainWindow::appendLog);
    
    // Auto-refresh course list when filter changes
    connect(ui->cbFilter, &QComboBox::currentTextChanged, this, [this](const QString&) {
        if (ui->cbCodex->count() > 0)
            on_btnRefreshCodex_clicked();
    });
    
    // Load saved settings (after setupUi so widgets exist)
    loadSettings();
}

MainWindow::~MainWindow()
{
    saveSettings();
    if (m_isRunning) m_bridge->stop();
    delete ui;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Settings (INI file next to the executable)
// ═══════════════════════════════════════════════════════════════════════════

QString MainWindow::settingsPath() const
{
    return QCoreApplication::applicationDirPath() + "/livebridge.ini";
}

void MainWindow::loadSettings()
{
    QString path = settingsPath();
    if (!QFileInfo::exists(path)) {
        appendLog("[CONFIG] Pas de fichier de config, paramètres par défaut.");
        return;
    }
    
    QSettings s(path, QSettings::IniFormat);
    
    s.beginGroup("Database");
    ui->leDbHost->setText(s.value("host", ui->leDbHost->text()).toString());
    ui->leDbPort->setText(s.value("port", ui->leDbPort->text()).toString());
    ui->leDbUser->setText(s.value("user", ui->leDbUser->text()).toString());
    ui->leDbPass->setText(s.value("password", ui->leDbPass->text()).toString());
    ui->leDbName->setText(s.value("name", ui->leDbName->text()).toString());
    s.endGroup();
    
    s.beginGroup("Notification");
    ui->leNotifHost->setText(s.value("host", ui->leNotifHost->text()).toString());
    ui->leNotifPort->setText(s.value("port", ui->leNotifPort->text()).toString());
    s.endGroup();
    
    s.beginGroup("WebSocket");
    ui->leWsPort->setText(s.value("port", ui->leWsPort->text()).toString());
    s.endGroup();
    
    s.beginGroup("UI");
    QString filter = s.value("filter", "Tout").toString();
    int idx = ui->cbFilter->findText(filter);
    if (idx >= 0) ui->cbFilter->setCurrentIndex(idx);
    s.endGroup();
    
    appendLog("[CONFIG] Paramètres chargés depuis " + path);
}

void MainWindow::saveSettings()
{
    QSettings s(settingsPath(), QSettings::IniFormat);
    
    s.beginGroup("Database");
    s.setValue("host", ui->leDbHost->text());
    s.setValue("port", ui->leDbPort->text());
    s.setValue("user", ui->leDbUser->text());
    s.setValue("password", ui->leDbPass->text());
    s.setValue("name", ui->leDbName->text());
    s.endGroup();
    
    s.beginGroup("Notification");
    s.setValue("host", ui->leNotifHost->text());
    s.setValue("port", ui->leNotifPort->text());
    s.endGroup();
    
    s.beginGroup("WebSocket");
    s.setValue("port", ui->leWsPort->text());
    s.endGroup();
    
    s.beginGroup("UI");
    s.setValue("filter", ui->cbFilter->currentText());
    s.endGroup();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Slots
// ═══════════════════════════════════════════════════════════════════════════

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
        QString selected = ui->cbCodex->currentText().trimmed();
        if (selected.isEmpty()) {
            QMessageBox::warning(this, "Erreur", "Veuillez sélectionner une course !");
            return;
        }
        
        // Codex = first part before " | "
        QString codex = selected.split(" | ").first().trimmed();

        LiveBridge::Config cfg;
        cfg.dbHost = ui->leDbHost->text(); cfg.dbPort = ui->leDbPort->text().toInt();
        cfg.dbUser = ui->leDbUser->text(); cfg.dbPass = ui->leDbPass->text();
        cfg.dbName = ui->leDbName->text();
        cfg.notifHost = ui->leNotifHost->text(); cfg.notifPort = ui->leNotifPort->text().toInt();
        cfg.wsPort = ui->leWsPort->text().toInt();
        cfg.keyRace = codex;

        saveSettings();

        ui->txtConsole->clear();
        appendLog("Codex sélectionné: " + codex);
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

    QString filter = ui->cbFilter->currentText();
    
    ui->cbCodex->clear();
    QStringList list = m_bridge->fetchCodex(cfg, filter);
    ui->cbCodex->addItems(list);
    
    if (list.isEmpty())
        appendLog("[INFO] Aucune course trouvée" + (filter != "Tout" ? " pour " + filter : "") + ".");
    else
        appendLog(QString("[INFO] %1 course(s) trouvée(s)%2.")
                  .arg(list.size())
                  .arg(filter != "Tout" ? " pour " + filter : ""));
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