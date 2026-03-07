#include "livebridge.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <utility>

LiveBridge::LiveBridge(QObject *parent) : QObject(parent) {
    m_tcpSocket = new QTcpSocket(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(5000);
    
    m_wsServer = new QWebSocketServer("WS-Live", QWebSocketServer::NonSecureMode, this);

    connect(m_tcpSocket, &QTcpSocket::connected, this, &LiveBridge::onNotifConnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead, this, &LiveBridge::onNotifReadyRead);
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, &LiveBridge::onNotifDisconnected);
    connect(m_reconnectTimer, &QTimer::timeout, this, &LiveBridge::tryReconnectNotif);

    connect(m_wsServer, &QWebSocketServer::newConnection, this, &LiveBridge::onWsNewConnection);
}

LiveBridge::~LiveBridge() { stop(); }

QStringList LiveBridge::fetchCodex(const Config& cfg) {
    QStringList list;
    QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", "fetch_conn");
    db.setHostName(cfg.dbHost); db.setPort(cfg.dbPort);
    db.setUserName(cfg.dbUser); db.setPassword(cfg.dbPass); db.setDatabaseName(cfg.dbName);

    if (db.open()) {
        QSqlQuery q("SELECT Codex FROM Competition ORDER BY Date_debut DESC, Code DESC LIMIT 50", db);
        while(q.next()) list << q.value(0).toString();
        db.close();
    } else {
        emit logMessage("[ERREUR BDD] " + db.lastError().text());
    }
    QSqlDatabase::removeDatabase("fetch_conn");
    return list;
}

bool LiveBridge::start(const Config& cfg) {
    m_cfg = cfg;
    m_db = QSqlDatabase::addDatabase("QMYSQL", "live_conn");
    m_db.setHostName(m_cfg.dbHost); m_db.setPort(m_cfg.dbPort);
    m_db.setUserName(m_cfg.dbUser); m_db.setPassword(m_cfg.dbPass); m_db.setDatabaseName(m_cfg.dbName);

    if (!m_db.open()) {
        emit logMessage("[MYSQL ERREUR] " + m_db.lastError().text());
        return false;
    }

    loadFromMySQL();

    if (!m_wsServer->listen(QHostAddress::Any, m_cfg.wsPort)) {
        emit logMessage("[WS ERREUR] Impossible d'écouter sur le port " + QString::number(m_cfg.wsPort));
        return false;
    }
    emit logMessage("[WS] Serveur démarré sur le port " + QString::number(m_cfg.wsPort));

    tryReconnectNotif();
    return true;
}

void LiveBridge::stop() {
    m_reconnectTimer->stop();
    m_tcpSocket->abort();
    m_wsServer->close();
    qDeleteAll(m_wsClients);
    m_wsClients.clear();
    if (m_db.isOpen()) m_db.close();
    QSqlDatabase::removeDatabase("live_conn");
    emit logMessage("[STOP] Serveur arrêté.");
}

void LiveBridge::loadFromMySQL() {
    emit logMessage("[MySQL] Chargement structure pour " + m_cfg.keyRace + "...");
    m_finishedBibs.clear();
    m_cacheCompetition = buildCompetitionLoad(m_cfg.keyRace);
    
    // Simplification pour l'exemple C++ : On laisse la méthode buildEpreuveLoad gérer 
    // l'extraction en temps réel quand le JS la demandera, comme dans ta version finale Python.
    emit logMessage("[MySQL] ✓ Structure chargée.");
}

void LiveBridge::tryReconnectNotif() {
    if (m_tcpSocket->state() == QAbstractSocket::UnconnectedState) {
        emit logMessage(QString("[NOTIF] Connexion à %1:%2...").arg(m_cfg.notifHost).arg(m_cfg.notifPort));
        m_tcpSocket->connectToHost(m_cfg.notifHost, m_cfg.notifPort);
    }
}

void LiveBridge::onNotifConnected() {
    emit logMessage("[NOTIF] ✓ Connecté au chronomètre !");
    m_reconnectTimer->stop();
}

