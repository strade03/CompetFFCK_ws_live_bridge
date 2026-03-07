#ifndef LIVEBRIDGE_H
#define LIVEBRIDGE_H

#include <QObject>
#include <QSqlDatabase>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSet>
#include <QJsonObject>
#include <QTimer>

class LiveBridge : public QObject
{
    Q_OBJECT
public:
    explicit LiveBridge(QObject *parent = nullptr);
    ~LiveBridge();

    struct Config {
        QString dbHost; int dbPort; QString dbUser; QString dbPass; QString dbName;
        QString notifHost; int notifPort; int wsPort; QString keyRace;
    };

    bool start(const Config& cfg);
    void stop();
    QStringList fetchCodex(const Config& cfg);

signals:
    void logMessage(const QString& msg);

private slots:
    void onNotifConnected();
    void onNotifReadyRead();
    void onNotifDisconnected();
    void tryReconnectNotif();

    void onWsNewConnection();
    void onWsTextMessage(const QString &message);
    void onWsDisconnected();

private:
    Config m_cfg;
    QSqlDatabase m_db;
    QTcpSocket* m_tcpSocket;
    QTimer* m_reconnectTimer;
    QWebSocketServer* m_wsServer;
    QList<QWebSocket*> m_wsClients;
    
    QSet<QString> m_finishedBibs;
    QByteArray m_notifBuffer;
    
    QJsonObject m_cacheCompetition;
    QMap<QString, QJsonObject> m_cacheRace;
    QMap<QString, QJsonObject> m_cacheEpreuve;

    void loadFromMySQL();
    QJsonObject buildCompetitionLoad(const QString& codex);
    QJsonObject buildRaceLoad(const QString& keyRace);
    QJsonObject buildEpreuveLoad(const QString& keyRace, const QString& epreuve, QSet<QString>& newlyFinished);
    
    void processNotif(QJsonObject obj);
    void broadcastWS(const QJsonObject& obj);
    
    QString toMs(const QVariant& val, const QString& def = "-1");
    QJsonObject sqlTableToAdv(class QSqlQuery& q, const QString& tableName);
    QJsonArray makeCol(const QString& name, int type = 1);
};

#endif // LIVEBRIDGE_H
