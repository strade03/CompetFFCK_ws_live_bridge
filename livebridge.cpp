#include "livebridge.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlField>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QMetaType>
#include <algorithm>


// ═══════════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

LiveBridge::LiveBridge(QObject *parent) : QObject(parent)
{
    m_tcpSocket = new QTcpSocket(this);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(5000);  // Reconnexion toutes les 5s

    m_wsServer = new QWebSocketServer("WS-Live", QWebSocketServer::NonSecureMode, this);

    // Connexions TCP (notifications CompetFFCK)
    connect(m_tcpSocket, &QTcpSocket::connected,    this, &LiveBridge::onNotifConnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead,    this, &LiveBridge::onNotifReadyRead);
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, &LiveBridge::onNotifDisconnected);
    connect(m_reconnectTimer, &QTimer::timeout,      this, &LiveBridge::tryReconnectNotif);

    // Connexions WebSocket (clients web)
    connect(m_wsServer, &QWebSocketServer::newConnection, this, &LiveBridge::onWsNewConnection);
}

LiveBridge::~LiveBridge()
{
    stop();
}


// ═══════════════════════════════════════════════════════════════════════════
//  Helpers statiques
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Convertit une valeur MySQL en millisecondes (string).
 * Gère : int, QTime, QDateTime, format "HH:MM:SS.fff", valeur brute.
 * Retourne @p def si la valeur est NULL, vide ou "0".
 */
QString LiveBridge::toMs(const QVariant& val, const QString& def)
{
    if (val.isNull() || !val.isValid())
        return def;

    QString s = val.toString().trimmed();
    if (s.isEmpty() || s == "0" || s.toLower() == "none")
        return def;

    // QTime / QDateTime natif
    if (val.typeId() == QMetaType::QTime || val.typeId() == QMetaType::QDateTime)
        return QString::number(val.toTime().msecsSinceStartOfDay());

    // Format "HH:MM:SS.fff" ou "MM:SS.fff"
    if (s.contains(':')) {
        QStringList parts = s.split(':');
        if (parts.size() >= 2) {
            int h   = parts.size() == 3 ? parts[0].toInt() : 0;
            int m   = parts[parts.size() - 2].toInt();
            QStringList sp = parts.last().split('.');
            int sec = sp[0].toInt();
            int ms  = sp.size() > 1 ? sp[1].leftJustified(3, '0').toInt() : 0;
            return QString::number((h * 3600 + m * 60 + sec) * 1000 + ms);
        }
    }

    // Valeur numérique brute
    bool ok;
    double d = s.toDouble(&ok);
    if (ok) return QString::number(qint64(d));

    return def;
}

/**
 * Crée une définition de colonne advTable.
 * Format : [name, label, format, widthDefault, style, type]
 * Le client JS lit columns[c][5] pour le type (CHRONO=7, RANKING=9, etc.)
 */
QJsonArray LiveBridge::makeCol(const QString& name, int type, const QString& fmt)
{
    QJsonArray col;
    col << name << name << fmt << "" << "" << QString::number(type);
    return col;
}

/**
 * Convertit un QMetaType MySQL en type advTable.
 * INT/LONG → ADV_LONG, FLOAT/DOUBLE → ADV_DOUBLE, DATE → ADV_DATE, sinon ADV_CHAR
 */
int LiveBridge::mysqlTypeToAdv(int mt)
{
    switch (mt) {
    case QMetaType::Short: case QMetaType::UShort:
    case QMetaType::Char:  case QMetaType::SChar: case QMetaType::UChar:
        return ADV_SHORT;
    case QMetaType::Int:      case QMetaType::UInt:
    case QMetaType::Long:     case QMetaType::ULong:
    case QMetaType::LongLong: case QMetaType::ULongLong:
        return ADV_LONG;
    case QMetaType::Float: case QMetaType::Double:
        return ADV_DOUBLE;
    case QMetaType::QDate: case QMetaType::QDateTime:
        return ADV_DATE;
    default:
        return ADV_CHAR;
    }
}

/**
 * Convertit le résultat d'une QSqlQuery en format advTable JSON.
 * Produit : { "tableName<sqlTable>": { "columns": [...], "rows": [...] } }
 * Les types MySQL sont convertis en types advTable automatiquement.
 */