void LiveBridge::onNotifDisconnected() {
    emit logMessage("[NOTIF] Déconnecté. Reconnexion...");
    m_reconnectTimer->start();
}

void LiveBridge::onNotifReadyRead() {
    m_notifBuffer.append(m_tcpSocket->readAll());
    int sepIdx;
    while ((sepIdx = m_notifBuffer.indexOf('\x00')) != -1) {
        QByteArray packet = m_notifBuffer.left(sepIdx);
        m_notifBuffer.remove(0, sepIdx + 1);
        if (!packet.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(packet);
            if (doc.isObject()) processNotif(doc.object());
            else if (doc.isArray()) {
                for (QJsonValue val : doc.array()) processNotif(val.toObject());
            }
        }
    }
}

void LiveBridge::processNotif(QJsonObject obj) {
    QString key = obj["key"].toString();
    
    if (key == "<bib_time>") {
        if (obj["passage"].toInt(-1) <= 0 && obj.contains("time_chrono")) {
            m_finishedBibs.insert(obj["bib"].toVariant().toString());
        }
    } else if (key == "<run_erase>") {
        m_finishedBibs.clear();
    } else if (key == "<on_course>") {
        if (obj.contains("table<sqlTable>")) {
            QJsonObject table = obj["table<sqlTable>"].toObject();
            QJsonArray rows = table["rows"].toArray();
            QJsonArray cols = table["columns"].toArray();
            int bibIdx = -1;
            for(int i=0; i<cols.size(); i++) {
                if(cols[i].toArray()[0].toString() == "bib") { bibIdx = i; break; }
            }
            if(bibIdx >= 0) {
                QJsonArray newRows;
                for(auto r : rows) {
                    if(!m_finishedBibs.contains(r.toArray()[bibIdx].toString())) {
                        newRows.append(r);
                    }
                }
                table["rows"] = newRows;
                obj["table<sqlTable>"] = table;
            }
        }
    }

    if (key != "<on_course>") {
        emit logMessage("[NOTIF] " + key + " bib=" + obj["bib"].toVariant().toString());
    }
    broadcastWS(obj);
}

void LiveBridge::onWsNewConnection() {
    QWebSocket *pSocket = m_wsServer->nextPendingConnection();
    connect(pSocket, &QWebSocket::textMessageReceived, this, &LiveBridge::onWsTextMessage);
    connect(pSocket, &QWebSocket::disconnected, this, &LiveBridge::onWsDisconnected);
    m_wsClients << pSocket;
}

void LiveBridge::onWsTextMessage(const QString &message) {
    QWebSocket *pClient = qobject_cast<QWebSocket *>(sender());
    if (!pClient) return;

    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject req = doc.object();
    QString key = req["key"].toString();

    if (key == "<competition_load>") {
        pClient->sendTextMessage(QJsonDocument(m_cacheCompetition).toJson(QJsonDocument::Compact));
    } 
    else if (key == "<race_load>") {
        QString kr = req["key_race"].toString();
        // Caching basique C++
        if(!m_cacheRace.contains(kr)) m_cacheRace[kr] = buildRaceLoad(kr);
        pClient->sendTextMessage(QJsonDocument(m_cacheRace[kr]).toJson(QJsonDocument::Compact));
    }
    else if (key == "<epreuve_load>") {
        QString kr = req["key_race"].toString(); // Géré via state JS
        QString ep = req["epreuve"].toString();
        
        QSet<QString> newlyFinished;
        QJsonObject epData = buildEpreuveLoad(kr, ep, newlyFinished);
        m_finishedBibs.unite(newlyFinished);
        pClient->sendTextMessage(QJsonDocument(epData).toJson(QJsonDocument::Compact));
    }
    else if (key == "<ping>") {
        QJsonObject pong; pong["key"] = "<pong>"; pong["online"] = (m_tcpSocket->state() == QAbstractSocket::ConnectedState);
        pClient->sendTextMessage(QJsonDocument(pong).toJson(QJsonDocument::Compact));
    }
}

void LiveBridge::onWsDisconnected() {
    QWebSocket *pClient = qobject_cast<QWebSocket *>(sender());
    if (pClient) { m_wsClients.removeAll(pClient); pClient->deleteLater(); }
}

