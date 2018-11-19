/*
 * Copyright 2018 by Marco Martin <mart@kde.org>
 * Copyright 2018 David Edmundson <davidedmundson@kde.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "abstractskillview.h"
#include "activeskillsmodel.h"
#include "abstractdelegate.h"
#include "sessiondatamap.h"
#include "sessiondatamodel.h"

#include <QWebSocket>
#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlContext>
#include <QQmlEngine>


AbstractSkillView::AbstractSkillView(QQuickItem *parent)
    : QQuickItem(parent),
      m_controller(MycroftController::instance()),
      m_id(QUuid::createUuid().toString())
{
    m_activeSkillsModel = new ActiveSkillsModel(this);

    m_guiWebSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_controller->registerView(this);

    connect(m_guiWebSocket, &QWebSocket::connected, this,
            [this] () {
                m_reconnectTimer.stop();
                emit statusChanged();
            });

    connect(m_guiWebSocket, &QWebSocket::disconnected, this, &AbstractSkillView::closed);

    connect(m_guiWebSocket, &QWebSocket::stateChanged, this,
            [this] (QAbstractSocket::SocketState state) {
                emit statusChanged();
            });

    connect(m_guiWebSocket, &QWebSocket::textMessageReceived, this, &AbstractSkillView::onGuiSocketMessageReceived);

    connect(m_guiWebSocket, &QWebSocket::stateChanged, this,
            [this](QAbstractSocket::SocketState socketState) {
                //TODO: when the connection closes, all session data and guis should be destroyed
                qWarning()<<"GUI SOCKET STATE:"<<socketState;
                //Try to reconnect if our connection died but the main server connection is still alive
                if (socketState == QAbstractSocket::UnconnectedState && m_url.isValid() && m_controller->status() == MycroftController::Open) {
                    m_reconnectTimer.start();
                }
            });



    connect(m_controller, &MycroftController::socketStatusChanged, this,
            [this]() {
                if (m_controller->status() == MycroftController::Open) {
                    if (m_url.isValid()) {
                        m_guiWebSocket->close();
                        m_guiWebSocket->open(m_url);
                    }
                } else if (status() != MycroftController::Open) {
                    m_guiWebSocket->close();
                    //don't assume the url will be still valid
                    m_url = QUrl();
                }
            });

    //Reconnect timer
    m_reconnectTimer.setInterval(1000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        m_guiWebSocket->close();
        m_guiWebSocket->open(m_url);
    });
}

AbstractSkillView::~AbstractSkillView()
{
}


QUrl AbstractSkillView::url() const
{
    return m_url;
}

void AbstractSkillView::setUrl(const QUrl &url)
{
    if (m_url == url) {
        return;
    }

    m_url = url;

    //don't connect if the controller is offline
    if (m_controller->status() == MycroftController::Open) {
        m_guiWebSocket->close();
        m_guiWebSocket->open(url);
    }
}

QString AbstractSkillView::id() const
{
    return m_id;
}

MycroftController::Status AbstractSkillView::status() const
{
    if (m_reconnectTimer.isActive()) {
        return MycroftController::Connecting;
    }

    switch(m_guiWebSocket->state())
    {
    case QAbstractSocket::ConnectingState:
    case QAbstractSocket::BoundState:
    case QAbstractSocket::HostLookupState:
        return MycroftController::Connecting;
    case QAbstractSocket::UnconnectedState:
        return MycroftController::Closed;
    case QAbstractSocket::ConnectedState:
        return MycroftController::Open;
    case QAbstractSocket::ClosingState:
        return MycroftController::Closing;
    default:
        return MycroftController::Connecting;
    }
}

ActiveSkillsModel *AbstractSkillView::activeSkills() const
{
    return m_activeSkillsModel;
}


SessionDataMap *AbstractSkillView::sessionDataForSkill(const QString &skillId)
{
    SessionDataMap *map = nullptr;

    if (m_skillData.contains(skillId)) {
        map = m_skillData[skillId];
    } else if (m_activeSkillsModel->skillIndex(skillId).isValid()) {
        map = new SessionDataMap(this);
        m_skillData[skillId] = map;
    }

    return map;
}

QList<QVariantMap> variantListToOrderedMap(const QVariantList &data)
{
    QList<QVariantMap> ordMap;

    QStringList roleNames;

    for (const auto &item : data) {
        if (!item.canConvert<QVariantMap>()) {
            qWarning() << "Error: Array data structure corrupted: " << data;
            return ordMap;
        }
        const auto &map = item.value<QVariantMap>();
        if (roleNames.isEmpty()) {
            roleNames = map.keys();
        } else if (roleNames != map.keys()) {
            qWarning() << "WARNING: Item with a wrong set of roles encountered, some roles will be inaccessible from QML, expected: " << roleNames << "Encountered: " << map.keys();
        }
        ordMap << map;
    }

    return ordMap;
}

QStringList jsonModelToStringList(const QString &key, const QJsonValue &data)
{
    QStringList items;

    if (!data.isArray()) {
        qWarning() << "Error: Model data is not an Array" << data;
        return items;
    }

    const auto &array = data.toArray();
    for (const auto &item : array) {
        if (!item.isObject()) {
            qWarning() << "Error: Array data structure currupted: " << data;
            items.clear();
            return items;
        }
        const auto &obj = item.toObject();
        if (obj.keys().length() != 1 || !obj.contains(key)) {
            qWarning() << "Error: Item with a wrong key encountered, expected: " << key << "Encountered: " << obj.keys();
            items.clear();
            return items;
        }
        const auto &value = obj.value(key);
        if (!value.isString()) {
            qWarning() << "Error: item in model not a string" << value;
        }
        items << value.toString();
    }

    return items;
}

void AbstractSkillView::onGuiSocketMessageReceived(const QString &message)
{
    auto doc = QJsonDocument::fromJson(message.toUtf8());

    if (doc.isEmpty()) {
        qWarning() << "Empty or invalid JSON message arrived on the gui socket:" << message;
        return;
    }

    auto type = doc[QStringLiteral("type")].toString();

    if (type.isEmpty()) {
        qWarning() << "Empty type in the JSON message on the gui socket";
        return;
    }

    qDebug() << "gui message type" << type;

///////////////SKILLDATA
    // The SkillData was updated by the server
    if (type == QLatin1String("mycroft.session.set")) {
        const QString skillId = doc[QStringLiteral("namespace")].toString();
        const QVariantMap data = doc[QStringLiteral("data")].toVariant().toMap();

        if (skillId.isEmpty()) {
            qWarning() << "Empty skill_id in mycroft.session.set";
            return;
        }
        if (!m_activeSkillsModel->skillIndex(skillId).isValid()) {
            qWarning() << "Invalid skill_id in mycroft.session.set:" << skillId;
            return;
        }
        if (data.isEmpty()) {
            qWarning() << "Empty dtata in mycroft.session.set";
            return;
        }

        //we already checked, assume *map is valid
        SessionDataMap *map = sessionDataForSkill(skillId);
         QVariantMap::const_iterator i;
        for (i = data.constBegin(); i != data.constEnd(); ++i) {
            //insert it as a model
            QList<QVariantMap> list = variantListToOrderedMap(i.value().value<QVariantList>());
            SessionDataModel *dm = map->value(i.key()).value<SessionDataModel *>();

            if (!list.isEmpty()) {
                if (!dm) {
                    dm = new SessionDataModel(map);
                    map->insertAndNotify(i.key(), QVariant::fromValue(dm));
                } else {
                    dm->clear();
                }
                dm->insertData(0, list);

            //insert it as is.
            } else {
                if (dm) {
                    dm->deleteLater();
                }
                map->insertAndNotify(i.key(), i.value());
            }
        }

    // The SkillData was removed by the server
    } else if (type == QLatin1String("mycroft.session.delete")) {
        const QString skillId = doc[QStringLiteral("namespace")].toString();
        const QString property = doc[QStringLiteral("property")].toString();
        if (skillId.isEmpty()) {
            qWarning() << "No skill_id provided in mycroft.session.delete";
            return;
        }
        if (!m_activeSkillsModel->skillIndex(skillId).isValid()) {
            qWarning() << "Invalid skill_id in mycroft.session.set:" << skillId;
            return;
        }
        if (property.isEmpty()) {
            qWarning() << "No property provided in mycroft.session.delete";
            return;
        }
        SessionDataMap *map = sessionDataForSkill(skillId);
        map->clearAndNotify(property);


//////SHOWGUI
    // The Skill from the server asked to show its gui
    } else if (type == QLatin1String("mycroft.gui.show")) {
        const QString skillId = doc[QStringLiteral("namespace")].toString();
        const QUrl delegateUrl(doc[QStringLiteral("gui_url")].toString());

        if (skillId.isEmpty()) {
            qWarning() << "Invalid mycroft.gui.show arrived with empty namespace";
            return;
        }
        if (delegateUrl.isEmpty()) {
            qWarning() << "Invalid mycroft.gui.show arrived with empty gui_url";
            return;
        }

        AbstractDelegate *delegate = m_activeSkillsModel->delegateForSkill(skillId, delegateUrl);

        if (delegate) {
            emit delegate->currentRequested();
        } else {
            QQmlEngine *engine = qmlEngine(this);
            QQmlContext *context = QQmlEngine::contextForObject(this);
            //NOTE: this should happen only when running as autotest
            //TODO: an ifdef?
            if (!engine) {
                engine = new QQmlEngine(this);
                context = engine->rootContext();
            }
            QQmlComponent delegateComponent(engine, delegateUrl, this);
            //TODO: separate context?
            QObject *guiObject = delegateComponent.beginCreate(context);
            delegate = qobject_cast<AbstractDelegate *>(guiObject);
            if (delegateComponent.isError()) {
                for (auto err : delegateComponent.errors()) {
                    qWarning() << err.toString();
                }
                return;
            }
            if (!delegate) {
                qWarning()<<"ERROR: QML gui not a Mycroft.AbstractDelegate instance";
                delegate->deleteLater();
                return;
            }

            delegate->setSkillId(skillId);
            delegate->setQmlUrl(delegateUrl);
            delegate->setSessionData(sessionDataForSkill(skillId));
            delegateComponent.completeCreate();
            m_activeSkillsModel->insertDelegate(delegate);
            emit delegate->currentRequested();
        }

        //TODO: change it to invoking a method on the gui object, to hide it from other skills
//        emit skillGuiCreated(skillId, guiItem);


/////////////ACTIVESKILLS

    // Insert new active skill
    //TODO: remove data
    } else if (type == QLatin1String("mycroft.session.insert") && doc[QStringLiteral("namespace")].toString() == QLatin1String("mycroft.system.active_skills")) {
        const int position = doc[QStringLiteral("position")].toInt();

        if (position < 0 || position > m_activeSkillsModel->rowCount()) {
            qWarning() << "Invalid position in mycroft.session.insert";
            return;
        }

        const QStringList skillList = jsonModelToStringList(QStringLiteral("skill_id"), doc[QStringLiteral("data")]);

        if (skillList.isEmpty()) {
            qWarning() << "Error: no valid skills received in mycroft.session.insert";
            return;
        }

        m_activeSkillsModel->insertSkills(position, skillList);


    // Active skill removed
    } else if (type == QLatin1String("mycroft.session.remove") && doc[QStringLiteral("namespace")].toString() == QLatin1String("mycroft.system.active_skills")) {
        const int position = doc[QStringLiteral("position")].toInt();
        const int itemsNumber = doc[QStringLiteral("items_number")].toInt();

        if (position < 0 || position > m_activeSkillsModel->rowCount() - 1) {
            qWarning() << "Invalid position";
            return;
        }
        if (itemsNumber < 0 || itemsNumber > m_activeSkillsModel->rowCount() - position - 1) {
            qWarning() << "Invalid items_number";
            return;
        }

        for (int i = 0; i < itemsNumber; ++i) {

            const QString skillId = m_activeSkillsModel->data(m_activeSkillsModel->index(position+i, 0)).toString();

            //TODO: do this after an animation
            {
                auto i = m_skillData.find(skillId);
                if (i != m_skillData.end()) {
                    i.value()->deleteLater();
                    m_skillData.erase(i);
                }
            }
        }
        m_activeSkillsModel->removeRows(position, itemsNumber);

    // Active skill moved
    } else if (type == QLatin1String("mycroft.session.move")) {
        const int from = doc[QStringLiteral("from")].toInt();
        const int to = doc[QStringLiteral("to")].toInt();
        const int itemsNumber = doc[QStringLiteral("items_number")].toInt();

        if (from < 0 || from > m_activeSkillsModel->rowCount() - 1) {
            qWarning() << "Invalid from position";
            return;
        }
        if (to < 0 || to > m_activeSkillsModel->rowCount() - 1) {
            qWarning() << "Invalid to position";
            return;
        }
        if (itemsNumber <= 0 || itemsNumber > m_activeSkillsModel->rowCount() - from) {
            qWarning() << "Invalid items_number";
            return;
        }
        m_activeSkillsModel->moveRows(QModelIndex(), from, itemsNumber, QModelIndex(), to);



//////EVENTS TODO
    // Action triggered from the server
    } else if (type == QLatin1String("mycroft.events.triggered")) {
        const QString skillOrSystem = doc[QStringLiteral("namespace")].toString();
        if (skillOrSystem.isEmpty()) {
            qWarning() << "No namespace provided for mycroft.events.triggered";
            return;
        }
        // If it's a skill it must exist
        if (skillOrSystem != QLatin1String("system") && !m_activeSkillsModel->skillIndex(skillOrSystem).isValid()) {
            qWarning() << "Invalid skill id passed as namespace for mycroft.events.triggered:" << skillOrSystem;
            return;
        }

        const QString eventName = doc[QStringLiteral("event_name")].toString();
        if (eventName.isEmpty()) {
            qWarning() << "No namespace provided for mycroft.events.triggered";
            return;
        }

        // data can also be empty
        const QVariantMap data = doc[QStringLiteral("data")].toVariant().toMap();

        QList<AbstractDelegate *> delegates = m_activeSkillsModel->delegatesForSkill(skillOrSystem == QLatin1String("system") ? QString() : skillOrSystem);

        for (auto *delegate : delegates) {
            emit delegate->event(eventName, data);
        }
    }
}

#include "moc_abstractskillview.cpp"