QJsonObject LiveBridge::sqlTableToAdv(QSqlQuery& q, const QString& tableName)
{
    QJsonObject res;
    QJsonObject table;
    QSqlRecord rec = q.record();

    // Colonnes avec types MySQL → advTable
    QJsonArray cols;
    for (int i = 0; i < rec.count(); i++) {
        int advType = mysqlTypeToAdv(rec.field(i).metaType().id());
        cols.append(makeCol(rec.fieldName(i), advType));
    }
    table["columns"] = cols;

    // Lignes (NULL → "")
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

/**
 * Helper : récupère une valeur d'un QMap<QString,QVariant> en lowercase, None-safe.
 * Retourne @p def si la clé n'existe pas, est NULL, vide ou "None".
 */
static QString getVal(const QMap<QString, QVariant>& m, const QString& key, const QString& def = "")
{
    if (!m.contains(key.toLower()))
        return def;
    QVariant v = m[key.toLower()];
    if (v.isNull())
        return def;
    QString s = v.toString().trimmed();
    if (s.isEmpty() || s.toLower() == "none")
        return def;
    return s;
}


// ═══════════════════════════════════════════════════════════════════════════
//  Liste des compétitions / Démarrage / Arrêt
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Récupère la liste des compétitions depuis la BDD.
 * Utilise une connexion temporaire séparée (fetch_conn) pour ne pas
 * interférer avec la connexion live.
 * Le QSqlQuery est dans un bloc {} pour être détruit avant removeDatabase.
 */
QStringList LiveBridge::fetchCodex(const Config& cfg, const QString& filterActivite)
{
    QStringList list;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", "fetch_conn");
        db.setHostName(cfg.dbHost);
        db.setPort(cfg.dbPort);
        db.setUserName(cfg.dbUser);
        db.setPassword(cfg.dbPass);
        db.setDatabaseName(cfg.dbName);

        if (db.open()) {
            QSqlQuery q(db);

            // Requête avec jointure pour récupérer les numéros de course
            QString sql =
                "SELECT c.Codex, c.Code, c.Nom, c.Code_activite, c.Date_debut, "
                "GROUP_CONCAT(DISTINCT cc.Code_course ORDER BY cc.Code_course SEPARATOR ',') AS courses "
                "FROM Competition c "
                "LEFT JOIN Competition_Course cc ON cc.Code_competition = c.Code "
                "WHERE c.Codex IS NOT NULL AND c.Codex != '' ";

            // Filtre optionnel par type d'activité
            if (!filterActivite.isEmpty() && filterActivite != "Tout")
                sql += "AND c.Code_activite = '" + filterActivite + "' ";

            sql += "GROUP BY c.Codex, c.Code, c.Nom, c.Code_activite, c.Date_debut "
                   "ORDER BY c.Date_debut DESC, c.Code DESC LIMIT 50";

            q.exec(sql);
            while (q.next()) {
                QString codex  = q.value("Codex").toString().trimmed();
                if (codex.isEmpty()) continue;

                QString nom     = q.value("Nom").toString().trimmed();
                QString act     = q.value("Code_activite").toString().trimmed();
                QString courses = q.value("courses").toString();

                // Format d'affichage : "CODEX | ACT | C:1,2 | Nom de la compétition"
                QString display = codex;
                if (!act.isEmpty())     display += " | " + act;
                if (!courses.isEmpty()) display += " | C:" + courses;
                if (!nom.isEmpty())     display += " | " + (nom.length() > 40 ? nom.left(40) + "…" : nom);
                list << display;
            }
            db.close();
        } else {
            emit logMessage("[ERREUR BDD] " + db.lastError().text());
        }
    }
    // removeDatabase APRÈS destruction du QSqlQuery (hors du bloc {})
    QSqlDatabase::removeDatabase("fetch_conn");
    return list;
}

/**
 * Démarre le bridge : connexion MySQL, chargement structure, serveur WS, client TCP.
 */
bool LiveBridge::start(const Config& cfg)
{
    m_cfg = cfg;

    // Connexion MySQL persistante
    m_db = QSqlDatabase::addDatabase("QMYSQL", "live_conn");
    m_db.setHostName(m_cfg.dbHost);
    m_db.setPort(m_cfg.dbPort);
    m_db.setUserName(m_cfg.dbUser);
    m_db.setPassword(m_cfg.dbPass);
    m_db.setDatabaseName(m_cfg.dbName);

    if (!m_db.open()) {
        emit logMessage("[MYSQL ERREUR] " + m_db.lastError().text());
        return false;
    }
    emit logMessage("[MySQL] Connecté à " + m_cfg.dbHost + "/" + m_cfg.dbName);

    // Chargement de la structure depuis MySQL
    loadFromMySQL();

    // Démarrage du serveur WebSocket
    if (!m_wsServer->listen(QHostAddress::Any, m_cfg.wsPort)) {
        emit logMessage("[WS ERREUR] Port " + QString::number(m_cfg.wsPort));
        return false;
    }
    emit logMessage("[WS] ws://0.0.0.0:" + QString::number(m_cfg.wsPort));

    // Connexion au serveur de notifications TCP
    tryReconnectNotif();
    return true;
}

/**
 * Arrête proprement : TCP, WS, MySQL.
 */
