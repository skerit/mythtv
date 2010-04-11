// POSIX headers
#include <sys/time.h>     // for setpriority
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>

#include "mythconfig.h"
#if CONFIG_DARWIN
    #include <sys/aio.h>    // O_SYNC
#endif

// C headers
#include <cstdlib>
#include <cerrno>

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegExp>
#include <QFile>
#include <QDir>
#include <QMap>

#include "tv_rec.h"
#include "scheduledrecording.h"
#include "autoexpire.h"
#include "scheduler.h"
#include "mainserver.h"
#include "encoderlink.h"
#include "remoteutil.h"
#include "housekeeper.h"

#include "mythcontext.h"
#include "mythverbose.h"
#include "mythversion.h"
#include "mythdb.h"
#include "exitcodes.h"
#include "compat.h"
#include "storagegroup.h"
#include "programinfo.h"
#include "dbcheck.h"
#include "jobqueue.h"
#include "previewgenerator.h"
#include "mythcommandlineparser.h"
#include "mythsystemevent.h"
#include "main_helpers.h"
#include "backendcontext.h"

#include "mediaserver.h"
#include "httpstatus.h"

#define LOC      QString("MythBackend: ")
#define LOC_WARN QString("MythBackend, Warning: ")
#define LOC_ERR  QString("MythBackend, Error: ")

bool setupTVs(bool ismaster, bool &error)
{
    error = false;
    QString localhostname = gContext->GetHostName();

    MSqlQuery query(MSqlQuery::InitCon());

    if (ismaster)
    {
        // Hack to make sure recorded.basename gets set if the user
        // downgrades to a prior version and creates new entries
        // without it.
        if (!query.exec("UPDATE recorded SET basename = CONCAT(chanid, '_', "
                        "DATE_FORMAT(starttime, '%Y%m%d%H%i00'), '_', "
                        "DATE_FORMAT(endtime, '%Y%m%d%H%i00'), '.nuv') "
                        "WHERE basename = '';"))
            MythDB::DBError("Updating record basename",
                                 query.lastQuery());

        // Hack to make sure record.station gets set if the user
        // downgrades to a prior version and creates new entries
        // without it.
        if (!query.exec("UPDATE channel SET callsign=chanid "
                        "WHERE callsign IS NULL OR callsign='';"))
            MythDB::DBError("Updating channel callsign", query.lastQuery());

        if (query.exec("SELECT MIN(chanid) FROM channel;"))
        {
            query.first();
            int min_chanid = query.value(0).toInt();
            if (!query.exec(QString("UPDATE record SET chanid = %1 "
                                    "WHERE chanid IS NULL;").arg(min_chanid)))
                MythDB::DBError("Updating record chanid", query.lastQuery());
        }
        else
            MythDB::DBError("Querying minimum chanid", query.lastQuery());

        MSqlQuery records_without_station(MSqlQuery::InitCon());
        records_without_station.prepare("SELECT record.chanid,"
                " channel.callsign FROM record LEFT JOIN channel"
                " ON record.chanid = channel.chanid WHERE record.station='';");
        if (records_without_station.exec() && records_without_station.next())
        {
            MSqlQuery update_record(MSqlQuery::InitCon());
            update_record.prepare("UPDATE record SET station = :CALLSIGN"
                    " WHERE chanid = :CHANID;");
            do
            {
                update_record.bindValue(":CALLSIGN",
                        records_without_station.value(1));
                update_record.bindValue(":CHANID",
                        records_without_station.value(0));
                if (!update_record.exec())
                {
                    MythDB::DBError("Updating record station",
                            update_record.lastQuery());
                }
            } while (records_without_station.next());
        }
    }

    if (!query.exec(
            "SELECT cardid, hostname "
            "FROM capturecard "
            "ORDER BY cardid"))
    {
        MythDB::DBError("Querying Recorders", query);
        return false;
    }

    vector<uint>    cardids;
    vector<QString> hosts;
    while (query.next())
    {
        uint    cardid = query.value(0).toUInt();
        QString host   = query.value(1).toString();
        QString cidmsg = QString("Card %1").arg(cardid);

        if (host.isEmpty())
        {
            QString msg = cidmsg + " does not have a hostname defined.\n"
                "Please run setup and confirm all of the capture cards.\n";

            VERBOSE(VB_IMPORTANT, msg);
            gContext->LogEntry("mythbackend", LP_CRITICAL,
                               "Problem with capture cards", msg);
            continue;
        }

        cardids.push_back(cardid);
        hosts.push_back(host);
    }

    for (uint i = 0; i < cardids.size(); i++)
    {
        if (hosts[i] == localhostname)
            new TVRec(cardids[i]);
    }

    for (uint i = 0; i < cardids.size(); i++)
    {
        uint    cardid = cardids[i];
        QString host   = hosts[i];
        QString cidmsg = QString("Card %1").arg(cardid);

        if (!ismaster)
        {
            if (host == localhostname)
            {
                TVRec *tv = TVRec::GetTVRec(cardid);
                if (tv->Init())
                {
                    EncoderLink *enc = new EncoderLink(cardid, tv);
                    tvList[cardid] = enc;
                }
                else
                {
                    gContext->LogEntry("mythbackend", LP_CRITICAL,
                                       "Problem with capture cards",
                                       cidmsg + " failed init");
                    delete tv;
                    // The master assumes card comes up so we need to
                    // set error and exit if a non-master card fails.
                    error = true;
                }
            }
        }
        else
        {
            if (host == localhostname)
            {
                TVRec *tv = TVRec::GetTVRec(cardid);
                if (tv->Init())
                {
                    EncoderLink *enc = new EncoderLink(cardid, tv);
                    tvList[cardid] = enc;
                }
                else
                {
                    gContext->LogEntry("mythbackend", LP_CRITICAL,
                                       "Problem with capture cards",
                                       cidmsg + "failed init");
                    delete tv;
                }
            }
            else
            {
                EncoderLink *enc = new EncoderLink(cardid, NULL, host);
                tvList[cardid] = enc;
            }
        }
    }

    if (tvList.empty())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "No valid capture cards are defined in the database.\n\t\t\t"
                "Perhaps you should re-read the installation instructions?");

        gContext->LogEntry("mythbackend", LP_CRITICAL,
                           "No capture cards are defined",
                           "Please run the setup program.");
        return false;
    }

    return true;
}

