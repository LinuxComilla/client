/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QtGui>
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QtWidgets>
#endif

#include "activitywidget.h"
#include "configfile.h"
#include "syncresult.h"
#include "logger.h"
#include "utility.h"
#include "theme.h"
#include "folderman.h"
#include "syncfileitem.h"
#include "folder.h"
#include "openfilemanager.h"
#include "owncloudpropagator.h"
#include "account.h"
#include "accountstate.h"
#include "accountmanager.h"
#include "activityitemdelegate.h"
#include "protocolwidget.h"
#include "QProgressIndicator.h"

#include "ui_activitywidget.h"

#include <climits>

namespace OCC {

void ActivityList::setAccountName( const QString& name )
{
    _accountName = name;
}

QString ActivityList::accountName() const
{
    return _accountName;
}

/* ==================================================================== */

ActivityListModel::ActivityListModel(QWidget *parent)
    :QAbstractListModel(parent)
{
}


Activity ActivityListModel::findItem(int indx) const
{
    Activity a;

    foreach( ActivityList al, _activityLists ) {
        if( indx < al.count() ) {
            a = al.at(indx);
            break;
        }
        indx -= al.count();
    }

    return a;
}

QVariant ActivityListModel::data(const QModelIndex &index, int role) const
{
    Activity a;

    if (!index.isValid())
        return QVariant();

    a = findItem(index.row());

    AccountStatePtr ast = AccountManager::instance()->account(a._accName);
    QStringList list;

    if (role == Qt::EditRole)
        return QVariant();

    switch (role) {
    case ActivityItemDelegate::PathRole:
        list = FolderMan::instance()->findFileInLocalFolders(a._file, ast->account());
        if( list.count() > 0 ) {
            return QVariant(list.at(0));
        }
        // File does not exist anymore? Let's try to open its path
        list = FolderMan::instance()->findFileInLocalFolders(QFileInfo(a._file).path(), ast->account());
        if( list.count() > 0 ) {
            return QVariant(list.at(0));
        }
        return QVariant();
        break;
    case ActivityItemDelegate::ActionIconRole:
        return QVariant(); // FIXME once the action can be quantified, display on Icon
        break;
    case ActivityItemDelegate::UserIconRole:
        return QIcon(QLatin1String(":/client/resources/account.png"));
        break;
    case Qt::ToolTipRole:
    case ActivityItemDelegate::ActionTextRole:
        return a._subject;
        break;
    case ActivityItemDelegate::LinkRole:
        return a._link;
        break;
    case ActivityItemDelegate::AccountRole:
        return a._accName;
        break;
    case ActivityItemDelegate::PointInTimeRole:
        return Utility::timeAgoInWords(a._dateTime);
        break;
    case ActivityItemDelegate::AccountConnectedRole:
        return (ast && ast->isConnected());
        break;
    default:
        return QVariant();

    }
    return QVariant();

}

int ActivityListModel::rowCount(const QModelIndex&) const
{
    int cnt = 0;

    foreach( ActivityList al, _activityLists) {
        cnt += al.count();
    }
    return cnt;
}

// current strategy: Fetch 100 items per Account
bool ActivityListModel::canFetchMore(const QModelIndex& ) const
{
    // if there are no activitylists registered yet, always
    // let fetch more.
    if( _activityLists.size() == 0 &&
            AccountManager::instance()->accounts().count() > 0 ) {
        return true;
    }

    int readyToFetch = 0;
    foreach( ActivityList list, _activityLists) {
        AccountStatePtr ast = AccountManager::instance()->account(list.accountName());

        if( ast && ast->isConnected()
                && list.count() == 0
                && ! _currentlyFetching.contains(ast.data()) ) {
            readyToFetch++;
        }
    }

    return readyToFetch > 0;
}

void ActivityListModel::startFetchJob(AccountState* s)
{
    if( !s->isConnected() ) {
        return;
    }
    JsonApiJob *job = new JsonApiJob(s->account(), QLatin1String("ocs/v1.php/cloud/activity"), this);
    QObject::connect(job, SIGNAL(jsonReceived(QVariantMap, int)),
                     this, SLOT(slotActivitiesReceived(QVariantMap, int)));
    job->setProperty("AccountStatePtr", QVariant::fromValue<AccountState*>(s));

    QList< QPair<QString,QString> > params;
    params.append(qMakePair(QString::fromLatin1("page"),     QString::fromLatin1("0")));
    params.append(qMakePair(QString::fromLatin1("pagesize"), QString::fromLatin1("100")));
    job->addQueryParams(params);

    _currentlyFetching.insert(s);
    qDebug() << "Start fetching activities for " << s->account()->displayName();
    job->start();
}

void ActivityListModel::slotActivitiesReceived(const QVariantMap& json, int statusCode)
{
    auto activities = json.value("ocs").toMap().value("data").toList();
    qDebug() << "*** activities" << activities;

    ActivityList list;
    AccountState* ai = qvariant_cast<AccountState*>(sender()->property("AccountStatePtr"));
    _currentlyFetching.remove(ai);
    list.setAccountName( ai->account()->displayName());

    foreach( auto activ, activities ) {
        auto json = activ.toMap();

        Activity a;
        a._accName  = ai->account()->displayName();
        a._id       = json.value("id").toLongLong();
        a._subject  = json.value("subject").toString();
        a._message  = json.value("message").toString();
        a._file     = json.value("file").toString();
        a._link     = json.value("link").toUrl();
        a._dateTime = json.value("date").toDateTime();
        list.append(a);
    }
    // activity app is not enabled, signalling.
    if( statusCode == 999 ) {
        emit accountWithoutActivityApp(ai);
    }

    addNewActivities(list);

}

void ActivityListModel::addNewActivities(const ActivityList& list)
{
    int startItem = 0; // the start number of items to delete in the virtual overall list
    int listIdx = 0;   // the index of the list that is to replace.

    // check the existing list of activity lists if the incoming account
    // is already in there.
    foreach( ActivityList oldList, _activityLists ) {
        if( oldList.accountName() == list.accountName() ) {
            // listIndx contains the array index of the list and startItem
            // the start item of the virtual overall list.
            break;
        }
        startItem += oldList.count(); // add the number of items in the list
        listIdx++;
    }

    // if the activity list for this account was already known, remove its
    // entry first and than insert the new list.
    if( listIdx < _activityLists.count()-1 ) {
        int removeItemCount = _activityLists.at(listIdx).count();
        beginRemoveRows(QModelIndex(), startItem, removeItemCount);
        _activityLists.value(listIdx).clear();
        endRemoveRows();
    }

    // insert the new list
    beginInsertRows(QModelIndex(), startItem, list.count() );
    if( listIdx == _activityLists.count() ) {
        // not yet in the list of activity lists
        _activityLists.append(list);
    } else {
        _activityLists[listIdx] = list;
    }
    endInsertRows();
}

void ActivityListModel::fetchMore(const QModelIndex &)
{
    QList<AccountStatePtr> accounts = AccountManager::instance()->accounts();

    foreach ( AccountStatePtr ast, accounts) {
        // For each account from the account manager, check if it has already
        // an entry in the models list, if not, add one and call the fetch
        // job.
        bool found = false;
        foreach (ActivityList list, _activityLists) {
            if( AccountManager::instance()->account(list.accountName())==ast.data()) {
                found = true;
                break;
            }
        }

        if( !found ) {
            // add new list to the activity lists
            ActivityList alist;
            alist.setAccountName(ast->account()->displayName());
            _activityLists.append(alist);
            startFetchJob(ast.data());
        }
    }
}

void ActivityListModel::slotRefreshActivity(AccountState *ast)
{
    if(ast ) {
        qDebug() << "**** Refreshing Activity list for" << ast->account()->displayName();
        startFetchJob(ast);
    }
}

void ActivityListModel::slotRemoveAccount(AccountState *ast )
{
    int i;
    int removeIndx = -1;
    int startRow = 0;
    for( i = 0; i < _activityLists.count(); i++) {
        ActivityList al = _activityLists.at(i);
        if( al.accountName() == ast->account()->displayName() ) {
            removeIndx = i;
            break;
        }
        startRow += al.count();
    }

    if( removeIndx > -1 ) {
        beginRemoveRows(QModelIndex(), startRow, startRow+_activityLists.at(removeIndx).count());
        _activityLists.removeAt(removeIndx);
        endRemoveRows();
        _currentlyFetching.remove(ast);
    }
}

// combine all activities into one big result list
ActivityList ActivityListModel::activityList()
{
    ActivityList all;
    int i;

    for( i = 0; i < _activityLists.count(); i++) {
        ActivityList al = _activityLists.at(i);
        all.append(al);
    }
    return all;
}

/* ==================================================================== */
ActivitySortProxyModel::ActivitySortProxyModel(QObject *parent)
    :QSortFilterProxyModel(parent)
{

}

bool ActivitySortProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    QVariant leftData = sourceModel()->data(left);
    QVariant rightData = sourceModel()->data(right);

