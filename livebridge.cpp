#include "livebridge.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlField>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <QMetaType>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Helpers statiques
// ═══════════════════════════════════════════════════════════════════════════

QString LiveBridge::toMs(const QVariant& val, const QString& def) {
    if (val.isNull() || !val.isValid()) return def;
    QString s = val.toString().trimmed();
    if (s.isEmpty() || s == "0" || s.toLower() == "none") return def;

    // QTime / QDateTime
    if (val.typeId() == QMetaType::QTime || val.typeId() == QMetaType::QDateTime) {
        return QString::number(val.toTime().msecsSinceStartOfDay());
    }
    // timedelta-like "HH:MM:SS.fff"
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
    // Already a number
    bool ok;
    double d = s.toDouble(&ok);
    if (ok) return QString::number(qint64(d));
    return def;
}

QJsonArray LiveBridge::makeCol(const QString& name, int type, const QString& fmt) {
    // Format: [name, label, format, widthDefault, style, type]
    QJsonArray col;
    col << name << name << fmt << "" << "" << QString::number(type);
    return col;
}

int LiveBridge::mysqlTypeToAdv(int metaType) {
    switch (metaType) {
        case QMetaType::Short: case QMetaType::UShort: case QMetaType::Char:
        case QMetaType::SChar: case QMetaType::UChar: return ADV_SHORT;
        case QMetaType::Int: case QMetaType::UInt: case QMetaType::Long:
        case QMetaType::ULong: case QMetaType::LongLong: case QMetaType::ULongLong:
            return ADV_LONG;
        case QMetaType::Float: case QMetaType::Double: return ADV_DOUBLE;
        case QMetaType::QDate: case QMetaType::QDateTime: return ADV_DATE;
        default: return ADV_CHAR;
    }
}