bool setup_context(const MythCommandLineParser &cmdline)
{
    if (!gContext->Init(false))
    {
        VERBOSE(VB_IMPORTANT, "Failed to init MythContext.");
        delete gContext;
        gContext = NULL;
        return false;
    }
    gContext->SetBackend(!cmdline.HasBackendCommand());

    QMap<QString,QString> settingsOverride = cmdline.GetSettingsOverride();
    if (settingsOverride.size())
    {
        QMap<QString, QString>::iterator it;
        for (it = settingsOverride.begin(); it != settingsOverride.end(); ++it)
        {
            VERBOSE(VB_IMPORTANT, QString("Setting '%1' being forced to '%2'")
                    .arg(it.key()).arg(*it));
            gContext->OverrideSettingForSession(it.key(), *it);
        }
    }

    return true;
}

void cleanup(void)
{
    delete sched;
    sched = NULL;

    delete g_pUPnp;
    g_pUPnp = NULL;

    delete gContext;
    gContext = NULL;

    if (pidfile.size())
    {
        unlink(pidfile.toAscii().constData());
        pidfile.clear();
    }

    signal(SIGHUP, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
}

int log_rotate(int report_error)
{
    /* http://www.gossamer-threads.com/lists/mythtv/dev/110113 */

    int new_logfd = open(logfile.toLocal8Bit().constData(),
                         O_WRONLY|O_CREAT|O_APPEND|O_SYNC, 0664);
    if (new_logfd < 0)
    {
        // If we can't open the new logfile, send data to /dev/null
        if (report_error)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    QString("Cannot open logfile '%1'").arg(logfile));
            return -1;
        }
        new_logfd = open("/dev/null", O_WRONLY);
        if (new_logfd < 0)
        {
            // There's not much we can do, so punt.
            return -1;
        }
    }
    while (dup2(new_logfd, 1) < 0 && errno == EINTR) ;
    while (dup2(new_logfd, 2) < 0 && errno == EINTR) ;
    while (close(new_logfd) < 0 && errno == EINTR) ;
    return 0;
}

void log_rotate_handler(int)
{
    log_rotate(0);
}

void upnp_rebuild(int)
{
    if (gContext->IsMasterHost())
    {
        g_pUPnp->RebuildMediaMap();
    }

}

