/** This file is part of Contacts daemon
 **
 ** Copyright (c) 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 **
 ** Contact:  Nokia Corporation (info@qt.nokia.com)
 **
 ** GNU Lesser General Public License Usage
 ** This file may be used under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation and appearing in the
 ** file LICENSE.LGPL included in the packaging of this file.  Please review the
 ** following information to ensure the GNU Lesser General Public License version
 ** 2.1 requirements will be met:
 ** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
 **
 ** In addition, as a special exception, Nokia gives you certain additional rights.
 ** These rights are described in the Nokia Qt LGPL Exception version 1.1, included
 ** in the file LGPL_EXCEPTION.txt in this package.
 **
 ** Other Usage
 ** Alternatively, this file may be used in accordance with the terms and
 ** conditions contained in a signed written agreement between you and Nokia.
 **/

#include "cdbirthdaycontroller.h"
#include "cdbirthdaycalendar.h"
#include "cdbirthdayplugin.h"
#include "debug.h"

#include <QDir>
#include <QFile>

#include <QContactBirthday>
#include <QContactDetailFilter>
#include <QContactFetchRequest>
#include <QContactSyncTarget>
#ifdef USING_QTPIM
#include <QContactIdFilter>
#else
#include <QContactLocalIdFilter>
#endif

#ifdef USING_QTPIM
QTCONTACTS_USE_NAMESPACE
#else
QTM_USE_NAMESPACE
#endif

using namespace Contactsd;

namespace {

// version number for current type of birthday events. Increase number when changing event details.
const int CURRENT_BIRTHDAY_VERSION = 1;
const int UPDATE_TIMEOUT = 1000; // ms

template<typename DetailType>
QContactDetailFilter detailFilter(
#ifdef USING_QTPIM
    int field = -1
#else
    const QString &field = QString()
#endif
)
{
    QContactDetailFilter filter;
#ifdef USING_QTPIM
    filter.setDetailType(DetailType::Type, field);
#else
    filter.setDetailDefinitionName(DetailType::DefinitionName, field);
#endif
    return filter;
}

}

CDBirthdayController::CDBirthdayController(QObject *parent)
    : QObject(parent)
    , mCalendar(0)
    , mManager(0)
{
    // We don't need to handle presence changes, so report them separately and ignore them
    QMap<QString, QString> parameters;
    parameters.insert(QString::fromLatin1("mergePresenceChanges"), QString::fromLatin1("false"));

    mManager = new QContactManager(QStringLiteral("org.nemomobile.contacts.sqlite"), parameters, this);

#ifdef USING_QTPIM
    connect(mManager, SIGNAL(contactsAdded(QList<QContactId>)),
            SLOT(contactsChanged(QList<QContactId>)));
    connect(mManager, SIGNAL(contactsChanged(QList<QContactId>)),
            SLOT(contactsChanged(QList<QContactId>)));
    connect(mManager, SIGNAL(contactsRemoved(QList<QContactId>)),
            SLOT(contactsRemoved(QList<QContactId>)));
#else
    connect(mManager, SIGNAL(contactsAdded(QList<QContactLocalId>)),
            SLOT(contactsChanged(QList<QContactLocalId>)));
    connect(mManager, SIGNAL(contactsChanged(QList<QContactLocalId>)),
            SLOT(contactsChanged(QList<QContactLocalId>)));
    connect(mManager, SIGNAL(contactsRemoved(QList<QContactLocalId>)),
            SLOT(contactsRemoved(QList<QContactLocalId>)));
#endif

    connect(mManager, SIGNAL(dataChanged()), SLOT(updateAllBirthdays()));

    const CDBirthdayCalendar::SyncMode syncMode = stampFileUpToDate() ? CDBirthdayCalendar::KeepOldDB :
                                                                        CDBirthdayCalendar::DropOldDB;

    mCalendar = new CDBirthdayCalendar(syncMode, this);
    updateAllBirthdays();

    mUpdateTimer.setInterval(UPDATE_TIMEOUT);
    mUpdateTimer.setSingleShot(true);
    connect(&mUpdateTimer, SIGNAL(timeout()), SLOT(onUpdateQueueTimeout()));
}

CDBirthdayController::~CDBirthdayController()
{
}

void
CDBirthdayController::contactsChanged(const QList<ContactIdType>& contacts)
{
    foreach (const ContactIdType &id, contacts)
        mUpdatedContacts.insert(id);

    // Just restart the timer - if it doesn't expire, we can afford to wait
    mUpdateTimer.start();
}