void LiveBridge::stop()
{
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

/**
 * Charge la structure de la compétition depuis MySQL.
 * Pré-cache <competition_load> et <race_load> pour chaque course/phase.
 * <epreuve_load> n'est PAS caché — il est reconstruit à chaque demande
 * car les données évoluent en temps réel.
 */
void LiveBridge::loadFromMySQL()
{
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

    // Pré-cache de <race_load> pour chaque course/phase trouvée
    QJsonArray compRows = comp["competitions<sqlTable>"].toObject()["rows"].toArray();
    for (const QJsonValue& rv : compRows) {
        QString kr = rv.toArray()[0].toString();
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
//  buildCompetitionLoad — Liste des courses/phases disponibles
//  Réponse au message WebSocket <competition_load>
//  Le client web affiche la liste des compétitions en live
// ═══════════════════════════════════════════════════════════════════════════

QJsonObject LiveBridge::buildCompetitionLoad(const QString& codex)
{
    QJsonObject result;
    result["key"] = "<competition_load>";

    // Trouver le Code interne depuis le Codex
    // NB : la table Competition utilise "Code" (pas "Code_competition") comme PK
    QSqlQuery q(m_db);
    q.prepare("SELECT Code FROM Competition WHERE Codex = ?");
    q.addBindValue(codex);
    if (!q.exec() || !q.next()) {
        emit logMessage("[MySQL] Codex '" + codex + "' non trouvé !");
        return {};
    }
    int codeComp = q.value(0).toInt();
    emit logMessage(QString("[MySQL] Codex=%1 → Code=%2").arg(codex).arg(codeComp));

    // Charger les infos de la compétition
    q.exec(QString("SELECT * FROM Competition WHERE Code = %1").arg(codeComp));
    QMap<QString, QVariant> comp;
    if (q.next()) {
        QSqlRecord r = q.record();
        for (int i = 0; i < r.count(); i++)
            comp[r.fieldName(i)] = q.value(i);
    }

    // Charger Competition_Course (jours de course)
    q.exec(QString("SELECT * FROM Competition_Course WHERE Code_competition = %1").arg(codeComp));
    QList<QMap<QString, QVariant>> ccRows;
    while (q.next()) {
        QMap<QString, QVariant> m;
        QSqlRecord r = q.record();
        for (int i = 0; i < r.count(); i++) m[r.fieldName(i)] = q.value(i);
        ccRows << m;
    }

    // Charger Competition_Course_Phase (qualifs, finale, etc.)
    q.exec(QString("SELECT * FROM Competition_Course_Phase WHERE Code_competition = %1").arg(codeComp));
    QList<QMap<QString, QVariant>> cpRows;
    while (q.next()) {
        QMap<QString, QVariant> m;
        QSqlRecord r = q.record();
        for (int i = 0; i < r.count(); i++) m[r.fieldName(i)] = q.value(i);
        cpRows << m;
    }

    // Helper pour convertir QVariant → QString (NULL → "")
    auto vs = [](const QVariant& v) -> QString { return v.isNull() ? "" : v.toString(); };

    // Colonnes de la table competitions (identiques au Python)
    QJsonArray columns;
    columns << QJsonArray({"key", "key"}) << QJsonArray({"active", "active"})
            << QJsonArray({"Code_competition", "Code_competition"})
            << QJsonArray({"Code_course", "Code_course"}) << QJsonArray({"Code_phase", "Code_phase"})
            << QJsonArray({"Codex", "Codex"}) << QJsonArray({"Code_activite", "Code_activite"})
            << QJsonArray({"Nom", "Nom"}) << QJsonArray({"Ville", "Ville"})
            << QJsonArray({"Riviere", "Riviere"}) << QJsonArray({"Organisateur", "Organisateur"})
            << QJsonArray({"Date_debut", "Date_debut"}) << QJsonArray({"Date_fin", "Date_fin"})
            << QJsonArray({"Libelle_course", "Libelle_course"}) << QJsonArray({"Date_course", "Date_course"})
            << QJsonArray({"Libelle_phase", "Libelle_phase"}) << QJsonArray({"Date_phase", "Date_phase"});

    // Construire une ligne par combinaison Course × Phase
    QJsonArray rows;
    for (const auto& cc : ccRows) {
        for (const auto& cp : cpRows) {
            if (cp["Code_course"].toInt() != cc["Code_course"].toInt())
                continue;

            int ccVal = cc["Code_course"].toInt();
            int cpVal = cp["Code_phase"].toInt();

            // key_race = "CODEX_course_phase" (ex: "FFCK20250553_1_1")
            QString kr = QString("%1_%2_%3").arg(codex).arg(ccVal).arg(cpVal);

            // active = 1 si c'est la course/phase demandée (ou toutes si codeCourse=0)
            int active = (m_cfg.codeCourse == 0 ||
                         (ccVal == m_cfg.codeCourse && cpVal == m_cfg.codePhase)) ? 1 : 0;

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
//  buildRaceLoad — Structure d'une course/phase
//  Réponse au message WebSocket <race_load>
//  Contient : Competition, Competition_Course, Competition_Course_Phase,
//             Competition_Course_Phase_Manche_Epreuve (liste des catégories)
//  Le client JS utilise adv.GetTable() qui cherche dans obj.tables['Name<sqlTable>']
// ═══════════════════════════════════════════════════════════════════════════

QJsonObject LiveBridge::buildRaceLoad(const QString& kr)
{
    QJsonObject result;
    result["key"] = "<race_load>";

    // Parser key_race : "CODEX_course_phase"
    QStringList p = kr.split("_");
    if (p.size() < 3) return result;
    QString codex = p[0];
    int cc = p[1].toInt();
    int cp = p[2].toInt();
    result["Code_course"] = cc;
    result["Code_phase"] = cp;

    QSqlQuery q(m_db);
    q.prepare("SELECT Code FROM Competition WHERE Codex = ?");
    q.addBindValue(codex);
    if (!q.exec() || !q.next()) return result;
    int codeComp = q.value(0).toInt();
    result["Code_competition"] = codeComp;

    // Sous-objet "tables" — le JS fait adv.GetTable(obj, 'Competition')
    // qui cherche dans obj.tables['Competition<sqlTable>']
    QJsonObject tables;
    auto merge = [&](const QJsonObject& src) {
        for (auto it = src.begin(); it != src.end(); ++it)
            tables.insert(it.key(), it.value());
    };

    // Competition (toutes les infos)
    q.exec(QString("SELECT * FROM Competition WHERE Code = %1").arg(codeComp));
    merge(sqlTableToAdv(q, "Competition"));

    // Competition_Course (tous les jours)
    q.exec(QString("SELECT * FROM Competition_Course WHERE Code_competition = %1").arg(codeComp));
    merge(sqlTableToAdv(q, "Competition_Course"));

    // Competition_Course_Phase (toutes les phases — pas filtré par cc/cp !)
    q.exec(QString("SELECT * FROM Competition_Course_Phase WHERE Code_competition = %1").arg(codeComp));
    merge(sqlTableToAdv(q, "Competition_Course_Phase"));

    // Épreuves : triées par Ordre pour l'affichage dans le bon ordre
    q.exec(QString("SELECT * FROM Competition_Course_Phase_Manche_Epreuve "
                   "WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3 "
                   "ORDER BY Ordre").arg(codeComp).arg(cc).arg(cp));
    merge(sqlTableToAdv(q, "Competition_Course_Phase_Manche_Epreuve"));

    result["tables"] = tables;

    // Nb_inter / Nb_porte — requête séparée car on a besoin des valeurs brutes
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
//  buildEpreuveLoad — Classement d'une épreuve (catégorie)
//  Réponse au message WebSocket <epreuve_load>
//
//  Construit le ranking depuis :
//    - Resultat : identité des concurrents (dossard, bateau, club, catégorie)
//    - Resultat_Course : temps chrono, pénalités, classement (par Code_bateau)
//    - Resultat_Chrono : heures de départ/arrivée (par Dossard + Seq)
//
//  Ajoute les colonnes dynamiques :
//    - @PENA_1_1..@PENA_N_1 : pénalités par porte (slalom)
//    - @CHRONO, @TIME, @RK, @START_TIME... : colonnes @ pour compat slalom
//    - Tps_chrono1_1, Cltc1_1, Heure_depart1_1... : colonnes finales (descente)
//
//  Calcule les classements Cltc (par catégorie) et Clt (scratch).
// ═══════════════════════════════════════════════════════════════════════════

LiveBridge::EpreuveResult LiveBridge::buildEpreuveLoad(const QString& kr, const QString& epreuve)
{
    EpreuveResult er;
    QJsonObject result;
    result["key"] = "<epreuve_load>";
    result["epreuve"] = epreuve;

    // Parser key_race
    QStringList p = kr.split("_");
    if (p.size() < 3) { er.data = result; return er; }
    QString codex = p[0];
    int cc = p[1].toInt();
    int cp = p[2].toInt();
    int cm = 1;  // Code_manche (toujours 1 en slalom comme en descente)
    QString suffix = QString("_%1_%2_%3").arg(cc).arg(cp).arg(cm);  // ex: "_1_1_1"
    QString cpStr  = QString("%1_%2").arg(cc).arg(cp);               // ex: "1_1"

    QSqlQuery q(m_db);
    q.prepare("SELECT Code FROM Competition WHERE Codex = ?");
    q.addBindValue(codex);
    if (!q.exec() || !q.next()) { er.data = result; return er; }
    int codeComp = q.value(0).toInt();

    // ─── Nb_inter / Nb_porte ─────────────────────────────────────
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

    // ─── Resultat : identité des concurrents ─────────────────────
    QString sql = QString("SELECT * FROM Resultat WHERE Code_competition = %1").arg(codeComp);
    if (!epreuve.isEmpty())
        sql += " AND Code_categorie = '" + epreuve + "'";
    sql += " ORDER BY Code_categorie, Dossard";
    q.exec(sql);

    if (!q.isActive()) {
        result["ranking<sqlTable>"] = QJsonObject();
        er.data = result;
        return er;
    }

    // Colonnes de base depuis Resultat (avec types MySQL)
    QSqlRecord rec = q.record();
    QJsonArray baseCols;
    QStringList baseColNames;
    for (int i = 0; i < rec.count(); i++) {
        baseCols.append(makeCol(rec.fieldName(i), mysqlTypeToAdv(rec.field(i).metaType().id())));
        baseColNames << rec.fieldName(i);
    }

    // Collecter les lignes de Resultat
    struct BaseRow { QJsonArray data; QString dossard, codeBateau, codeCat; };
    QList<BaseRow> baseRows;
    while (q.next()) {
        QJsonArray row;
        QString dos, cb, cat;
        for (int i = 0; i < rec.count(); i++) {
            QVariant v = q.value(i);
            QString s = v.isNull() ? "" : v.toString();
            row.append(s);
            if (rec.fieldName(i) == "Dossard")         dos = s.trimmed();
            else if (rec.fieldName(i) == "Code_bateau") cb = s.trimmed();
            else if (rec.fieldName(i) == "Code_categorie") cat = s.trimmed();
        }
        baseRows.append({row, dos, cb, cat});
    }

    // ─── Colonnes dynamiques ─────────────────────────────────────

    // Pénalités par porte (slalom : @PENA_1_1 .. @PENA_N_1)
    QJsonArray penaCols;
    for (int i = 1; i <= nbPorte; i++)
        penaCols.append(makeCol(QString("@PENA_%1_%2").arg(i).arg(cm), ADV_SHORT));

    // Colonnes @... (compatibilité slalom — le JS SetColumnNameRanking les renomme)
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

    // Colonnes finales (noms directs — pour la descente, le JS ne les renomme PAS)
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

    // Assemblage de toutes les colonnes
    QJsonArray allCols = baseCols;
    for (const auto& c : penaCols)  allCols.append(c);
    for (const auto& c : atCols)    allCols.append(c);
    for (const auto& c : finalCols) allCols.append(c);

    // ─── 1. Resultat_Chrono : heures de départ/arrivée par Dossard ───
    // Seq 0 (ou min) = heure de départ, Seq 99 = heure d'arrivée
    QMap<QString, QMap<int, QVariant>> chronoByDos;
    q.exec(QString("SELECT Dossard, Seq, Heure FROM Resultat_Chrono "
                   "WHERE Code_competition = %1 AND Code_course = %2 AND Code_phase = %3")
           .arg(codeComp).arg(cc).arg(cp));
    while (q.next()) {
        QString d = q.value(0).toString().trimmed();
        int seq   = q.value(1).toInt();
        if (!d.isEmpty())
            chronoByDos[d][seq] = q.value(2);
    }

    // ─── 2. Resultat_Course : temps/pénalités par Code_bateau ────────
    // Filtré par Code_manche pour éviter les "bateaux parasites" inter-phases
    QMap<QString, QMap<QString, QVariant>> rcByBateau;
    q.exec(QString("SELECT * FROM Resultat_Course "
                   "WHERE Code_competition = %1 AND Code_course = %2 "
                   "AND Code_phase = %3 AND Code_manche = %4")
           .arg(codeComp).arg(cc).arg(cp).arg(cm));
    if (q.isActive()) {
        QSqlRecord rr = q.record();
        while (q.next()) {
            QMap<QString, QVariant> rd;
            for (int i = 0; i < rr.count(); i++)
                rd[rr.fieldName(i).toLower()] = q.value(i);
            QString cb = rd["code_bateau"].toString().trimmed();
            if (!cb.isEmpty())
                rcByBateau[cb] = rd;
        }
    }

    // ─── Construction des lignes du ranking ──────────────────────────

    QJsonArray allRows;
    for (int idx = 0; idx < baseRows.size(); idx++) {
        const BaseRow& br = baseRows[idx];
        QJsonArray row = br.data;  // Commence avec les colonnes de Resultat

        // Données Resultat_Course pour ce bateau
        auto rc = rcByBateau.value(br.codeBateau);

        // ── Parsing des pénalités (slalom) ──
        // Format : chaîne de caractères, 1 char par porte
        // "0" = ok, "2" = touché (+2s), "5" = raté (+50s)
        QString penaStr = getVal(rc, "penalite", "");
        int sumPena = 0, nbPenaJudged = 0;
        for (int i = 0; i < nbPorte; i++) {
            if (i < penaStr.length()) {
                QChar ch = penaStr[i];
                if (ch == '0')      { row.append("0");  nbPenaJudged++; }
                else if (ch == '2') { row.append("2");  sumPena += 2;  nbPenaJudged++; }
                else if (ch == '5') { row.append("50"); sumPena += 50; nbPenaJudged++; }
                else                { row.append(""); }
            } else {
                row.append("");
            }
        }
        QString totalPena = (nbPenaJudged > 0) ? QString::number(sumPena)
                                                 : getVal(rc, "total_pena", "-1");

        // ── Temps chrono et temps total ──
        QString tChr = getVal(rc, "tps_chrono", "-1");
        QString tFin = getVal(rc, "tps", "-1");
        if (tChr == "0" || tChr == "0.0") tChr = "-1";
        if (tFin == "0" || tFin == "0.0") tFin = "-1";
        QString tpsChrono = toMs(tChr, "-1");
        QString tps       = toMs(tFin, "-1");

        // Marquer comme "arrivé" seulement si chrono positif réel
        if (tpsChrono.toLongLong() > 0)
            er.finishedBibs.insert(br.dossard);

        // Classements depuis Resultat_Course (pré-calculés par CompetFFCK)
        QString cltc       = getVal(rc, "cltc", "0");
        QString clt        = getVal(rc, "clt", "0");
        QString cltcChrono = getVal(rc, "cltc_chrono", "0");
        QString cltChrono  = getVal(rc, "clt_chrono", "0");

        // Heures réelles
        QString hdR = getVal(rc, "heure_depart_reel");
        if (hdR.isEmpty()) hdR = getVal(rc, "heure_depart_reelle");
        QString heureDepReel = !hdR.isEmpty() ? toMs(hdR, "0") : "0";

        QString haR = getVal(rc, "heure_arrivee_reel");
        if (haR.isEmpty()) haR = getVal(rc, "heure_arrivee_reelle");
        QString heureArrReel = !haR.isEmpty() ? toMs(haR, "0") : "0";

        // ── Heure de départ depuis Resultat_Chrono ──
        QString startTime = "-1";
        if (chronoByDos.contains(br.dossard)) {
            auto& ct = chronoByDos[br.dossard];

            // Seq 0 = heure de départ programmée
            if (ct.contains(0))
                startTime = toMs(ct[0], "-1");
            else if (!ct.isEmpty())
                startTime = toMs(ct[ct.keys().first()], "-1");

            // Seq 99 = arrivée, SEULEMENT si le bateau a un Resultat_Course
            // (évite la contamination inter-phases)
            if (ct.contains(99) && heureArrReel == "0" && !rc.isEmpty())
                heureArrReel = toMs(ct[99], "0");
        }

        // Fallback : heure de départ depuis Resultat_Course
        QString hdDb = getVal(rc, "heure_depart");
        if (!hdDb.isEmpty() && startTime == "-1")
            startTime = toMs(hdDb, "-1");

        // Calcul heure d'arrivée = départ + chrono (si chrono connu)
        if (heureArrReel == "0" && startTime != "-1" && tpsChrono != "-1") {
            qint64 tc = tpsChrono.toLongLong();
            if (tc > 0)
                heureArrReel = QString::number(startTime.toLongLong() + tc);
        }

        // NE PAS calculer chrono depuis arrivée - départ
        // → Crée des faux positifs entre phases (contamination)
        // Le chrono vient EXCLUSIVEMENT de Resultat_Course.Tps_chrono

        // ── Slalom : Tps = Tps_chrono + pénalités * 1000 ──
        // En descente : sumPena = 0 (pas de portes) donc Tps = Tps_chrono
        if (tps == "-1" && tpsChrono != "-1") {
            tps = QString::number(tpsChrono.toLongLong() + sumPena * 1000LL);
        }

        // Bateau sans chrono → pas d'heure d'arrivée
        if (tpsChrono == "-1") {
            heureArrReel = "0";
        } else if (heureArrReel == "0") {
            // Timestamp géant pour trier les bateaux sans arrivée en fin de liste
            heureArrReel = QString::number(2000000000000LL + idx * 1000LL);
        }

        // ── Colonnes @... (28 valeurs) ──
        // Ordre identique au Python : @CHRONO, @RK_CHRONO, @RK_CHRONO_SCRATCH,
        // @PENASTRING, @SUMPENA, @NBPENA, @TIME, @PTS, @RK, @RK_SCRATCH,
        // @DIFF, @START_RK, @START_TIME, @ETAT_PENA, inter1×3, inter2×3, @TIME_cp, @RK_cp
        row << tpsChrono << cltChrono << cltChrono << "" << totalPena << QString::number(nbPenaJudged)
            << tps << "" << cltc << clt << "" << cltc << startTime << ""
            << "" << "" << "" << "" << "" << "" << "" << "" << "" << "" << "" << ""
            << tps << cltc;

        // ── Colonnes finales (15 valeurs) ──
        // Noms directs pour la descente (le JS ne les renomme pas)
        row << tpsChrono << cltChrono << cltcChrono << startTime
            << heureDepReel << heureArrReel
            << "" << tps << clt << cltc << totalPena
            << "" << "" << "" << "";

        allRows.append(row);
    }

    // ═══════════════════════════════════════════════════════════════
    //  Calcul des classements (Cltc par catégorie, Clt scratch)
    //  Le JS attend ces valeurs pré-calculées pour l'affichage initial.
    // ═══════════════════════════════════════════════════════════════

    auto findCol = [&](const QString& name) -> int {
        for (int i = 0; i < allCols.size(); i++)
            if (allCols[i].toArray()[0].toString() == name) return i;
        return -1;
    };

    int idxCltc    = findCol("Cltc" + cpStr);
    int idxClt     = findCol("Clt" + cpStr);
    int idxCltcC   = findCol("Cltc_chrono" + cpStr);
    int idxCltC    = findCol("Clt_chrono" + cpStr);
    int idxTps     = findCol("Tps" + cpStr);
    int idxTpsC    = findCol("Tps_chrono" + cpStr);
    int idxAtRk    = findCol("@RK" + suffix);
    int idxAtRkC   = findCol("@RK_CHRONO" + suffix);
    int idxCat     = findCol("Code_categorie");

    /**
     * Calcule les classements scratch et par catégorie.
     * @param iTime      Index de la colonne temps
     * @param iRkCat     Index de la colonne classement par catégorie
     * @param iRkScr     Index de la colonne classement scratch
     * @param iAtRk      Index de la colonne @RK (slalom compat)
     */
    auto computeRanking = [&](int iTime, int iRkCat, int iRkScr, int iAtRk) {
        if (iTime < 0 || iRkCat < 0) return;

        // Collecter les bateaux avec un temps positif
        QList<QPair<int, qint64>> timed;
        for (int i = 0; i < allRows.size(); i++) {
            qint64 t = allRows[i].toArray()[iTime].toString().toLongLong();
            if (t > 0) timed.append({i, t});
        }
        std::sort(timed.begin(), timed.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        // Classement scratch (tous concurrents)
        if (iRkScr >= 0) {
            for (int r = 0; r < timed.size(); r++) {
                QJsonArray rw = allRows[timed[r].first].toArray();
                QString cur = rw[iRkScr].toString();
                if (cur == "0" || cur.isEmpty() || cur == "-1") {
                    rw[iRkScr] = QString::number(r + 1);
                    allRows[timed[r].first] = rw;
                }
            }
        }

        // Classement par catégorie
        if (idxCat >= 0) {
            QMap<QString, QList<QPair<int, qint64>>> byCat;
            for (const auto& [i, t] : timed) {
                QString cat = allRows[i].toArray()[idxCat].toString();
                byCat[cat].append({i, t});
            }

            for (auto it = byCat.begin(); it != byCat.end(); ++it) {
                auto& entries = it.value();
                std::sort(entries.begin(), entries.end(),
                          [](const auto& a, const auto& b) { return a.second < b.second; });

                for (int r = 0; r < entries.size(); r++) {
                    QJsonArray rw = allRows[entries[r].first].toArray();

                    // Cltc (classement catégorie)
                    QString cur = rw[iRkCat].toString();
                    if (cur == "0" || cur.isEmpty() || cur == "-1")
                        rw[iRkCat] = QString::number(r + 1);

                    // @RK (slalom compat)
                    if (iAtRk >= 0) {
                        QString ca = rw[iAtRk].toString();
                        if (ca == "0" || ca.isEmpty() || ca == "-1")
                            rw[iAtRk] = QString::number(r + 1);
                    }

                    allRows[entries[r].first] = rw;
                }
            }
        }
    };

    // Calcul sur Tps (temps total = chrono + pénalités)
    computeRanking(idxTps, idxCltc, idxClt, idxAtRk);
    // Calcul sur Tps_chrono (chrono pur)
    computeRanking(idxTpsC, idxCltcC, idxCltC, idxAtRkC);

    // Assemblage du résultat
    QJsonObject tableNode;
    tableNode["columns"] = allCols;
    tableNode["rows"] = allRows;
    result["ranking<sqlTable>"] = tableNode;

    er.data = result;
    return er;
}


// ═══════════════════════════════════════════════════════════════════════════
//  Notification TCP — Connexion au serveur de notifications de CompetFFCK
//  Protocole : paquets JSON séparés par \0
// ═══════════════════════════════════════════════════════════════════════════

void LiveBridge::tryReconnectNotif()
{
    if (m_tcpSocket->state() == QAbstractSocket::UnconnectedState) {
        emit logMessage(QString("[NOTIF] Connexion → %1:%2...")
                       .arg(m_cfg.notifHost).arg(m_cfg.notifPort));
        m_tcpSocket->connectToHost(m_cfg.notifHost, m_cfg.notifPort);
    }
}

void LiveBridge::onNotifConnected()
{
    emit logMessage("[NOTIF] ✓ Connecté");
    m_reconnectTimer->stop();
}

void LiveBridge::onNotifDisconnected()
{
    emit logMessage("[NOTIF] Déconnecté. Reconnexion...");
    m_reconnectTimer->start();
}

/**
 * Lecture des paquets TCP.
 * Les données arrivent en continu, séparées par \0.
 * Chaque paquet est un JSON (objet ou tableau d'objets).
 */
void LiveBridge::onNotifReadyRead()
{
    m_notifBuffer.append(m_tcpSocket->readAll());
    int sepIdx;
    while ((sepIdx = m_notifBuffer.indexOf('\x00')) != -1) {
        QByteArray packet = m_notifBuffer.left(sepIdx);
        m_notifBuffer.remove(0, sepIdx + 1);

        if (!packet.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(packet);
            if (doc.isObject()) {
                processNotif(doc.object());
            } else if (doc.isArray()) {
                for (const QJsonValue& v : doc.array())
                    processNotif(v.toObject());
            }
        }
    }
}

/**
 * Traitement d'une notification.
 *
 * <bib_time>    : un bateau a franchi la ligne → marquer comme "arrivé"
 * <run_erase>   : remise à zéro de la manche → vider les arrivés
 * <on_course>   : bateaux en piste → filtrer ceux déjà arrivés
 *
 * Toutes les notifications sont retransmises aux clients WebSocket.
 */
void LiveBridge::processNotif(QJsonObject obj)
{
    QString key = obj["key"].toString();

    if (key == "<bib_time>") {
        // Un bateau a franchi la ligne d'arrivée (passage <= 0 = finish)
        int passage = obj["passage"].toInt(-1);
        if (passage <= 0 && obj.contains("time_chrono"))
            m_finishedBibs.insert(obj["bib"].toVariant().toString());
    }
    else if (key == "<run_erase>") {
        // Remise à zéro de la manche
        m_finishedBibs.clear();
    }
    else if (key == "<on_course>") {
        // Filtrer les bateaux déjà arrivés de la table on_course
        // pour éviter qu'ils restent affichés avec un temps tournant
        QString tk = "table<sqlTable>";
        if (obj.contains(tk)) {
            QJsonObject table = obj[tk].toObject();
            QJsonArray rows = table["rows"].toArray();
            QJsonArray cols = table["columns"].toArray();

            // Trouver l'index de la colonne "bib"
            int bibIdx = -1;
            for (int i = 0; i < cols.size(); i++) {
                if (cols[i].toArray()[0].toString() == "bib") {
                    bibIdx = i;
                    break;
                }
            }

            // Retirer les bateaux arrivés
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

    // Log (sauf on_course qui arrive toutes les 100ms)
    if (key != "<on_course>")
        emit logMessage("[NOTIF] " + key + " bib=" + obj["bib"].toVariant().toString());

    // Retransmission à tous les clients WebSocket
    broadcastWS(obj);
}


// ═══════════════════════════════════════════════════════════════════════════
//  WebSocket Server — Communication avec les pages web (live.html, etc.)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Nouveau client WebSocket connecté.
 * On track son key_race (initialement vide, mis à jour par <race_load>).
 */
void LiveBridge::onWsNewConnection()
{
    QWebSocket* pSocket = m_wsServer->nextPendingConnection();
    connect(pSocket, &QWebSocket::textMessageReceived, this, &LiveBridge::onWsTextMessage);
    connect(pSocket, &QWebSocket::disconnected,        this, &LiveBridge::onWsDisconnected);
    m_wsClientKeyRace[pSocket] = "";
    emit logMessage(QString("⊕ WS client (%1)").arg(m_wsClientKeyRace.size()));
}

/**
 * Message reçu d'un client WebSocket.
 *
 * Commandes gérées :
 *   <competition_load> : renvoie la liste des courses/phases
 *   <race_load>        : renvoie la structure d'une course + track key_race
 *   <epreuve_load>     : renvoie le ranking d'une épreuve (reconstruit depuis MySQL)
 *   <ping>             : renvoie <pong> avec l'état de connexion TCP
 */
void LiveBridge::onWsTextMessage(const QString& message)
{
    QWebSocket* pClient = qobject_cast<QWebSocket*>(sender());
    if (!pClient) return;

    QJsonObject req = QJsonDocument::fromJson(message.toUtf8()).object();
    QString key = req["key"].toString();

    if (key == "<competition_load>") {
        // Renvoie le cache pré-construit
        if (!m_cacheCompetitionJson.isEmpty())
            pClient->sendTextMessage(m_cacheCompetitionJson);
    }
    else if (key == "<race_load>") {
        QString kr = req["key_race"].toString();
        if (kr == "*")
            kr = m_cacheRaceJson.isEmpty() ? "" : m_cacheRaceJson.firstKey();

        if (m_cacheRaceJson.contains(kr)) {
            pClient->sendTextMessage(m_cacheRaceJson[kr]);
            // IMPORTANT : tracker le key_race de ce client
            // Le JS n'envoie PAS key_race dans <epreuve_load>
            m_wsClientKeyRace[pClient] = kr;
            emit logMessage("[WS →] <race_load> [" + kr + "]");
        }
    }
    else if (key == "<epreuve_load>") {
        QString ep = req["epreuve"].toString();
        // key_race vient du state tracké, PAS du message (le JS ne l'envoie pas)
        QString kr = m_wsClientKeyRace.value(pClient, "");

        if (!kr.isEmpty()) {
            // Reconstruction depuis MySQL à chaque demande (données live)
            EpreuveResult er = buildEpreuveLoad(kr, ep);
            m_finishedBibs.unite(er.finishedBibs);
            pClient->sendTextMessage(QJsonDocument(er.data).toJson(QJsonDocument::Compact));
            emit logMessage(QString("[WS →] <epreuve_load> [%1]").arg(ep.isEmpty() ? "scratch" : ep));
        }
    }
    else if (key == "<ping>") {
        QJsonObject pong;
        pong["key"] = "<pong>";
        pong["online"] = (m_tcpSocket->state() == QAbstractSocket::ConnectedState);
        pClient->sendTextMessage(QJsonDocument(pong).toJson(QJsonDocument::Compact));
    }
}

/**
 * Client WebSocket déconnecté — nettoyage.
 */
void LiveBridge::onWsDisconnected()
{
    QWebSocket* pClient = qobject_cast<QWebSocket*>(sender());
    if (pClient) {
        m_wsClientKeyRace.remove(pClient);
        pClient->deleteLater();
        emit logMessage(QString("⊖ WS client (%1)").arg(m_wsClientKeyRace.size()));
    }
}

/**
 * Broadcast une notification à tous les clients WebSocket connectés.
 * Pas de filtre par key_race — en mode local, tout le monde reçoit tout.
 */
void LiveBridge::broadcastWS(const QJsonObject& obj)
{
    if (m_wsClientKeyRace.isEmpty()) return;
    QString msg = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    for (QWebSocket* pClient : m_wsClientKeyRace.keys())
        pClient->sendTextMessage(msg);
}