int preview_helper(const QString &chanid, const QString &starttime,
                   long long previewFrameNumber, long long previewSeconds,
                   const QSize &previewSize,
                   const QString &infile, const QString &outfile)
{
    // Lower scheduling priority, to avoid problems with recordings.
    if (setpriority(PRIO_PROCESS, 0, 9))
        VERBOSE(VB_GENERAL, "Setting priority failed." + ENO);

    ProgramInfo *pginfo = NULL;
    if (!chanid.isEmpty() && !starttime.isEmpty())
    {
        pginfo = ProgramInfo::GetProgramFromRecorded(chanid, starttime);
        if (!pginfo)
        {
            VERBOSE(VB_IMPORTANT, QString(
                        "Can not locate recording made on '%1' at '%2'")
                    .arg(chanid).arg(starttime));
            return GENERIC_EXIT_NOT_OK;
        }
        pginfo->pathname = pginfo->GetPlaybackURL(false, true);
    }
    else if (!infile.isEmpty())
    {
        pginfo = ProgramInfo::GetProgramFromBasename(infile);
        if (!pginfo)
        {
            if (!QFileInfo(infile).exists())
            {
                VERBOSE(VB_IMPORTANT, QString(
                            "Can not locate recording '%1'").arg(infile));
                return GENERIC_EXIT_NOT_OK;
            }
            else
            {
                pginfo = new ProgramInfo();
                pginfo->isVideo = true;

                QDir d(infile + "/VIDEO_TS");
                if ((infile.section('.', -1) == "iso") ||
                    (infile.section('.', -1) == "img") ||
                    d.exists())
                {
                    pginfo->pathname = QString("dvd:%1").arg(infile);
                }
                else
                {
                    pginfo->pathname = QFileInfo(infile).absoluteFilePath();
                }
            }

        }
        else
        {
            pginfo->pathname = pginfo->GetPlaybackURL(false, true);
        }
    }
    else
    {
        VERBOSE(VB_IMPORTANT, "Can not locate recording for preview");
        return GENERIC_EXIT_NOT_OK;
    }

    PreviewGenerator *previewgen = new PreviewGenerator(
        pginfo, PreviewGenerator::kLocal);

    if (previewFrameNumber >= 0)
        previewgen->SetPreviewTimeAsFrameNumber(previewFrameNumber);

    if (previewSeconds >= 0)
        previewgen->SetPreviewTimeAsSeconds(previewSeconds);

    previewgen->SetOutputSize(previewSize);
    previewgen->SetOutputFilename(outfile);
    bool ok = previewgen->RunReal();
    previewgen->deleteLater();

    delete pginfo;

    return (ok) ? GENERIC_EXIT_OK : GENERIC_EXIT_NOT_OK;
}

void showUsage(const MythCommandLineParser &cmdlineparser, const QString &version)
{
    QString    help  = cmdlineparser.GetHelpString(false);
    QByteArray ahelp = help.toLocal8Bit();

    cerr << qPrintable(version) << endl <<
    "Valid options are: " << endl <<
    "-h or --help                   List valid command line parameters"
         << endl << ahelp.constData() << endl;
}

void setupLogfile(void)
{
    if (!logfile.isEmpty())
    {
        if (log_rotate(1) < 0)
        {
            VERBOSE(VB_IMPORTANT, LOC_WARN +
                    "Cannot open logfile; using stdout/stderr instead");
        }
        else
            signal(SIGHUP, &log_rotate_handler);
    }
}

bool openPidfile(ofstream &pidfs, const QString &pidfile)
{
    if (!pidfile.isEmpty())
    {
        pidfs.open(pidfile.toAscii().constData());
        if (!pidfs)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    "Could not open pid file" + ENO);
            return false;
        }
    }
    return true;
}

bool setUser(const QString &username)
{
    if (username.isEmpty())
        return true;

    struct passwd *user_info = getpwnam(username.toLocal8Bit().constData());
    const uid_t user_id = geteuid();

    if (user_id && (!user_info || user_id != user_info->pw_uid))
    {
        VERBOSE(VB_IMPORTANT,
                "You must be running as root to use the --user switch.");
        return false;
    }
    else if (user_info && user_id == user_info->pw_uid)
    {
        VERBOSE(VB_IMPORTANT,
                QString("Already running as '%1'").arg(username));
    }
    else if (!user_id && user_info)
    {
        if (setenv("HOME", user_info->pw_dir,1) == -1)
        {
            VERBOSE(VB_IMPORTANT, "Error setting home directory.");
            return false;
        }
        if (setgid(user_info->pw_gid) == -1)
        {
            VERBOSE(VB_IMPORTANT, "Error setting effective group.");
            return false;
        }
        if (initgroups(user_info->pw_name, user_info->pw_gid) == -1)
        {
            VERBOSE(VB_IMPORTANT, "Error setting groups.");
            return false;
        }
        if (setuid(user_info->pw_uid) == -1)
        {
            VERBOSE(VB_IMPORTANT, "Error setting effective user.");
            return false;
        }
    }
    else
    {
        VERBOSE(VB_IMPORTANT,
                QString("Invalid user '%1' specified with --user")
                .arg(username));
        return false;
    }
    return true;
}