QJsonObject LiveBridge::sqlTableToAdv(QSqlQuery& q, const QString& tableName) {
    QJsonObject res;
    QJsonObject table;
    QJsonArray cols;
    QSqlRecord rec = q.record();
    for (int i = 0; i < rec.count(); i++) {
        int advType = mysqlTypeToAdv(rec.field(i).metaType().id());
        cols.append(makeCol(rec.fieldName(i), advType));
    }
    table["columns"] = cols;
    QJsonArray rows;
    while (q.next()) {
        QJsonArray row;
        for (int i = 0; i < rec.count(); i++) {
            QVariant v = q.value(i);
            row.append(v.isNull() ? "" : v.toString());
        }
        rows.append(row);
    }
    table["rows"] = rows;
    res[tableName + "<sqlTable>"] = table;
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Codex list & start/stop
// ═══════════════════════════════════════════════════════════════════════════

QStringList LiveBridge::fetchCodex(const Config& cfg) {
    QStringList list;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", "fetch_conn");
        db.setHostName(cfg.dbHost); db.setPort(cfg.dbPort);
        db.setUserName(cfg.dbUser); db.setPassword(cfg.dbPass); db.setDatabaseName(cfg.dbName);

        if (db.open()) {
            QSqlQuery q(db);
            q.exec(
                "SELECT c.Codex, c.Code, c.Nom, c.Code_activite, c.Date_debut, "
                "       GROUP_CONCAT(DISTINCT cc.Code_course ORDER BY cc.Code_course SEPARATOR ',') AS courses "
                "FROM Competition c "
                "LEFT JOIN Competition_Course cc ON cc.Code_competition = c.Code "
                "WHERE c.Codex IS NOT NULL AND c.Codex != '' "
                "GROUP BY c.Codex, c.Code, c.Nom, c.Code_activite, c.Date_debut "
                "ORDER BY c.Date_debut DESC, c.Code DESC "
                "LIMIT 50"
            );
            while (q.next()) {
                QString codex = q.value("Codex").toString().trimmed();
                QString nom = q.value("Nom").toString().trimmed();
                QString activite = q.value("Code_activite").toString().trimmed();
                QString date = q.value("Date_debut").toString();
                QString courses = q.value("courses").toString();
                if (!codex.isEmpty()) {
                    // Format: "CODEX | Activité | Course(s) N | Nom"
                    QString display = codex;
                    if (!activite.isEmpty()) display += " | " + activite;
                    if (!courses.isEmpty()) display += " | C:" + courses;
                    if (!nom.isEmpty()) {
                        // Tronquer le nom si trop long
                        if (nom.length() > 40) nom = nom.left(40) + "…";
                        display += " | " + nom;
                    }
                    list << display;
                }
            }
            db.close();
        } else {
            emit logMessage("[ERREUR BDD] " + db.lastError().text());
        }
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
    emit logMessage("[MySQL] Connecté à " + m_cfg.dbHost + "/" + m_cfg.dbName);

    loadFromMySQL();

    if (!m_wsServer->listen(QHostAddress::Any, m_cfg.wsPort)) {
        emit logMessage("[WS ERREUR] Port " + QString::number(m_cfg.wsPort));
        return false;
    }
    emit logMessage("[WS] Serveur ws://0.0.0.0:" + QString::number(m_cfg.wsPort));

    tryReconnectNotif();
    return true;
}

void LiveBridge::stop() {
    m_reconnectTimer->stop();
    m_tcpSocket->abort();
    m_wsServer->close();
    for (auto* ws : m_wsClientKeyRace.keys()) {
        ws->close();
        ws->deleteLater();
    }
    m_wsClientKeyRace.clear();
    if (m_db.isOpen()) m_db.close();
    QSqlDatabase::removeDatabase("live_conn");
    emit logMessage("[STOP] Serveur arrêté.");
}

// ═══════════════════════════════════════════════════════════════════════════
//  MySQL Loading
// ═══════════════════════════════════════════════════════════════════════════

void LiveBridge::loadFromMySQL() {
    emit logMessage("[MySQL] Chargement pour " + m_cfg.keyRace + "...");
    m_finishedBibs.clear();
    m_cacheCompetitionJson.clear();
    m_cacheRaceJson.clear();

    QJsonObject comp = buildCompetitionLoad(m_cfg.keyRace);
    if (comp.isEmpty()) {
        emit logMessage("[MySQL] ✗ Échec chargement");
        return;
    }
    m_cacheCompetitionJson = QJsonDocument(comp).toJson(QJsonDocument::Compact);

    // Pre-cache race_load for each course/phase
    QJsonObject tComp = comp["competitions<sqlTable>"].toObject();
    QJsonArray compRows = tComp["rows"].toArray();
    for (const QJsonValue& rv : compRows) {
        QJsonArray row = rv.toArray();
        QString kr = row[0].toString();
        emit logMessage("  → course: " + kr);

        QJsonObject race = buildRaceLoad(kr);
        if (!race.isEmpty()) {
            m_cacheRaceJson[kr] = QJsonDocument(race).toJson(QJsonDocument::Compact);
            emit logMessage("  ✓ race_load [" + kr + "]");
        }
    }
    emit logMessage("[MySQL] ✓ Chargement terminé.");
}

// ═══════════════════════════════════════════════════════════════════════════
//  buildCompetitionLoad — EXACT match to Python
// ═══════════════════════════════════════════════════════════════════════════

QJsonObject LiveBridge::buildCompetitionLoad(const QString& codex) {
    QJsonObject result;
    result["key"] = "<competition_load>";

    QSqlQuery q(m_db);
    q.prepare("SELECT Code FROM Competition WHERE Codex = ?");
    q.addBindValue(codex);
    if (!q.exec() || !q.next()) {
        emit logMessage("[MySQL] Compétition '" + codex + "' non trouvée !");
        return {};
    }
    int codeComp = q.value(0).toInt();
    emit logMessage(QString("[MySQL] Compétition trouvée: Codex=%1 → Code=%2").arg(codex).arg(codeComp));

    // Load Competition info
    q.exec(QString("SELECT * FROM Competition WHERE Code = %1").arg(codeComp));
    QMap<QString, QVariant> comp;
    if (q.next()) {
        QSqlRecord rec = q.record();
        for (int i = 0; i < rec.count(); i++)
            comp[rec.fieldName(i)] = q.value(i);
    }

    // Load Competition_Course
    q.exec(QString("SELECT * FROM Competition_Course WHERE Code_competition = %1").arg(codeComp));
    QList<QMap<QString, QVariant>> ccRows;
    while (q.next()) {
        QMap<QString, QVariant> row;
        QSqlRecord rec = q.record();
        for (int i = 0; i < rec.count(); i++) row[rec.fieldName(i)] = q.value(i);
        ccRows << row;
    }

    // Load Competition_Course_Phase
    q.exec(QString("SELECT * FROM Competition_Course_Phase WHERE Code_competition = %1").arg(codeComp));
    QList<QMap<QString, QVariant>> cpRows;
    while (q.next()) {
        QMap<QString, QVariant> row;
        QSqlRecord rec = q.record();
        for (int i = 0; i < rec.count(); i++) row[rec.fieldName(i)] = q.value(i);
        cpRows << row;
    }

    // Build competition table — ALL columns like Python
    QJsonArray columns;
    columns << QJsonArray({"key","key"}) << QJsonArray({"active","active"})
            << QJsonArray({"Code_competition","Code_competition"})
            << QJsonArray({"Code_course","Code_course"}) << QJsonArray({"Code_phase","Code_phase"})
            << QJsonArray({"Codex","Codex"}) << QJsonArray({"Code_activite","Code_activite"})
            << QJsonArray({"Nom","Nom"}) << QJsonArray({"Ville","Ville"})
            << QJsonArray({"Riviere","Riviere"}) << QJsonArray({"Organisateur","Organisateur"})
            << QJsonArray({"Date_debut","Date_debut"}) << QJsonArray({"Date_fin","Date_fin"})
            << QJsonArray({"Libelle_course","Libelle_course"}) << QJsonArray({"Date_course","Date_course"})
            << QJsonArray({"Libelle_phase","Libelle_phase"}) << QJsonArray({"Date_phase","Date_phase"});

    QJsonArray rows;
    for (const auto& cc : ccRows) {
        for (const auto& cp : cpRows) {
            if (cp["Code_course"].toInt() != cc["Code_course"].toInt()) continue;
            int ccVal = cc["Code_course"].toInt();
            int cpVal = cp["Code_phase"].toInt();
            QString kr = QString("%1_%2_%3").arg(codex).arg(ccVal).arg(cpVal);
            int active = (m_cfg.codeCourse == 0 ||
                         (ccVal == m_cfg.codeCourse && cpVal == m_cfg.codePhase)) ? 1 : 0;

            auto vs = [](const QVariant& v) -> QString {
                return v.isNull() ? "" : v.toString();
            };

            QJsonArray row;
            row << kr << active << codeComp << ccVal << cpVal
                << vs(comp["Codex"]) << vs(comp["Code_activite"])
                << vs(comp["Nom"]) << vs(comp["Ville"])
                << vs(comp["Riviere"]) << vs(comp["Organisateur"])
                << vs(comp["Date_debut"]) << vs(comp["Date_fin"])
                << vs(cc["Libelle"]) << vs(cc["Date_course"])
                << vs(cp["Libelle"]) << vs(cp["Date_phase"]);
            rows.append(row);
        }
    }

    QJsonObject table;
    table["columns"] = columns;
    table["rows"] = rows;
    result["competitions<sqlTable>"] = table;

    emit logMessage(QString("[MySQL] competition_load: %1 entrée(s)").arg(rows.size()));
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  buildRaceLoad — EXACT match to Python
// ═══════════════════════════════════════════════════════════════════════════

QJsonObject LiveBridge::buildRaceLoad(const QString& kr) {
    QJsonObject result;
    result["key"] = "<race_load>";
    QStringList p = kr.split("_");
    if (p.size() < 3) return result;
    QString codex = p[0];
    int cc = p[1].toInt(), cp = p[2].toInt();
    result["Code_course"] = cc;
    result["Code_phase"] = cp;

    QSqlQuery q(m_db);
    q.prepare("SELECT Code FROM Competition WHERE Codex = ?");
    q.addBindValue(codex);
    if (!q.exec() || !q.next()) return result;
    int codeComp = q.value(0).toInt();
    result["Code_competition"] = codeComp;

    // Tables dans le sous-objet "tables" (le JS fait adv.GetTable qui cherche dans obj.tables)
    QJsonObject tables;
    auto merge = [&](const QJsonObject& src) {
        for (auto it = src.begin(); it != src.end(); ++it)
            tables.insert(it.key(), it.value());
    };

    q.exec(QString("SELECT * FROM Competition WHERE Code = %1").arg(codeComp));
    merge(sqlTableToAdv(q, "Competition"));

    q.exec(QString("SELECT * FROM Competition_Course WHERE Code_competition = %1").arg(codeComp));
    merge(sqlTableToAdv(q, "Competition_Course"));

    // Competition_Course_Phase — ALL phases (not filtered by cc/cp)
    q.exec(QString("SELECT * FROM Competition_Course_Phase WHERE Code_competition = %1").arg(codeComp));
    merge(sqlTableToAdv(q, "Competition_Course_Phase"));

    // Competition_Course_Phase_Manche_Epreuve — filtered AND ordered
    q.exec(QString("SELECT * FROM Competition_Course_Phase_Manche_Epreuve "
                   "WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3 "
                   "ORDER BY Ordre").arg(codeComp).arg(cc).arg(cp));
    merge(sqlTableToAdv(q, "Competition_Course_Phase_Manche_Epreuve"));

    result["tables"] = tables;

    // Nb_inter, Nb_porte — separate query
    q.exec(QString("SELECT Nb_inter, Nb_porte FROM Competition_Course_Phase "
                   "WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3")
           .arg(codeComp).arg(cc).arg(cp));
    if (q.next()) {
        result["Nb_inter"] = q.value(0).isNull() ? 0 : q.value(0).toInt();
        result["Nb_porte"] = q.value(1).isNull() ? 0 : q.value(1).toInt();
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  buildEpreuveLoad — EXACT match to Python (the big one!)
// ═══════════════════════════════════════════════════════════════════════════

QJsonObject LiveBridge::buildEpreuveLoad(const QString& kr, const QString& epreuve, QSet<QString>& newlyFinished) {
    QJsonObject result;
    result["key"] = "<epreuve_load>";
    result["epreuve"] = epreuve;

    QStringList p = kr.split("_");
    if (p.size() < 3) return result;
    QString codex = p[0];
    int cc = p[1].toInt(), cp = p[2].toInt();

    QSqlQuery q(m_db);
    q.prepare("SELECT Code FROM Competition WHERE Codex = ?");
    q.addBindValue(codex);
    if (!q.exec() || !q.next()) return result;
    int codeComp = q.value(0).toInt();

    // Nb_inter, Nb_porte
    q.exec(QString("SELECT Nb_inter, Nb_porte FROM Competition_Course_Phase "
                   "WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3")
           .arg(codeComp).arg(cc).arg(cp));
    int nbInter = 0, nbPorte = 0;
    if (q.next()) {
        nbInter = q.value(0).isNull() ? 0 : q.value(0).toInt();
        nbPorte = q.value(1).isNull() ? 0 : q.value(1).toInt();
    }
    result["Nb_inter"] = nbInter;
    result["Nb_porte"] = nbPorte;

    // Load Resultat (base identity)
    QString sql = QString("SELECT * FROM Resultat WHERE Code_competition = %1").arg(codeComp);
    if (!epreuve.isEmpty()) sql += QString(" AND Code_categorie = '%1'").arg(epreuve);
    sql += " ORDER BY Code_categorie, Dossard";
    q.exec(sql);

    if (!q.isActive()) { result["ranking<sqlTable>"] = QJsonObject(); return result; }

    QSqlRecord rec = q.record();

    // Base columns with proper MySQL types
    QJsonArray baseCols;
    QStringList baseColNames;
    for (int i = 0; i < rec.count(); i++) {
        int advType = mysqlTypeToAdv(rec.field(i).metaType().id());
        baseCols.append(makeCol(rec.fieldName(i), advType));
        baseColNames << rec.fieldName(i);
    }

    // Collect base rows
    struct BaseRow { QJsonArray data; QString dossard; QString codeBateau; QString codeCat; };
    QList<BaseRow> baseRows;
    while (q.next()) {
        QJsonArray row;
        QString dossard, codeBateau, codeCat;
        for (int i = 0; i < rec.count(); i++) {
            QVariant v = q.value(i);
            QString s = v.isNull() ? "" : v.toString();
            row.append(s);
            if (rec.fieldName(i) == "Dossard") dossard = s.trimmed();
            else if (rec.fieldName(i) == "Code_bateau") codeBateau = s.trimmed();
            else if (rec.fieldName(i) == "Code_categorie") codeCat = s.trimmed();
        }
        baseRows.append({row, dossard, codeBateau, codeCat});
    }

    // Dynamic column names
    int cm = 1;
    QString suffix = QString("_%1_%2_%3").arg(cc).arg(cp).arg(cm);
    QString cpStr = QString("%1_%2").arg(cc).arg(cp);

    // Pénalités
    QJsonArray penaCols;
    for (int p2 = 1; p2 <= nbPorte; p2++)
        penaCols.append(makeCol(QString("@PENA_%1_%2").arg(p2).arg(cm), ADV_SHORT));

    // @-columns (slalom compat)
    QJsonArray atCols;
    atCols.append(makeCol("@CHRONO" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@RK_CHRONO" + suffix, ADV_RANKING));
    atCols.append(makeCol("@RK_CHRONO_SCRATCH" + suffix, ADV_RANKING));
    atCols.append(makeCol("@PENASTRING" + suffix, ADV_CHAR));
    atCols.append(makeCol("@SUMPENA" + suffix, ADV_LONG));
    atCols.append(makeCol("@NBPENA" + suffix, ADV_LONG));
    atCols.append(makeCol("@TIME" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@PTS" + suffix, ADV_DOUBLE));
    atCols.append(makeCol("@RK" + suffix, ADV_RANKING));
    atCols.append(makeCol("@RK_SCRATCH" + suffix, ADV_RANKING));
    atCols.append(makeCol("@DIFF" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@START_RK" + suffix, ADV_RANKING));
    atCols.append(makeCol("@START_TIME" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@ETAT_PENA" + suffix, ADV_RANKING));
    atCols.append(makeCol("@CHRONO_INTER1" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@RK_CHRONO_INTER1" + suffix, ADV_RANKING));
    atCols.append(makeCol("@DIFF_CHRONO_INTER1" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@INTER1" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@RK_INTER1" + suffix, ADV_RANKING));
    atCols.append(makeCol("@DIFF_INTER1" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@CHRONO_INTER2" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@RK_CHRONO_INTER2" + suffix, ADV_RANKING));
    atCols.append(makeCol("@DIFF_CHRONO_INTER2" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@INTER2" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@RK_INTER2" + suffix, ADV_RANKING));
    atCols.append(makeCol("@DIFF_INTER2" + suffix, ADV_CHRONO));
    atCols.append(makeCol("@TIME_" + cpStr, ADV_CHRONO));
    atCols.append(makeCol("@RK_" + cpStr, ADV_RANKING));

    // Final columns (descente mode — JS doesn't rename these)
    QJsonArray finalCols;
    finalCols.append(makeCol("Tps_chrono" + cpStr, ADV_CHRONO));
    finalCols.append(makeCol("Clt_chrono" + cpStr, ADV_RANKING));
    finalCols.append(makeCol("Cltc_chrono" + cpStr, ADV_RANKING));
    finalCols.append(makeCol("Heure_depart" + cpStr, ADV_CHRONO));
    finalCols.append(makeCol("Heure_depart_reelle", ADV_LONG));
    finalCols.append(makeCol("Heure_arrivee_reelle", ADV_LONG));
    finalCols.append(makeCol("Rang" + cpStr, ADV_RANKING));
    finalCols.append(makeCol("Tps" + cpStr, ADV_CHRONO));
    finalCols.append(makeCol("Clt" + cpStr, ADV_RANKING));
    finalCols.append(makeCol("Cltc" + cpStr, ADV_RANKING));
    finalCols.append(makeCol("Total_pena" + cpStr, ADV_LONG));
    finalCols.append(makeCol("Tps_chrono" + cpStr + "_inter1", ADV_CHRONO));
    finalCols.append(makeCol("Cltc_chrono" + cpStr + "_inter1", ADV_RANKING));
    finalCols.append(makeCol("Tps_chrono" + cpStr + "_inter2", ADV_CHRONO));
    finalCols.append(makeCol("Cltc_chrono" + cpStr + "_inter2", ADV_RANKING));

    // Merge all columns
    QJsonArray allCols = baseCols;
    for (const auto& c : penaCols) allCols.append(c);
    for (const auto& c : atCols) allCols.append(c);
    for (const auto& c : finalCols) allCols.append(c);

    // Load Resultat_Chrono (start times by Dossard+Seq)
    QMap<QString, QMap<int,QVariant>> chronoByDossard;
    q.exec(QString("SELECT Dossard, Seq, Heure FROM Resultat_Chrono "
                   "WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3")
           .arg(codeComp).arg(cc).arg(cp));
    while (q.next()) {
        QString d = q.value(0).toString().trimmed();
        int seq = q.value(1).toInt();
        if (!d.isEmpty()) chronoByDossard[d][seq] = q.value(2);
    }

    // Load Resultat_Course (by Code_bateau)
    QMap<QString, QMap<QString,QVariant>> rcByBateau;
    q.exec(QString("SELECT * FROM Resultat_Course WHERE Code_competition = %1 "
                   "AND Code_course = %2 AND Code_phase = %3")
           .arg(codeComp).arg(cc).arg(cp));
    if (q.isActive()) {
        QSqlRecord rcRec = q.record();
        while (q.next()) {
            QMap<QString,QVariant> rd;
            for (int i = 0; i < rcRec.count(); i++)
                rd[rcRec.fieldName(i).toLower()] = q.value(i);
            QString cb = rd["code_bateau"].toString().trimmed();
            if (!cb.isEmpty()) rcByBateau[cb] = rd;
        }
    }

    // Helper
    auto getVal = [](const QMap<QString,QVariant>& m, const QString& key, const QString& def = "") -> QString {
        if (!m.contains(key.toLower())) return def;
        QVariant v = m[key.toLower()];
        if (v.isNull()) return def;
        QString s = v.toString().trimmed();
        if (s.isEmpty() || s.toLower() == "none") return def;
        return s;
    };

    // Build rows
    QJsonArray allRows;
    // We need to track indices for ranking calculation later
    struct RowInfo { int rowIdx; QString cat; qint64 tps; qint64 tpsChrono; };
    QList<RowInfo> rowInfos;

    for (int idx = 0; idx < baseRows.size(); idx++) {
        const auto& br = baseRows[idx];
        QJsonArray row = br.data; // start with base columns

        // Pénalités (empty)
        for (int p2 = 0; p2 < nbPorte; p2++) row.append("");

        auto rc = rcByBateau.value(br.codeBateau);

        QString t_chr = getVal(rc, "tps_chrono", "-1");
        QString t_fin = getVal(rc, "tps", "-1");
        if (t_chr == "0") t_chr = "-1";
        if (t_fin == "0") t_fin = "-1";

        QString tpsChrono = toMs(t_chr, "-1");
        QString tps = toMs(t_fin, "-1");

        if (tpsChrono != "-1" || tps != "-1") newlyFinished.insert(br.dossard);

        QString cltc = getVal(rc, "cltc", "0");
        QString clt = getVal(rc, "clt", "0");
        QString cltcChrono = getVal(rc, "cltc_chrono", "0");
        QString cltChrono = getVal(rc, "clt_chrono", "0");
        QString totalPena = getVal(rc, "total_pena", "-1");

        QString hdReel = getVal(rc, "heure_depart_reel");
        if (hdReel.isEmpty()) hdReel = getVal(rc, "heure_depart_reelle");
        QString heureDepReel = !hdReel.isEmpty() ? toMs(hdReel, "0") : "0";

        QString haReel = getVal(rc, "heure_arrivee_reel");
        if (haReel.isEmpty()) haReel = getVal(rc, "heure_arrivee_reelle");
        QString heureArrReel = !haReel.isEmpty() ? toMs(haReel, "0") : "0";

        // Start time from Resultat_Chrono
        QString startTime = "-1";
        if (chronoByDossard.contains(br.dossard)) {
            auto& ct = chronoByDossard[br.dossard];
            if (ct.contains(0)) startTime = toMs(ct[0], "-1");
            else if (!ct.isEmpty()) startTime = toMs(ct[ct.keys().first()], "-1");

            // Seq 99 = finish line
            if (ct.contains(99) && heureArrReel == "0")
                heureArrReel = toMs(ct[99], "0");
        }
        // Fallback to Resultat_Course.Heure_depart
        QString hdDb = getVal(rc, "heure_depart");
        if (!hdDb.isEmpty() && startTime == "-1") startTime = toMs(hdDb, "-1");

        // Compute missing values
        if (heureArrReel == "0" && startTime != "-1" && tpsChrono != "-1") {
            heureArrReel = QString::number(startTime.toLongLong() + tpsChrono.toLongLong());
        }
        if (tpsChrono == "-1" && heureArrReel != "0" && startTime != "-1") {
            tpsChrono = QString::number(heureArrReel.toLongLong() - startTime.toLongLong());
            newlyFinished.insert(br.dossard);
        }
        if (tps == "-1" && tpsChrono != "-1") {
            qint64 pena = (totalPena != "-1" && !totalPena.isEmpty()) ? totalPena.toLongLong() : 0;
            tps = QString::number(tpsChrono.toLongLong() + pena * 1000);
        }

        // No time → no arrival
        if (tpsChrono == "-1") {
            heureArrReel = "0";
        } else if (heureArrReel == "0") {
            heureArrReel = QString::number(2000000000000LL + (qint64)idx * 1000);
        }

        // @-columns (28 values) — matching Python order exactly
        row << tpsChrono << cltChrono << cltChrono << "" << totalPena << ""
            << tps << "" << cltc << clt << "" << cltc << startTime << ""
            << "" << "" << "" << "" << "" << "" << "" << "" << "" << "" << "" << ""
            << tps << cltc;

        // Final columns (15 values)
        row << tpsChrono << cltChrono << cltcChrono << startTime
            << heureDepReel << heureArrReel
            << "" << tps << clt << cltc << totalPena
            << "" << "" << "" << "";

        allRows.append(row);

        // Track for ranking
        rowInfos.append({idx, br.codeCat, tps.toLongLong(), tpsChrono.toLongLong()});
    }

    // ═══════════════════════════════════════════════════════════════
    // Ranking calculation (Cltc by category, Clt scratch)
    // ═══════════════════════════════════════════════════════════════

    // Find column indices in allCols
    auto findColIdx = [&](const QString& name) -> int {
        for (int i = 0; i < allCols.size(); i++) {
            if (allCols[i].toArray()[0].toString() == name) return i;
        }
        return -1;
    };

    int idxCltc = findColIdx("Cltc" + cpStr);
    int idxClt = findColIdx("Clt" + cpStr);
    int idxCltcChrono = findColIdx("Cltc_chrono" + cpStr);
    int idxCltChrono = findColIdx("Clt_chrono" + cpStr);
    int idxTps = findColIdx("Tps" + cpStr);
    int idxTpsChrono = findColIdx("Tps_chrono" + cpStr);
    int idxAtRk = findColIdx("@RK" + suffix);
    int idxAtRkChrono = findColIdx("@RK_CHRONO" + suffix);

    auto computeRanking = [&](int idxTime, int idxRankCat, int idxRankScratch, int idxAtRank) {
        if (idxTime < 0 || idxRankCat < 0) return;

        // Scratch ranking
        QList<QPair<int, qint64>> timed;
        for (int i = 0; i < allRows.size(); i++) {
            qint64 t = allRows[i].toArray()[idxTime].toString().toLongLong();
            if (t > 0) timed.append({i, t});
        }
        std::sort(timed.begin(), timed.end(), [](auto& a, auto& b){ return a.second < b.second; });
        if (idxRankScratch >= 0) {
            for (int rank = 0; rank < timed.size(); rank++) {
                QJsonArray r = allRows[timed[rank].first].toArray();
                QString cur = r[idxRankScratch].toString();
                if (cur == "0" || cur.isEmpty() || cur == "-1")
                    r[idxRankScratch] = QString::number(rank + 1);
                allRows[timed[rank].first] = r;
            }
        }

        // Category ranking
        QMap<QString, QList<QPair<int, qint64>>> byCat;
        for (int i = 0; i < allRows.size(); i++) {
            QJsonArray r = allRows[i].toArray();
            QString cat = (findColIdx("Code_categorie") >= 0) ? r[findColIdx("Code_categorie")].toString() : "";
            qint64 t = r[idxTime].toString().toLongLong();
            if (t > 0) byCat[cat].append({i, t});
        }
        for (auto it = byCat.begin(); it != byCat.end(); ++it) {
            auto& entries = it.value();
            std::sort(entries.begin(), entries.end(), [](auto& a, auto& b){ return a.second < b.second; });
            for (int rank = 0; rank < entries.size(); rank++) {
                QJsonArray r = allRows[entries[rank].first].toArray();
                QString cur = r[idxRankCat].toString();
                if (cur == "0" || cur.isEmpty() || cur == "-1")
                    r[idxRankCat] = QString::number(rank + 1);
                if (idxAtRank >= 0) {
                    QString curAt = r[idxAtRank].toString();
                    if (curAt == "0" || curAt.isEmpty() || curAt == "-1")
                        r[idxAtRank] = QString::number(rank + 1);
                }
                allRows[entries[rank].first] = r;
            }
        }
    };

    computeRanking(idxTps, idxCltc, idxClt, idxAtRk);
    computeRanking(idxTpsChrono, idxCltcChrono, idxCltChrono, idxAtRkChrono);

    QJsonObject tableNode;
    tableNode["columns"] = allCols;
    tableNode["rows"] = allRows;
    result["ranking<sqlTable>"] = tableNode;

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Notification TCP
// ═══════════════════════════════════════════════════════════════════════════

void LiveBridge::tryReconnectNotif() {
    if (m_tcpSocket->state() == QAbstractSocket::UnconnectedState) {
        emit logMessage(QString("[NOTIF] Connexion → %1:%2...").arg(m_cfg.notifHost).arg(m_cfg.notifPort));
        m_tcpSocket->connectToHost(m_cfg.notifHost, m_cfg.notifPort);
    }
}

void LiveBridge::onNotifConnected() {
    emit logMessage("[NOTIF] ✓ Connecté");
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
                for (const QJsonValue& val : doc.array()) processNotif(val.toObject());
            }
        }
    }
}

void LiveBridge::processNotif(QJsonObject obj) {
    QString key = obj["key"].toString();

    // Track finished bibs
    if (key == "<bib_time>") {
        int passage = obj["passage"].toInt(-1);
        if (passage <= 0 && obj.contains("time_chrono"))
            m_finishedBibs.insert(obj["bib"].toVariant().toString());
    } else if (key == "<run_erase>") {
        m_finishedBibs.clear();
    } else if (key == "<on_course>") {
        // Filter out finished bibs from on_course
        QString tk = "table<sqlTable>";
        if (obj.contains(tk)) {
            QJsonObject table = obj[tk].toObject();
            QJsonArray rows = table["rows"].toArray();
            QJsonArray cols = table["columns"].toArray();
            int bibIdx = -1;
            for (int i = 0; i < cols.size(); i++) {
                if (cols[i].toArray()[0].toString() == "bib") { bibIdx = i; break; }
            }
            if (bibIdx >= 0) {
                QJsonArray newRows;
                for (const auto& r : rows) {
                    if (!m_finishedBibs.contains(r.toArray()[bibIdx].toString()))
                        newRows.append(r);
                }
                table["rows"] = newRows;
                obj[tk] = table;
            }
        }
    }

    if (key != "<on_course>") {
        emit logMessage("[NOTIF] " + key + " bib=" + obj["bib"].toVariant().toString());
    }
    broadcastWS(obj);
}

// ═══════════════════════════════════════════════════════════════════════════
//  WebSocket Server
// ═══════════════════════════════════════════════════════════════════════════

void LiveBridge::onWsNewConnection() {
    QWebSocket *pSocket = m_wsServer->nextPendingConnection();
    connect(pSocket, &QWebSocket::textMessageReceived, this, &LiveBridge::onWsTextMessage);
    connect(pSocket, &QWebSocket::disconnected, this, &LiveBridge::onWsDisconnected);
    m_wsClientKeyRace[pSocket] = "";
    emit logMessage(QString("⊕ WS client (%1)").arg(m_wsClientKeyRace.size()));
}

void LiveBridge::onWsTextMessage(const QString &message) {
    QWebSocket *pClient = qobject_cast<QWebSocket *>(sender());
    if (!pClient) return;

    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject req = doc.object();
    QString key = req["key"].toString();

    if (key == "<competition_load>") {
        if (!m_cacheCompetitionJson.isEmpty())
            pClient->sendTextMessage(m_cacheCompetitionJson);

    } else if (key == "<race_load>") {
        QString kr = req["key_race"].toString();
        if (kr == "*") kr = m_cacheRaceJson.isEmpty() ? "" : m_cacheRaceJson.firstKey();
        if (m_cacheRaceJson.contains(kr)) {
            pClient->sendTextMessage(m_cacheRaceJson[kr]);
            m_wsClientKeyRace[pClient] = kr;  // Track key_race per client!
            emit logMessage("[WS →] <race_load> [" + kr + "]");
        }

    } else if (key == "<epreuve_load>") {
        QString ep = req["epreuve"].toString();
        // key_race comes from tracked state, NOT from the request!
        QString kr = m_wsClientKeyRace.value(pClient, "");
        if (!kr.isEmpty()) {
            QSet<QString> newlyFinished;
            QJsonObject epData = buildEpreuveLoad(kr, ep, newlyFinished);
            m_finishedBibs.unite(newlyFinished);
            pClient->sendTextMessage(QJsonDocument(epData).toJson(QJsonDocument::Compact));
            emit logMessage(QString("[WS →] <epreuve_load> [%1]").arg(ep.isEmpty() ? "scratch" : ep));
        }

    } else if (key == "<ping>") {
        QJsonObject pong;
        pong["key"] = "<pong>";
        pong["online"] = (m_tcpSocket->state() == QAbstractSocket::ConnectedState);
        pClient->sendTextMessage(QJsonDocument(pong).toJson(QJsonDocument::Compact));
    }
}

void LiveBridge::onWsDisconnected() {
    QWebSocket *pClient = qobject_cast<QWebSocket *>(sender());
    if (pClient) {
        m_wsClientKeyRace.remove(pClient);
        pClient->deleteLater();
        emit logMessage(QString("⊖ WS client (%1)").arg(m_wsClientKeyRace.size()));
    }
}

void LiveBridge::broadcastWS(const QJsonObject& obj) {
    if (m_wsClientKeyRace.isEmpty()) return;
    QString msg = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    for (QWebSocket *pClient : m_wsClientKeyRace.keys())
        pClient->sendTextMessage(msg);
}