void CDBirthdayController::contactsRemoved(const QList<ContactIdType>& contacts)
{
    foreach (const ContactIdType &id, contacts)
        mCalendar->deleteBirthday(id);
    mCalendar->save();
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Full sync logic
///////////////////////////////////////////////////////////////////////////////////////////////////

bool
CDBirthdayController::stampFileUpToDate()
{
    QFile cacheFile(stampFilePath());
    if (!cacheFile.exists()) {
        return false;
    }

    if (!cacheFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray content = cacheFile.read(100);
    bool ok;
    int value = content.toInt(&ok);
    return (ok && value == CURRENT_BIRTHDAY_VERSION);
}

void
CDBirthdayController::createStampFile()
{
    QFile cacheFile(stampFilePath());

    if (not cacheFile.open(QIODevice::WriteOnly)) {
        warning() << Q_FUNC_INFO << "Unable to create birthday plugin stamp file "
                  << cacheFile.fileName() << " with error " << cacheFile.errorString();
    }

    cacheFile.write(QByteArray::number(CURRENT_BIRTHDAY_VERSION));
}

QString
CDBirthdayController::stampFilePath() const
{
    return BasePlugin::cacheFileName(QLatin1String("calendar.stamp"));
}

void
CDBirthdayController::updateAllBirthdays()
{
    // Fetch any contact with a birthday.
    QContactDetailFilter fetchFilter(detailFilter<QContactBirthday>());
    fetchContacts(fetchFilter, SLOT(onFullSyncRequestStateChanged(QContactAbstractRequest::State)));
}

void
CDBirthdayController::onFullSyncRequestStateChanged(QContactAbstractRequest::State newState)
{
    if (processFetchRequest(qobject_cast<QContactFetchRequest*>(sender()), newState, FullSync)) {
        // Create the stamp file only after a successful full sync.
        createStampFile();
    }
}

void
CDBirthdayController::onUpdateQueueTimeout()
{
    QList<ContactIdType> contactIds(mUpdatedContacts.toList());

    // If we request too many contact IDs, we will exceed the SQLite bound variable limit
    const int batchSize = 200;
    if (contactIds.count() > batchSize) {
        mUpdatedContacts = contactIds.mid(batchSize).toSet();
        contactIds = contactIds.mid(0, batchSize);
    } else {
        mUpdatedContacts.clear();
    }

    fetchContacts(contactIds);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Incremental sync logic
///////////////////////////////////////////////////////////////////////////////////////////////////

void
CDBirthdayController::fetchContacts(const QList<ContactIdType> &contactIds)
{
#ifdef USING_QTPIM
    QContactIdFilter fetchFilter;
#else
    QContactLocalIdFilter fetchFilter;
#endif
    fetchFilter.setIds(contactIds);

    fetchContacts(fetchFilter, SLOT(onFetchRequestStateChanged(QContactAbstractRequest::State)));
}

void
CDBirthdayController::onFetchRequestStateChanged(QContactAbstractRequest::State newState)
{
    processFetchRequest(qobject_cast<QContactFetchRequest*>(sender()), newState, Incremental);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Common sync logic
///////////////////////////////////////////////////////////////////////////////////////////////////

void
CDBirthdayController::fetchContacts(const QContactFilter &filter, const char *slot)
{
    QContactFetchHint fetchHint;
#ifdef USING_QTPIM
    static const QList<QContactDetail::DetailType> detailTypes = QList<QContactDetail::DetailType>() 
        << QContactBirthday::Type
        << QContactDisplayLabel::Type;
    fetchHint.setDetailTypesHint(detailTypes);
#else
    static const QStringList detailDefinitions = QStringList() << QContactBirthday::DefinitionName
                                                               << QContactDisplayLabel::DefinitionName;
    fetchHint.setDetailDefinitionsHint(detailDefinitions);
#endif
    fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships |
                                   QContactFetchHint::NoActionPreferences |
                                   QContactFetchHint::NoBinaryBlobs);

    // Only fetch aggregate contacts
    QContactDetailFilter syncTargetFilter(detailFilter<QContactSyncTarget>(QContactSyncTarget::FieldSyncTarget));
    syncTargetFilter.setValue(QString::fromLatin1("aggregate"));

    QContactFetchRequest * const fetchRequest = new QContactFetchRequest(this);
    fetchRequest->setManager(mManager);
    fetchRequest->setFetchHint(fetchHint);
    fetchRequest->setFilter(filter & syncTargetFilter);

    connect(fetchRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)), slot);

    if (not fetchRequest->start()) {
        warning() << Q_FUNC_INFO << "Unable to start birthday contact fetch request";
        delete fetchRequest;
        return;
    }

    debug() << "Birthday contacts fetch request started";
}

bool
CDBirthdayController::processFetchRequest(QContactFetchRequest *const fetchRequest,
                                          QContactAbstractRequest::State newState,
                                          SyncMode syncMode)
{
    if (fetchRequest == 0) {
        warning() << Q_FUNC_INFO << "Invalid fetch request";
        return false;
    }

    bool success = false;

    switch (newState) {
    case QContactAbstractRequest::FinishedState:
        debug() << "Birthday contacts fetch request finished";

        if (fetchRequest->error() != QContactManager::NoError) {
            warning() << Q_FUNC_INFO << "Error during birthday contact fetch request, code: "
                      << fetchRequest->error();
        } else {
            const QList<QContact> contacts = fetchRequest->contacts();

            if (FullSync == syncMode) {
                syncBirthdays(contacts);
            } else {
                updateBirthdays(contacts);
            }
            success = true;
        }

        break;

    case QContactAbstractRequest::CanceledState:
        break;

    default:
        return false;
    }

    // Save the calendar in any case (success or not), since this "save" call
    // also applies for the deleteBirthday() calls in processNotificationQueues()
    mCalendar->save();

    // Provide hint we are done with this request.
    fetchRequest->deleteLater();

    // If some updated contacts weren't requested, we need to go again
    if (!mUpdatedContacts.isEmpty() && !mUpdateTimer.isActive()) {
        mUpdateTimer.start();
    }

    return success;
}

#ifdef USING_QTPIM
QContactId apiId(const QContact &contact) { return contact.id(); }
#else
QContactLocalId apiId(const QContact &contact) { return contact.localId(); }
#endif

void
CDBirthdayController::updateBirthdays(const QList<QContact> &changedBirthdays)
{
    foreach (const QContact &contact, changedBirthdays) {
        const QContactBirthday contactBirthday = contact.detail<QContactBirthday>();
#ifdef USING_QTPIM
        const QString contactDisplayLabel = contact.detail<QContactDisplayLabel>().label();
#else
        const QString contactDisplayLabel = contact.displayLabel();
#endif
        const CalendarBirthday calendarBirthday = mCalendar->birthday(apiId(contact));

        // Display label or birthdate was removed from the contact, so delete it from the calendar.
        if (contactDisplayLabel.isEmpty() || contactBirthday.date().isNull()) {
            if (!calendarBirthday.date().isNull()) {
                debug() << "Contact: " << apiId(contact) << " removed birthday or displayLabel, so delete the calendar event";

                mCalendar->deleteBirthday(apiId(contact));
            }
        // Display label or birthdate was changed on the contact, so update the calendar.
        } else if ((contactDisplayLabel != calendarBirthday.summary()) ||
                   (contactBirthday.date() != calendarBirthday.date())) {
            debug() << "Contact with calendar birthday: " << calendarBirthday.date()
                    << " and calendar displayLabel: " << calendarBirthday.summary()
                    << " changed details to: " << contactBirthday.date() << contactDisplayLabel
                    << ", so update the calendar event";

            mCalendar->updateBirthday(contact);
        }
    }
}

void
CDBirthdayController::syncBirthdays(const QList<QContact> &birthdayContacts)
{
    QHash<ContactIdType, CalendarBirthday> oldBirthdays = mCalendar->birthdays();

    // Check all birthdays from the contacts if the stored calendar item is up-to-date
    foreach (const QContact &contact, birthdayContacts) {
#ifdef USING_QTPIM
        const QString contactDisplayLabel = contact.detail<QContactDisplayLabel>().label();
#else
        const QString contactDisplayLabel = contact.displayLabel();
#endif

        if (contactDisplayLabel.isNull()) {
            debug() << "Contact: " << contact << " has no displayLabel, so not syncing to calendar";
            continue;
        }

        QHash<ContactIdType, CalendarBirthday>::Iterator it = oldBirthdays.find(apiId(contact));

        if (oldBirthdays.end() != it) {
            const QContactBirthday contactBirthday = contact.detail<QContactBirthday>();
            const CalendarBirthday &calendarBirthday = *it;

            // Display label or birthdate was changed on the contact, so update the calendar.
            if ((contactDisplayLabel != calendarBirthday.summary()) ||
                (contactBirthday.date() != calendarBirthday.date())) {
                debug() << "Contact with calendar birthday: " << contactBirthday.date()
                        << " and calendar displayLabel: " << calendarBirthday.summary()
                        << " changed details to: " << contact << ", so update the calendar event";

                mCalendar->updateBirthday(contact);
            }

            // Birthday exists, so not a garbage one
            oldBirthdays.erase(it);
        } else {
            // Create new birthday
            mCalendar->updateBirthday(contact);
        }
    }

    // Remaining old birthdays in the calendar db do not did not match any contact, so remove them.
    foreach (const ContactIdType &id, oldBirthdays.keys()) {
        debug() << "Birthday with contact id" << id << "no longer has a matching contact, trashing it";
        mCalendar->deleteBirthday(id);
    }
}