    if (leftData.type() == QVariant::DateTime) {
        return leftData.toDateTime() < rightData.toDateTime();
    } else {
        qDebug() << "OOOOO " << endl;
    }
    return true;
}

/* ==================================================================== */

ActivityWidget::ActivityWidget(QWidget *parent) :
    QWidget(parent),
    _ui(new Ui::ActivityWidget)
{
    _ui->setupUi(this);

    // Adjust copyToClipboard() when making changes here!
#if defined(Q_OS_MAC)
    _ui->_activityList->setMinimumWidth(400);
#endif


    _model = new ActivitySortProxyModel(this);
    _model->setSourceModel(new ActivityListModel);

    ActivityItemDelegate *delegate = new ActivityItemDelegate;
    delegate->setParent(this);
    _ui->_activityList->setItemDelegate(delegate);
    _ui->_activityList->setAlternatingRowColors(true);
    _ui->_activityList->setModel(_model);

    showLabels();

    connect(_model, SIGNAL(accountWithoutActivityApp(AccountState*)),
            this, SLOT(slotAccountWithoutActivityApp(AccountState*)));

    _copyBtn = _ui->_dialogButtonBox->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    _copyBtn->setToolTip( tr("Copy the activity list to the clipboard."));
    connect(_copyBtn, SIGNAL(clicked()), SIGNAL(copyToClipboard()));

    connect(_model, SIGNAL(rowsInserted(QModelIndex,int,int)), SIGNAL(rowsInserted()));

    connect( _ui->_activityList, SIGNAL(activated(QModelIndex)), this,
             SLOT(slotOpenFile(QModelIndex)));
}