int handle_command(const MythCommandLineParser &cmdline)
{
    QString eventString = cmdline.GetEventString();
    if (!eventString.isEmpty())
    {
        if (gContext->ConnectToMasterServer())
        {
            if (eventString.startsWith("SYSTEM_EVENT"))
            {
                eventString += QString(" SENDER %1")
                    .arg(gContext->GetHostName());
            }

            RemoteSendMessage(eventString);
            return BACKEND_EXIT_OK;
        }
        return BACKEND_EXIT_NO_MYTHCONTEXT;
    }

    if (cmdline.WantUPnPRebuild())
    {
        VERBOSE(VB_GENERAL, "Rebuilding UPNP Media Map");

        UPnpMedia *rebuildit = new UPnpMedia(false,false);
        rebuildit->BuildMediaMap();

        return BACKEND_EXIT_OK;
    }

    if (cmdline.SetVerbose())
    {
        if (gContext->ConnectToMasterServer())
        {
            QString message = "SET_VERBOSE ";
            message += cmdline.GetNewVerbose();

            RemoteSendMessage(message);
            VERBOSE(VB_IMPORTANT, QString("Sent '%1' message").arg(message));
            return BACKEND_EXIT_OK;
        }
        else
        {
            VERBOSE(VB_IMPORTANT,
                    "Unable to connect to backend, verbose level unchanged ");
            return BACKEND_EXIT_NO_CONNECT;
        }
    }

    if (cmdline.ClearSettingsCache())
    {
        if (gContext->ConnectToMasterServer())
        {
            RemoteSendMessage("CLEAR_SETTINGS_CACHE");
            VERBOSE(VB_IMPORTANT, "Sent CLEAR_SETTINGS_CACHE message");
            return BACKEND_EXIT_OK;
        }
        else
        {
            VERBOSE(VB_IMPORTANT, "Unable to connect to backend, settings "
                    "cache will not be cleared.");
            return BACKEND_EXIT_NO_CONNECT;
        }
    }

    if (cmdline.IsPrintScheduleEnabled() ||
        cmdline.IsTestSchedulerEnabled())
    {
        sched = new Scheduler(false, &tvList);
        if (!cmdline.IsTestSchedulerEnabled() &&
            gContext->ConnectToMasterServer())
        {
            cout << "Retrieving Schedule from Master backend.\n";
            sched->FillRecordListFromMaster();
        }
        else
        {
            cout << "Calculating Schedule from database.\n" <<
                    "Inputs, Card IDs, and Conflict info may be invalid "
                    "if you have multiple tuners.\n";
            sched->FillRecordListFromDB();
        }

        print_verbose_messages |= VB_SCHEDULE;
        sched->PrintList(true);
        return BACKEND_EXIT_OK;
    }

    if (cmdline.Reschedule())
    {
        bool ok = false;
        if (gContext->ConnectToMasterServer())
        {
            VERBOSE(VB_IMPORTANT, "Connected to master for reschedule");
            ScheduledRecording::signalChange(-1);
            ok = true;
        }
        else
            VERBOSE(VB_IMPORTANT, "Cannot connect to master for reschedule");

        return (ok) ? BACKEND_EXIT_OK : BACKEND_EXIT_NO_CONNECT;
    }

    if (!cmdline.GetPrintExpire().isEmpty())
    {
        expirer = new AutoExpire();
        expirer->PrintExpireList(cmdline.GetPrintExpire());
        return BACKEND_EXIT_OK;
    }

    if ((cmdline.GetPreviewFrameNumber() >= -1) ||
        (cmdline.GetPreviewSeconds() >= -1))
    {
        int ret = preview_helper(
            QString::number(cmdline.GetChanID()),
            cmdline.GetStartTime().toString(Qt::ISODate),
            cmdline.GetPreviewFrameNumber(), cmdline.GetPreviewSeconds(),
            cmdline.GetPreviewSize(),
            cmdline.GetInputFilename(), cmdline.GetOutputFilename());
        return ret;
    }

    // This should never actually be reached..
    return BACKEND_EXIT_OK;
}

