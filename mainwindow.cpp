#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QHBoxLayout>
#include <QLabel>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    m_bridge = new LiveBridge(this);
    m_isRunning = false;
    
    // Rename the button
    ui->btnRefreshCodex->setText("Lister Courses");
    
    // Add filter combo next to the refresh button
    QWidget *parentWidget = ui->btnRefreshCodex->parentWidget();
    QLayout *parentLayout = parentWidget ? parentWidget->layout() : nullptr;
    
    m_cbFilter = new QComboBox(this);
    m_cbFilter->setMinimumWidth(80);
    m_cbFilter->addItems({"Tout", "SLA", "DES", "EXS", "OCR", "DRB", "FRE", "JEU", "MAR"});
    m_cbFilter->setToolTip("Filtrer par type d'activité");
    
    if (parentLayout) {
        QBoxLayout *boxLayout = qobject_cast<QBoxLayout*>(parentLayout);
        if (boxLayout) {
            int btnIndex = boxLayout->indexOf(ui->btnRefreshCodex);
            if (btnIndex >= 0) {
                QLabel *lblFilter = new QLabel("Filtre :", this);
                boxLayout->insertWidget(btnIndex, m_cbFilter);
                boxLayout->insertWidget(btnIndex, lblFilter);
            } else {
                boxLayout->addWidget(new QLabel("Filtre :", this));
                boxLayout->addWidget(m_cbFilter);
            }
        } else {
            parentLayout->addWidget(m_cbFilter);
        }
    } else {
        // Fallback: place near cbCodex
        QWidget *codexParent = ui->cbCodex->parentWidget();
        QLayout *codexLayout = codexParent ? codexParent->layout() : nullptr;
        if (codexLayout) {
            QBoxLayout *bl = qobject_cast<QBoxLayout*>(codexLayout);
            if (bl) {
                bl->addWidget(new QLabel("Filtre :", this));
                bl->addWidget(m_cbFilter);
            }
        }
    }
    
    // Auto-refresh when filter changes
    connect(m_cbFilter, &QComboBox::currentTextChanged, this, [this](const QString&) {
        if (ui->cbCodex->count() > 0) {
            on_btnRefreshCodex_clicked();
        }
    });
    
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
        QString selected = ui->cbCodex->currentText().trimmed();
        if (selected.isEmpty()) {
            QMessageBox::warning(this, "Erreur", "Veuillez sélectionner une course !");
            return;
        }
        
        QString codex = selected.split(" | ").first().trimmed();

        LiveBridge::Config cfg;
        cfg.dbHost = ui->leDbHost->text(); cfg.dbPort = ui->leDbPort->text().toInt();
        cfg.dbUser = ui->leDbUser->text(); cfg.dbPass = ui->leDbPass->text();
        cfg.dbName = ui->leDbName->text();
        cfg.notifHost = ui->leNotifHost->text(); cfg.notifPort = ui->leNotifPort->text().toInt();
        cfg.wsPort = ui->leWsPort->text().toInt();
        cfg.keyRace = codex;

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

    QString filter = m_cbFilter->currentText();
    
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