ActivityWidget::~ActivityWidget()
{
    delete _ui;
}

void ActivityWidget::slotRefresh(AccountState *ptr)
{
    qobject_cast<ActivityListModel*>(_model->sourceModel())->slotRefreshActivity(ptr);
}

void ActivityWidget::slotRemoveAccount( AccountState *ptr )
{
    qobject_cast<ActivityListModel*>(_model->sourceModel())->slotRemoveAccount(ptr);
}

void ActivityWidget::showLabels()
{
    QString t = tr("Server Activities");
    _ui->_headerLabel->setTextFormat(Qt::RichText);
    _ui->_headerLabel->setText(t);

    t.clear();
    QSetIterator<QString> i(_accountsWithoutActivities);
    while (i.hasNext() ) {
        t.append( tr("<br/>Account %1 does not have activities enabled.").arg(i.next()));
    }
    _ui->_bottomLabel->setTextFormat(Qt::RichText);
    _ui->_bottomLabel->setText(t);
}

void ActivityWidget::slotAccountWithoutActivityApp(AccountState *ast)
{
    if( ast && ast->account() ) {
        _accountsWithoutActivities.insert(ast->account()->displayName());
    }

    showLabels();
}

// FIXME: Reused from protocol widget. Move over to utilities.
QString ActivityWidget::timeString(QDateTime dt, QLocale::FormatType format) const
{
    const QLocale loc = QLocale::system();
    QString dtFormat = loc.dateTimeFormat(format);
    static const QRegExp re("(HH|H|hh|h):mm(?!:s)");
    dtFormat.replace(re, "\\1:mm:ss");
    return loc.toString(dt, dtFormat);
}

void ActivityWidget::storeActivityList( QTextStream& ts )
{
    ActivityList activities = qobject_cast<ActivityListModel*>(_model->sourceModel())->activityList();

    foreach( Activity activity, activities ) {
        ts << left
              // account name
           << qSetFieldWidth(30)
           << activity._accName
              // date and time
           << qSetFieldWidth(34)
           << activity._dateTime.toString()
              // subject
           << qSetFieldWidth(10)
           << activity._subject
              // file
           << qSetFieldWidth(30)
           << activity._file
              // message (mostly empty)
           << qSetFieldWidth(55)
           << activity._message
              //
           << qSetFieldWidth(0)
           << endl;
    }
}