int connect_to_master(void)
{
    MythSocket *tempMonitorConnection = new MythSocket();
    if (tempMonitorConnection->connect(
            gContext->GetSetting("MasterServerIP", "127.0.0.1"),
            gContext->GetNumSetting("MasterServerPort", 6543)))
    {
        if (!gContext->CheckProtoVersion(tempMonitorConnection))
        {
            VERBOSE(VB_IMPORTANT, "Master backend is incompatible with "
                    "this backend.\nCannot become a slave.");
            return BACKEND_EXIT_NO_CONNECT;
        }

        QStringList tempMonitorDone("DONE");

        QStringList tempMonitorAnnounce("ANN Monitor tzcheck 0");
        tempMonitorConnection->writeStringList(tempMonitorAnnounce);
        tempMonitorConnection->readStringList(tempMonitorAnnounce);
        if (tempMonitorAnnounce.empty() ||
            tempMonitorAnnounce[0] == "ERROR")
        {
            tempMonitorConnection->DownRef();
            tempMonitorConnection = NULL;
            if (tempMonitorAnnounce.empty())
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR +
                        "Failed to open event socket, timeout");
            }
            else
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR +
                        "Failed to open event socket" +
                        ((tempMonitorAnnounce.size() >= 2) ?
                         QString(", error was %1").arg(tempMonitorAnnounce[1]) :
                         QString(", remote error")));
            }
        }

        QStringList tzCheck("QUERY_TIME_ZONE");
        if (tempMonitorConnection)
        {
            tempMonitorConnection->writeStringList(tzCheck);
            tempMonitorConnection->readStringList(tzCheck);
        }
        if (tzCheck.size() && !checkTimeZone(tzCheck))
        {
            // Check for different time zones, different offsets, different
            // times
            VERBOSE(VB_IMPORTANT, "The time and/or time zone settings on "
                    "this system do not match those in use on the master "
                    "backend. Please ensure all frontend and backend "
                    "systems are configured to use the same time zone and "
                    "have the current time properly set.");
            VERBOSE(VB_IMPORTANT,
                    "Unable to run with invalid time settings. Exiting.");
            tempMonitorConnection->writeStringList(tempMonitorDone);
            tempMonitorConnection->DownRef();
            return BACKEND_EXIT_INVALID_TIMEZONE;
        }
        else
        {
            VERBOSE(VB_IMPORTANT,
                    QString("Backend is running in %1 time zone.")
                    .arg(getTimeZoneID()));
        }
        if (tempMonitorConnection)
            tempMonitorConnection->writeStringList(tempMonitorDone);
    }
    if (tempMonitorConnection)
        tempMonitorConnection->DownRef();

    return BACKEND_EXIT_OK;
}

int setup_basics(const MythCommandLineParser &cmdline)
{
    ofstream pidfs;
    if (!openPidfile(pidfs, cmdline.GetPIDFilename()))
        return BACKEND_EXIT_OPENING_PIDFILE_ERROR;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        VERBOSE(VB_IMPORTANT, LOC_WARN + "Unable to ignore SIGPIPE");

    if (cmdline.IsDaemonizeEnabled() && (daemon(0, 1) < 0))
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to daemonize" + ENO);
        return BACKEND_EXIT_DAEMONIZING_ERROR;
    }

    QString username = cmdline.GetUsername();
    if (!username.isEmpty() && !setUser(username))
        return BACKEND_EXIT_PERMISSIONS_ERROR;

    if (pidfs)
    {
        pidfs << getpid() << endl;
        pidfs.close();
    }

    return BACKEND_EXIT_OK;
}

void print_warnings(const MythCommandLineParser &cmdline)
{
    if (!cmdline.IsHouseKeeperEnabled())
    {
        VERBOSE(VB_IMPORTANT, LOC_WARN +
                "****** The Housekeeper has been DISABLED with "
                "the --nohousekeeper option ******");
    }
    if (!cmdline.IsSchedulerEnabled())
    {
        VERBOSE(VB_IMPORTANT, LOC_WARN +
                "********** The Scheduler has been DISABLED with "
                "the --nosched option **********");
    }
    if (!cmdline.IsAutoExpirerEnabled())
    {
        VERBOSE(VB_IMPORTANT, LOC_WARN +
                "********* Auto-Expire has been DISABLED with "
                "the --noautoexpire option ********");
    }
    if (!cmdline.IsJobQueueEnabled())
    {
        VERBOSE(VB_IMPORTANT, LOC_WARN +
                "********* The JobQueue has been DISABLED with "
                "the --nojobqueue option *********");
    }
}

