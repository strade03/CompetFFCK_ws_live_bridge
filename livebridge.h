#ifndef LIVEBRIDGE_H
#define LIVEBRIDGE_H

#include <QObject>
#include <QSqlDatabase>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QMap>

// Types advTable (cf. adv.js adv.index_type)
enum AdvType {
    ADV_NULL    = 0,
    ADV_CHAR    = 1,
    ADV_VARCHAR = 2,
    ADV_TEXT    = 3,
    ADV_SHORT   = 4,
    ADV_LONG    = 5,
    ADV_DOUBLE  = 6,
    ADV_CHRONO  = 7,
    ADV_CHRONO_INV = 8,
    ADV_RANKING = 9,
    ADV_DATE    = 10,
};

class LiveBridge : public QObject
{
    Q_OBJECT
public:
    explicit LiveBridge(QObject *parent = nullptr);
    ~LiveBridge();

    struct Config {
        QString dbHost; int dbPort; QString dbUser; QString dbPass; QString dbName;
        QString notifHost; int notifPort; int wsPort; QString keyRace;
        int codeCourse = 0; int codePhase = 0;
    };

    bool start(const Config& cfg);
    void stop();
    QStringList fetchCodex(const Config& cfg, const QString& filterActivite = "");

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

    // Track key_race per WS client (like Python ws_clients[ws]["key_race"])
    QMap<QWebSocket*, QString> m_wsClientKeyRace;
    
    QSet<QString> m_finishedBibs;
    QByteArray m_notifBuffer;
    
    QString m_cacheCompetitionJson;
    QMap<QString, QString> m_cacheRaceJson;
    // No epreuve cache — rebuilt each time from DB like Python

    void loadFromMySQL();
    QJsonObject buildCompetitionLoad(const QString& codex);
    QJsonObject buildRaceLoad(const QString& keyRace);
    QJsonObject buildEpreuveLoad(const QString& keyRace, const QString& epreuve, QSet<QString>& newlyFinished);
    
    void processNotif(QJsonObject obj);
    void broadcastWS(const QJsonObject& obj);
    
    // Helpers
    static QString toMs(const QVariant& val, const QString& def = "-1");
    static QJsonArray makeCol(const QString& name, int type = ADV_CHAR, const QString& fmt = "");
    static int mysqlTypeToAdv(int mysqlMetaType);
    QJsonObject sqlTableToAdv(class QSqlQuery& q, const QString& tableName);
};

#endif // LIVEBRIDGE_H