void ActivityWidget::slotOpenFile(QModelIndex indx)
{
    qDebug() << indx.isValid() << indx.data(ActivityItemDelegate::PathRole).toString() << QFile::exists(indx.data(ActivityItemDelegate::PathRole).toString());
    if( indx.isValid() ) {
        QString fullPath = indx.data(ActivityItemDelegate::PathRole).toString();

        if (QFile::exists(fullPath)) {
            showInFileManager(fullPath);
        }
    }
}

/* ==================================================================== */

ActivitySettings::ActivitySettings(QWidget *parent)
    :QWidget(parent)
{
    QHBoxLayout *hbox = new QHBoxLayout(this);
    setLayout(hbox);

    // create a tab widget for the three activity views
    _tab = new QTabWidget(this);
    hbox->addWidget(_tab);
    _activityWidget = new ActivityWidget(this);
    _tab->addTab(_activityWidget, Theme::instance()->applicationIcon(), tr("Server Activity"));
    connect(_activityWidget, SIGNAL(copyToClipboard()), this, SLOT(slotCopyToClipboard()));


    _protocolWidget = new ProtocolWidget(this);
    _tab->addTab(_protocolWidget, Theme::instance()->syncStateIcon(SyncResult::Success), tr("Sync Protocol"));
    connect(_protocolWidget, SIGNAL(copyToClipboard()), this, SLOT(slotCopyToClipboard()));

    // Add the not-synced list into the tab
    QWidget *w = new QWidget;
    QVBoxLayout *vbox2 = new QVBoxLayout(w);
    vbox2->addWidget(new QLabel(tr("List of ignored or erroneous files"), this));
    vbox2->addWidget(_protocolWidget->issueWidget());
    QDialogButtonBox *dlgButtonBox = new QDialogButtonBox(this);
    vbox2->addWidget(dlgButtonBox);
    QPushButton *_copyBtn = dlgButtonBox->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    _copyBtn->setToolTip( tr("Copy the activity list to the clipboard."));
    _copyBtn->setEnabled(true);
    connect(_copyBtn, SIGNAL(clicked()), this, SLOT(slotCopyToClipboard()));

    w->setLayout(vbox2);
    _tab->addTab(w, Theme::instance()->syncStateIcon(SyncResult::Problem), tr("Not Synced"));

    // Add a progress indicator to spin if the acitivity list is updated.
    _progressIndicator = new QProgressIndicator(this);
    _tab->setCornerWidget(_progressIndicator);

    // connect a model signal to stop the animation.
    connect(_activityWidget, SIGNAL(rowsInserted()), _progressIndicator, SLOT(stopAnimation()));
}

void ActivitySettings::slotCopyToClipboard()
{
    QString text;
    QTextStream ts(&text);

    int idx = _tab->currentIndex();
    QString theSubject;

    if( idx == 0 ) {
        // the activity widget
        _activityWidget->storeActivityList(ts);
        theSubject = tr("server activity list");
    } else if(idx == 1 ) {
        // the protocol widget
        _protocolWidget->storeSyncActivity(ts);
        theSubject = tr("sync activity list");
    } else if(idx == 2 ) {
        // issues Widget
        theSubject = tr("not syned items list");
       _protocolWidget->storeSyncIssues(ts);
    }

    QApplication::clipboard()->setText(text);
    emit guiLog(tr("Copied to clipboard"), tr("The %1 has been copied to the clipboard.").arg(theSubject));
}

void ActivitySettings::slotRemoveAccount( AccountState *ptr )
{
    _activityWidget->slotRemoveAccount(ptr);
}

void ActivitySettings::slotRefresh( AccountState* ptr )
{
    if( ptr && ptr->isConnected() && isVisible()) {
        qDebug() << "Refreshing Activity list for " << ptr->account()->displayName();
        _progressIndicator->startAnimation();
        _activityWidget->slotRefresh(ptr);
    }
}

bool ActivitySettings::event(QEvent* e)
{
    if (e->type() == QEvent::Show) {
        AccountManager *am = AccountManager::instance();
        foreach (AccountStatePtr a, am->accounts()) {
            slotRefresh(a.data());
        }
    }
    return QWidget::event(e);
}

ActivitySettings::~ActivitySettings()
{

}


}
