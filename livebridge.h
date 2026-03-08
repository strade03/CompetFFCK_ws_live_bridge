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

// ═══════════════════════════════════════════════════════════════════════════
//  Types de colonnes advTable (cf. adv.js → adv.index_type)
//  Le client JS utilise columns[c][5] pour déterminer le type d'affichage :
//    - CHRONO → formatage HH:MM:SS.CC
//    - RANKING → affichage classement (0 = pas classé)
//    - LONG → entier
//    - etc.
// ═══════════════════════════════════════════════════════════════════════════
enum AdvType {
    ADV_NULL       = 0,
    ADV_CHAR       = 1,
    ADV_VARCHAR    = 2,
    ADV_TEXT       = 3,
    ADV_SHORT      = 4,
    ADV_LONG       = 5,
    ADV_DOUBLE     = 6,
    ADV_CHRONO     = 7,   // Temps chrono en millisecondes
    ADV_CHRONO_INV = 8,
    ADV_RANKING    = 9,   // Classement (0 = non classé)
    ADV_DATE       = 10,
};

/**
 * LiveBridge — Pont entre CompetFFCK (notifications TCP) et les pages web (WebSocket)
 * 
 * Architecture :
 *   CompetFFCK (TCP:9000) → [LiveBridge] → WebSocket (8080) → live.html / monitor_race.html
 *   MySQL (base_ffck)     ↗
 * 
 * Le bridge :
 *   1) Charge la structure de la compétition depuis MySQL au démarrage
 *   2) Se connecte au serveur de notifications TCP de CompetFFCK
 *   3) Sert un WebSocket pour les clients web (live.html, monitor_race.html)
 *   4) Retransmet les notifications temps réel (on_course, bib_time, penalty_add)
 */
class LiveBridge : public QObject
{
    Q_OBJECT

public:
    explicit LiveBridge(QObject *parent = nullptr);
    ~LiveBridge();

    /// Configuration de connexion
    struct Config {
        QString dbHost;      int dbPort;
        QString dbUser;      QString dbPass;
        QString dbName;
        QString notifHost;   int notifPort;
        int wsPort;
        QString keyRace;     // Codex de la compétition (ex: "FFCK20250553")
        int codeCourse = 0;  // 0 = toutes les courses
        int codePhase = 0;   // 0 = toutes les phases
    };

    bool start(const Config& cfg);
    void stop();

    /// Récupère la liste des compétitions disponibles dans la BDD
    /// @param filterActivite Filtre par type d'activité ("SLA", "DES", "Tout"...)
    QStringList fetchCodex(const Config& cfg, const QString& filterActivite = "");

signals:
    void logMessage(const QString& msg);

private slots:
    // Notification TCP (connexion au chronomètre CompetFFCK)
    void onNotifConnected();
    void onNotifReadyRead();
    void onNotifDisconnected();
    void tryReconnectNotif();

    // WebSocket (clients web)
    void onWsNewConnection();
    void onWsTextMessage(const QString &message);
    void onWsDisconnected();

private:
    Config          m_cfg;
    QSqlDatabase    m_db;
    QTcpSocket*     m_tcpSocket;
    QTimer*         m_reconnectTimer;
    QWebSocketServer* m_wsServer;

    /// Suivi du key_race par client WebSocket
    /// Le JS n'envoie pas key_race dans <epreuve_load>, on le track depuis <race_load>
    QMap<QWebSocket*, QString> m_wsClientKeyRace;

    /// Dossards des bateaux arrivés (pour filtrer on_course)
    QSet<QString> m_finishedBibs;

    /// Buffer de réception TCP (paquets séparés par \0)
    QByteArray m_notifBuffer;

    /// Cache JSON pré-sérialisé pour les réponses rapides
    QString m_cacheCompetitionJson;              // <competition_load>
    QMap<QString, QString> m_cacheRaceJson;      // <race_load> par key_race

    // Construction des réponses depuis MySQL
    void loadFromMySQL();
    QJsonObject buildCompetitionLoad(const QString& codex);
    QJsonObject buildRaceLoad(const QString& keyRace);

    /// Résultat de buildEpreuveLoad : données JSON + liste des dossards arrivés
    struct EpreuveResult {
        QJsonObject data;
        QSet<QString> finishedBibs;
    };
    EpreuveResult buildEpreuveLoad(const QString& keyRace, const QString& epreuve);

    // Traitement des notifications et broadcast
    void processNotif(QJsonObject obj);
    void broadcastWS(const QJsonObject& obj);

    // Helpers statiques
    static QString toMs(const QVariant& val, const QString& def = "-1");
    static QJsonArray makeCol(const QString& name, int type = ADV_CHAR, const QString& fmt = "");
    static int mysqlTypeToAdv(int metaType);
    QJsonObject sqlTableToAdv(QSqlQuery& q, const QString& tableName);
};

#endif // LIVEBRIDGE_H