void LiveBridge::broadcastWS(const QJsonObject& obj) {
    QString msg = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    for (QWebSocket *pClient : std::as_const(m_wsClients)) pClient->sendTextMessage(msg);
}

// -------------------------------------------------------------------------
// REQUÊTES SQL & LOGIQUE FFCK (Simplifiée pour la structure Qt)
// -------------------------------------------------------------------------

QString LiveBridge::toMs(const QVariant& val, const QString& def) {
    if (val.isNull() || !val.isValid()) return def;
    QString s = val.toString().trimmed();
    if (s.isEmpty() || s == "0" || s.toLower() == "none") return def;
    if (val.typeId() == QMetaType::QTime || val.typeId() == QMetaType::QDateTime) {
        return QString::number(val.toTime().msecsSinceStartOfDay());
    }
    if (s.contains(':')) {
        QStringList parts = s.split(':');
        if (parts.size() >= 2) {
            int h = parts.size() == 3 ? parts[0].toInt() : 0;
            int m = parts[parts.size()-2].toInt();
            QStringList secParts = parts.last().split('.');
            int sec = secParts[0].toInt();
            int ms = secParts.size() > 1 ? secParts[1].leftJustified(3, '0').toInt() : 0;
            return QString::number((h * 3600 + m * 60 + sec) * 1000 + ms);
        }
    }
    return s;
}

QJsonArray LiveBridge::makeCol(const QString& name, int type) {
    QJsonArray col; col << name << name << "" << "" << "" << QString::number(type); return col;
}

QJsonObject LiveBridge::sqlTableToAdv(QSqlQuery& q, const QString& tableName) {
    QJsonObject res; QJsonObject table;
    QJsonArray cols; QSqlRecord rec = q.record();
    for(int i=0; i<rec.count(); i++) cols.append(makeCol(rec.fieldName(i), 1));
    table["columns"] = cols;
    QJsonArray rows;
    while(q.next()) {
        QJsonArray row;
        for(int i=0; i<rec.count(); i++) row.append(q.value(i).toString());
        rows.append(row);
    }
    table["rows"] = rows;
    res[tableName + "<sqlTable>"] = table;
    return res;
}

QJsonObject LiveBridge::buildCompetitionLoad(const QString& codex) {
    // Structure C++ basique pour <competition_load> (identique à Python)
    QJsonObject res; res["key"] = "<competition_load>";
    QSqlQuery q(m_db); q.prepare("SELECT Code FROM Competition WHERE Codex = ?"); q.addBindValue(codex);
    if(q.exec() && q.next()) {
        QString codeComp = q.value(0).toString();
        // Extraction Competition_Course / Phase simplifié
        q.exec("SELECT * FROM Competition_Course_Phase WHERE Code_competition = " + codeComp);
        QJsonObject table;
        QJsonArray cols; cols << makeCol("key") << makeCol("active") << makeCol("Codex") << makeCol("Libelle_phase");
        table["columns"] = cols;
        QJsonArray rows;
        while(q.next()) {
            QJsonArray row;
            row << codex + "_" + q.value("Code_course").toString() + "_" + q.value("Code_phase").toString()
                << "1" << codex << q.value("Libelle").toString();
            rows.append(row);
        }
        table["rows"] = rows;
        res["competitions<sqlTable>"] = table;
    }
    return res;
}