int run_backend(const MythCommandLineParser &cmdline)
{
    if (!setup_context(cmdline))
        return BACKEND_EXIT_NO_MYTHCONTEXT;

    if (!UpgradeTVDatabaseSchema(true, true))
    {
        VERBOSE(VB_IMPORTANT, "Couldn't upgrade database to new schema");
        return BACKEND_EXIT_DB_OUTOFDATE;
    }

    ///////////////////////////////////////////

    bool ismaster = gContext->IsMasterHost();

    g_pUPnp = new MediaServer(ismaster, !cmdline.IsUPnPEnabled() );

    if (!ismaster)
    {
        int ret = connect_to_master();
        if (BACKEND_EXIT_OK != ret)
            return ret;
    }

    QString myip = gContext->GetSetting("BackendServerIP");
    int     port = gContext->GetNumSetting("BackendServerPort", 6543);
    if (myip.isEmpty())
    {
        cerr << "No setting found for this machine's BackendServerIP.\n"
             << "Please run setup on this machine and modify the first page\n"
             << "of the general settings.\n";
        return BACKEND_EXIT_NO_IP_ADDRESS;
    }

    MythSystemEventHandler *sysEventHandler = new MythSystemEventHandler();

    if (ismaster)
    {
        VERBOSE(VB_GENERAL, LOC + "Starting up as the master server.");
        gContext->LogEntry("mythbackend", LP_INFO,
                           "MythBackend started as master server", "");
    }
    else
    {
        VERBOSE(VB_GENERAL, LOC + "Running as a slave backend.");
        gContext->LogEntry("mythbackend", LP_INFO,
                           "MythBackend started as a slave backend", "");
    }

    bool fatal_error = false;
    bool runsched = setupTVs(ismaster, fatal_error);
    if (fatal_error)
    {
        delete sysEventHandler;
        return BACKEND_EXIT_CAP_CARD_SETUP_ERROR;
    }

    if (ismaster)
    {
        if (runsched)
        {
            sched = new Scheduler(true, &tvList);
            int err = sched->GetError();
            if (err)
                return err;

            if (!cmdline.IsSchedulerEnabled())
                sched->DisableScheduling();
        }

        if (cmdline.IsHouseKeeperEnabled())
            housekeeping = new HouseKeeper(true, ismaster, sched);

        if (!cmdline.IsAutoExpirerEnabled())
        {
            expirer = new AutoExpire(&tvList);
            if (sched)
                sched->SetExpirer(expirer);
        }
    }
    else if (cmdline.IsHouseKeeperEnabled())
    {
        housekeeping = new HouseKeeper(true, ismaster, NULL);
    }

    if (cmdline.IsJobQueueEnabled())
        jobqueue = new JobQueue(ismaster);

    // Setup status server
    HttpServer *pHS = g_pUPnp->GetHttpServer();
    if (pHS)
    {
        VERBOSE(VB_IMPORTANT, "Main::Registering HttpStatus Extension");

        pHS->RegisterExtension( new HttpStatus( &tvList, sched,
                                                expirer, ismaster ));
    }

    if (ismaster)
    {
        // kill -USR1 mythbackendpid will force a upnpmedia rebuild
        signal(SIGUSR1, &upnp_rebuild);
    }

    VERBOSE(VB_IMPORTANT, QString("Enabled verbose msgs: %1")
            .arg(verboseString));

    MainServer *mainServer = new MainServer(
        ismaster, port, &tvList, sched, expirer);

    int exitCode = mainServer->GetExitCode();
    if (exitCode != BACKEND_EXIT_OK)
    {
        VERBOSE(VB_IMPORTANT, "Backend exiting, MainServer initialization "
                "error.");
        delete mainServer;
        return exitCode;
    }

    StorageGroup::CheckAllStorageGroupDirs();

    if (gContext->IsMasterBackend())
        SendMythSystemEvent("MASTER_STARTED");

    ///////////////////////////////
    ///////////////////////////////
    exitCode = qApp->exec();
    ///////////////////////////////
    ///////////////////////////////

    if (gContext->IsMasterBackend())
    {
        SendMythSystemEvent("MASTER_SHUTDOWN");
        qApp->processEvents();
    }

    gContext->LogEntry("mythbackend", LP_INFO, "MythBackend exiting", "");

    delete sysEventHandler;
    delete mainServer;

    return exitCode;
}