QJsonObject LiveBridge::buildRaceLoad(const QString& kr) {
    QJsonObject res; res["key"] = "<race_load>";
    QStringList p = kr.split("_"); if(p.size() != 3) return res;
    QSqlQuery q(m_db); q.prepare("SELECT Code FROM Competition WHERE Codex = ?"); q.addBindValue(p[0]);
    if(q.exec() && q.next()) {
        QString codeComp = q.value(0).toString();
        QJsonObject tables;

        // Fonction lambda pour fusionner les tables JSON
        auto merge = [&](const QJsonObject& src) {
            for(auto it = src.begin(); it != src.end(); ++it) {
                tables.insert(it.key(), it.value());
            }
        };

        q.exec("SELECT * FROM Competition WHERE Code = " + codeComp);
        merge(sqlTableToAdv(q, "Competition"));

        q.exec("SELECT * FROM Competition_Course WHERE Code_competition = " + codeComp);
        merge(sqlTableToAdv(q, "Competition_Course"));

        q.exec(QString("SELECT * FROM Competition_Course_Phase WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3").arg(codeComp, p[1], p[2]));
        merge(sqlTableToAdv(q, "Competition_Course_Phase"));

        if(q.first()) { res["Nb_inter"] = q.value("Nb_inter").toInt(); res["Nb_porte"] = q.value("Nb_porte").toInt(); }

        q.exec(QString("SELECT * FROM Competition_Course_Phase_Manche_Epreuve WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3 ORDER BY Ordre").arg(codeComp, p[1], p[2]));
        merge(sqlTableToAdv(q, "Competition_Course_Phase_Manche_Epreuve"));

        res["tables"] = tables;
    }
    return res;
}

QJsonObject LiveBridge::buildEpreuveLoad(const QString& kr, const QString& epreuve, QSet<QString>& newlyFinished) {
    QJsonObject res; res["key"] = "<epreuve_load>"; res["epreuve"] = epreuve;
    // Note: Pour garder le code lisible ici, la logique est la même qu'en Python mais
    // adaptée via les appels QSqlQuery. 
    QStringList p = kr.split("_"); if(p.size() != 3) return res;
    QSqlQuery q(m_db); q.prepare("SELECT Code FROM Competition WHERE Codex = ?"); q.addBindValue(p[0]);
    if(!q.exec() || !q.next()) return res;
    QString codeComp = q.value(0).toString();

    // 1. Charger Dossards (Identité)
    QString sql = "SELECT * FROM Resultat WHERE Code_competition = " + codeComp;
    if(!epreuve.isEmpty()) sql += " AND Code_categorie = '" + epreuve + "'";
    sql += " ORDER BY Code_categorie, Dossard";
    q.exec(sql);
    QJsonObject tableNode; tableNode["columns"] = QJsonArray(); tableNode["rows"] = QJsonArray();
    if(!q.isActive()) return res;
    
    QSqlRecord rec = q.record();
    QJsonArray cols; for(int i=0; i<rec.count(); i++) cols.append(makeCol(rec.fieldName(i)));
    
    // Ajout manuel des colonnes requises par le JS
    QString cp_str = p[1] + "_" + p[2];
    cols.append(makeCol("Tps_chrono" + cp_str, 7)); cols.append(makeCol("Cltc_chrono" + cp_str, 9));
    cols.append(makeCol("Heure_depart_reelle", 5)); cols.append(makeCol("Heure_arrivee_reelle", 5));
    cols.append(makeCol("Tps" + cp_str, 7)); cols.append(makeCol("Cltc" + cp_str, 9));
    tableNode["columns"] = cols;

    // 2. Map des temps courses
    QMap<QString, QSqlRecord> rcMap;
    QSqlQuery qRc("SELECT * FROM Resultat_Course WHERE Code_competition = " + codeComp, m_db);
    while(qRc.next()) rcMap[qRc.value("Code_bateau").toString()] = qRc.record();

    QJsonArray rows;
    while(q.next()) {
        QJsonArray row;
        for(int i=0; i<rec.count(); i++) row.append(q.value(i).toString());
        
        QString bib = q.value("Dossard").toString();
        QString cb = q.value("Code_bateau").toString();
        
        QString tps = "-1", cltc = "0", ha = "0";
        if(rcMap.contains(cb)) {
            QSqlRecord rc = rcMap[cb];
            tps = toMs(rc.value("Tps"), "-1");
            cltc = rc.value("Cltc").toString();
            ha = toMs(rc.value("Heure_arrivee_reel"), "0");
        }
        
        if (tps != "-1") newlyFinished.insert(bib);
        
        row.append(tps); row.append(cltc);
        row.append("0"); row.append(ha);
        row.append(tps); row.append(cltc);
        
        rows.append(row);
    }
    tableNode["rows"] = rows;
    res["ranking<sqlTable>"] = tableNode;
    
    return res;
}
