// C headers
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <sched.h> // for sched_yield

// C++ headers
#include <iostream>
using namespace std;

// MythTV headers
#include "previewgeneratorqueue.h"
#include "mythconfig.h"
#include "tv_rec.h"
#include "osd.h"
#include "mythcorecontext.h"
#include "dialogbox.h"
#include "recordingprofile.h"
#include "util.h"
#include "programinfo.h"
#include "dtvsignalmonitor.h"
#include "mythdb.h"
#include "jobqueue.h"
#include "recordingrule.h"
#include "eitscanner.h"
#include "RingBuffer.h"
#include "storagegroup.h"
#include "remoteutil.h"
#include "tvremoteutil.h"
#include "mythsystemevent.h"

#include "atscstreamdata.h"
#include "dvbstreamdata.h"
#include "atsctables.h"

#include "livetvchain.h"

#include "channelutil.h"
#include "channelbase.h"
#include "dummychannel.h"
#include "dtvchannel.h"
#include "dvbchannel.h"
#include "hdhrchannel.h"
#include "iptvchannel.h"
#include "firewirechannel.h"

#include "recorderbase.h"
#include "NuppelVideoRecorder.h"
#include "mpegrecorder.h"
#include "dvbrecorder.h"
#include "hdhrrecorder.h"
#include "iptvrecorder.h"
#include "firewirerecorder.h"
#include "importrecorder.h"

#include "channelgroup.h"

#ifdef USING_V4L
#include "v4lchannel.h"
#endif

#define DEBUG_CHANNEL_PREFIX 0 /**< set to 1 to channel prefixing */

#define LOC QString("TVRec(%1): ").arg(cardid)
#define LOC_ERR QString("TVRec(%1) Error: ").arg(cardid)

/// How many milliseconds the signal monitor should wait between checks
const uint TVRec::kSignalMonitoringRate = 50; /* msec */

QMutex            TVRec::cardsLock;
QMap<uint,TVRec*> TVRec::cards;

static bool is_dishnet_eit(uint cardid);
static QString load_profile(QString,void*,RecordingInfo*,RecordingProfile&);
static int init_jobs(const RecordingInfo *rec, RecordingProfile &profile,
                     bool on_host, bool transcode_bfr_comm, bool on_line_comm);

/** \class TVRec
 *  \brief This is the coordinating class of the \ref recorder_subsystem.
 *
 *  TVRec is used by EncoderLink, which in turn is used by RemoteEncoder
 *  which allows the TV class on the frontend to communicate with TVRec
 *  and is used by MainServer to implement portions of the
 *  \ref myth_network_protocol on the backend.
 *
 *  TVRec contains an instance of RecorderBase, which actually handles the
 *  recording of a program. It also contains an instance of RingBuffer, which
 *  in this case is used to either stream an existing recording to the
 *  frontend, or to save a stream from the RecorderBase to disk. Finally,
 *  if there is a tuner on the hardware RecorderBase is implementing then
 *  TVRec contains a channel instance for that hardware, and possibly a
 *  SignalMonitor instance which monitors the signal quality on a tuners
 *  current input.
 */

/** \fn TVRec::TVRec(int)
 *  \brief Performs instance initialiation not requiring access to database.
 *
 *  \sa Init()
 *  \param capturecardnum Capture card number
 */
TVRec::TVRec(int capturecardnum)
       // Various components TVRec coordinates
    : recorder(NULL), channel(NULL), signalMonitor(NULL),
      scanner(NULL),
      // Event processing thread, runs RunTV()
      event_thread(pthread_t()),
      // Recorder thread, runs RecorderBase::StartRecording()
      recorder_thread(pthread_t()),
      // Configuration variables from database
      transcodeFirst(false),
      earlyCommFlag(false),         runJobOnHostOnly(false),
      eitCrawlIdleStart(60),        eitTransportTimeout(5*60),
      audioSampleRateDB(0),
      overRecordSecNrml(0),         overRecordSecCat(0),
      overRecordCategory(""),
      // Configuration variables from setup rutines
      cardid(capturecardnum), ispip(false),
      // State variables
      stateChangeLock(QMutex::Recursive),
      pendingRecLock(QMutex::Recursive),
      internalState(kState_None), desiredNextState(kState_None),
      changeState(false), pauseNotify(true),
      stateFlags(0), lastTuningRequest(0),
      triggerEventLoopLock(QMutex::NonRecursive),
      triggerEventLoopSignal(false),
      triggerEventSleepLock(QMutex::NonRecursive),
      triggerEventSleepSignal(false),
      m_switchingBuffer(false),
      m_recStatus(rsUnknown),
      // Current recording info
      curRecording(NULL), autoRunJobs(JOB_NONE),
      overrecordseconds(0),
      // Pseudo LiveTV recording
      pseudoLiveTVRecording(NULL),
      nextLiveTVDir(""),            nextLiveTVDirLock(),
      // tvchain
      tvchain(NULL),
      // RingBuffer info
      ringBuffer(NULL), rbFileExt("mpg")
{
    QMutexLocker locker(&cardsLock);
    cards[cardid] = this;
}

bool TVRec::CreateChannel(const QString &startchannel)
{
    rbFileExt = "mpg";
    bool init_run = false;

    if (genOpt.cardtype == "DVB")
    {
#ifdef USING_DVB
        channel = new DVBChannel(genOpt.videodev, this);
        if (!channel->Open())
            return false;
        GetDVBChannel()->SetSlowTuning(dvbOpt.dvb_tuning_delay);
        InitChannel(genOpt.defaultinput, startchannel);
        CloseChannel(); // Close the channel if in dvb_on_demand mode
        init_run = true;
#endif
    }
    else if (genOpt.cardtype == "FIREWIRE")
    {
#ifdef USING_FIREWIRE
        channel = new FirewireChannel(this, genOpt.videodev, fwOpt);
        if (!channel->Open())
            return false;
        InitChannel(genOpt.defaultinput, startchannel);
        init_run = true;
#endif
    }
    else if (genOpt.cardtype == "HDHOMERUN")
    {
#ifdef USING_HDHOMERUN
        channel = new HDHRChannel(this, genOpt.videodev);
        if (!channel->Open())
            return false;
        InitChannel(genOpt.defaultinput, startchannel);
        GetDTVChannel()->EnterPowerSavingMode();
        init_run = true;
#endif
    }
    else if ((genOpt.cardtype == "IMPORT") ||
             (genOpt.cardtype == "DEMO") ||
             (genOpt.cardtype == "MPEG" &&
              genOpt.videodev.toLower().left(5) == "file:"))
    {
        channel = new DummyChannel(this);
        if (!channel->Open())
            return false;
        InitChannel(genOpt.defaultinput, startchannel);
        init_run = true;
    }
    else if (genOpt.cardtype == "FREEBOX")
    {
#ifdef USING_IPTV
        channel = new IPTVChannel(this, genOpt.videodev);
        if (!channel->Open())
            return false;
        InitChannel(genOpt.defaultinput, startchannel);
        init_run = true;
#endif
    }
    else // "V4L" or "MPEG", ie, analog TV
    {
#ifdef USING_V4L
        channel = new V4LChannel(this, genOpt.videodev);
        if (!channel->Open())
            return false;
        InitChannel(genOpt.defaultinput, startchannel);
        CloseChannel();
        init_run = true;
#endif
        if ((genOpt.cardtype != "MPEG") && (genOpt.cardtype != "HDPVR"))
            rbFileExt = "nuv";
    }

    if (!init_run)
    {
        QString msg = QString(
            "%1 card configured on video device %2, \n"
            "but MythTV was not compiled with %3 support. \n"
            "\n"
            "Recompile MythTV with %4 support or remove the card \n"
            "from the configuration and restart MythTV.")
            .arg(genOpt.cardtype).arg(genOpt.videodev)
            .arg(genOpt.cardtype).arg(genOpt.cardtype);
        VERBOSE(VB_IMPORTANT, LOC_ERR + "\n" + msg + "\n");
        SetFlags(kFlagErrored);
        return false;
    }
    return true;
}

/** \fn TVRec::Init(void)
 *  \brief Performs instance initialization, returns true on success.
 *
 *  \return Returns true on success, false on failure.
 */
bool TVRec::Init(void)
{
    QMutexLocker lock(&stateChangeLock);

    if (!GetDevices(cardid, genOpt, dvbOpt, fwOpt))
        return false;

    pendingRecLock.lock();
    m_recStatus = rsUnknown;
    pendingRecLock.unlock();

    // configure the Channel instance
    QString startchannel = GetStartChannel(cardid, genOpt.defaultinput);
    if (!CreateChannel(startchannel))
        return false;

    transcodeFirst    =
        gCoreContext->GetNumSetting("AutoTranscodeBeforeAutoCommflag", 0);
    earlyCommFlag     = gCoreContext->GetNumSetting("AutoCommflagWhileRecording", 0);
    runJobOnHostOnly  = gCoreContext->GetNumSetting("JobsRunOnRecordHost", 0);
    eitTransportTimeout=gCoreContext->GetNumSetting("EITTransportTimeout", 5) * 60;
    eitCrawlIdleStart = gCoreContext->GetNumSetting("EITCrawIdleStart", 60);
    audioSampleRateDB = gCoreContext->GetNumSetting("AudioSampleRate");
    overRecordSecNrml = gCoreContext->GetNumSetting("RecordOverTime");
    overRecordSecCat  = gCoreContext->GetNumSetting("CategoryOverTime") * 60;
    overRecordCategory= gCoreContext->GetSetting("OverTimeCategory");

    pthread_create(&event_thread, NULL, EventThread, this);

    WaitForEventThreadSleep();

    return true;
}

/** \fn TVRec::~TVRec()
 *  \brief Stops the event and scanning threads and deletes any ChannelBase,
 *         RingBuffer, and RecorderBase instances.
 */
TVRec::~TVRec()
{
    QMutexLocker locker(&cardsLock);
    cards.remove(cardid);
    TeardownAll();
}

void TVRec::TeardownAll(void)
{
    if (HasFlags(kFlagRunMainLoop))
    {
        ClearFlags(kFlagRunMainLoop);
        pthread_join(event_thread, NULL);
    }

    TeardownSignalMonitor();

    if (scanner)
    {
        delete scanner;
        scanner = NULL;
    }

    if (channel)
    {
        delete channel;
        channel = NULL;
    }

    TeardownRecorder(true);

    SetRingBuffer(NULL);
}

void TVRec::WakeEventLoop(void)
{
    QMutexLocker locker(&triggerEventLoopLock);
    triggerEventLoopSignal = true;
    triggerEventLoopWait.wakeAll();
}

/** \fn TVRec::GetState() const
 *  \brief Returns the TVState of the recorder.
 *
 *   If there is a pending state change kState_ChangingState is returned.
 *  \sa EncoderLink::GetState(), \ref recorder_subsystem
 */
TVState TVRec::GetState(void) const
{
    if (changeState)
        return kState_ChangingState;
    return internalState;
}

/** \fn TVRec::GetRecording(void)
 *  \brief Allocates and returns a ProgramInfo for the current recording.
 *
 *  Note: The user of this function must free the %ProgramInfo this returns.
 *  \return %ProgramInfo for the current recording, if it exists, blank
 *          %ProgramInfo otherwise.
 */
ProgramInfo *TVRec::GetRecording(void)
{
    QMutexLocker lock(&stateChangeLock);

    ProgramInfo *tmppginfo = NULL;

    if (curRecording && !changeState)
    {
        tmppginfo = new ProgramInfo(*curRecording);
        tmppginfo->SetRecordingStatus(rsRecording);
    }
    else
        tmppginfo = new ProgramInfo();
    tmppginfo->SetCardID(cardid);

    return tmppginfo;
}

/** \fn TVRec::RecordPending(const ProgramInfo*, int, bool)
 *  \brief Tells TVRec "rcinfo" is the next pending recording.
 *
 *   When there is a pending recording and the frontend is in "Live TV"
 *   mode the TVRec event loop will send a "ASK_RECORDING" message to
 *   it. Depending on what that query returns, the recording will be
 *   started or not started.
 *
 *  \sa TV::AskAllowRecording(const QStringList&, int, bool)
 *  \param rcinfo   ProgramInfo on pending program.
 *  \param secsleft Seconds left until pending recording begins.
 *                  Set to -1 to revoke the current pending recording.
 *  \param hasLater If true, a later non-conflicting showing is available.
 */
void TVRec::RecordPending(const ProgramInfo *rcinfo, int secsleft,
                          bool hasLater)
{
    QMutexLocker statelock(&stateChangeLock);
    QMutexLocker pendlock(&pendingRecLock);

    if (secsleft < 0)
    {
        VERBOSE(VB_RECORD, LOC + "Pending recording revoked on " +
                QString("inputid %1").arg(rcinfo->GetInputID()));

        PendingMap::iterator it = pendingRecordings.find(rcinfo->GetCardID());
        if (it != pendingRecordings.end())
        {
            (*it).ask = false;
            (*it).doNotAsk = (*it).canceled = true;
        }
        return;
    }

    VERBOSE(VB_RECORD, LOC +
            QString("RecordPending on inputid %1").arg(rcinfo->GetInputID()));

    PendingInfo pending;
    pending.info            = new ProgramInfo(*rcinfo);
    pending.recordingStart  = QDateTime::currentDateTime().addSecs(secsleft);
    pending.hasLaterShowing = hasLater;
    pending.ask             = true;
    pending.doNotAsk        = false;

    pendingRecordings[rcinfo->GetCardID()] = pending;

    // If this isn't a recording for this instance to make, we are done
    if (rcinfo->GetCardID() != cardid)
        return;

    // We also need to check our input groups
    vector<uint> cardids = CardUtil::GetConflictingCards(
        rcinfo->GetInputID(), cardid);

    pendingRecordings[rcinfo->GetCardID()].possibleConflicts = cardids;

    pendlock.unlock();
    statelock.unlock();
    for (uint i = 0; i < cardids.size(); i++)
        RemoteRecordPending(cardids[i], rcinfo, secsleft, hasLater);
    statelock.relock();
    pendlock.relock();
}

/** \fn TVRec::SetPseudoLiveTVRecording(ProgramInfo*)
 *  \brief Sets the pseudo LiveTV ProgramInfo
 */
void TVRec::SetPseudoLiveTVRecording(ProgramInfo *pi)
{
    ProgramInfo *old_rec = pseudoLiveTVRecording;
    pseudoLiveTVRecording = pi;
    if (old_rec)
        delete old_rec;
}

/** \fn TVRec::GetRecordEndTime(const ProgramInfo*) const
 *  \brief Returns recording end time with proper post-roll
 */
QDateTime TVRec::GetRecordEndTime(const ProgramInfo *pi) const
{
    bool spcat = (!overRecordCategory.isEmpty() &&
                  pi->GetCategory() == overRecordCategory);
    int secs = (spcat) ? overRecordSecCat : overRecordSecNrml;
    return pi->GetRecordingEndTime().addSecs(secs);
}

/** \fn TVRec::CancelNextRecording(bool)
 *  \brief Tells TVRec to cancel the upcoming recording.
 *  \sa RecordPending(const ProgramInfo*, int, bool),
 *      TV::AskAllowRecording(const QStringList&, int, bool)
 */
void TVRec::CancelNextRecording(bool cancel)
{
    QMutexLocker pendlock(&pendingRecLock);
    VERBOSE(VB_RECORD, LOC + "CancelNextRecording("<<cancel<<") -- begin");

    PendingMap::iterator it = pendingRecordings.find(cardid);
    if (it == pendingRecordings.end())
    {
        VERBOSE(VB_RECORD, LOC + "CancelNextRecording("<<cancel<<") -- "
                "error, unknown recording");
        return;
    }

    if (cancel)
    {
        vector<uint> &cardids = (*it).possibleConflicts;
        for (uint i = 0; i < cardids.size(); i++)
        {
            VERBOSE(VB_RECORD, LOC +
                    "CancelNextRecording -- cardid "<<cardids[i]);

            pendlock.unlock();
            RemoteRecordPending(cardids[i], (*it).info, -1, false);
            pendlock.relock();
        }

        VERBOSE(VB_RECORD, LOC + "CancelNextRecording -- cardid "<<cardid);

        RecordPending((*it).info, -1, false);
    }
    else
    {
        (*it).canceled = false;
    }

    VERBOSE(VB_RECORD, LOC + "CancelNextRecording("<<cancel<<") -- end");
}

/** \fn TVRec::StartRecording(const ProgramInfo*)
 *  \brief Tells TVRec to Start recording the program "rcinfo"
 *         as soon as possible.
 *
 *  \return +1 if the recording started successfully,
 *          -1 if TVRec is busy doing something else, 0 otherwise.
 *  \sa EncoderLink::StartRecording(const ProgramInfo*)
 *      RecordPending(const ProgramInfo*, int, bool), StopRecording()
 */
RecStatusType TVRec::StartRecording(const ProgramInfo *rcinfo)
{
    VERBOSE(VB_RECORD, LOC + QString("StartRecording(%1)")
            .arg(rcinfo->toString(ProgramInfo::kTitleSubtitle)));

    QMutexLocker lock(&stateChangeLock);
    QString msg("");

    pendingRecLock.lock();
    m_recStatus = rsAborted;
    pendingRecLock.unlock();

    // Flush out any pending state changes
    WaitForEventThreadSleep();

    // We need to do this check early so we don't cancel an overrecord
    // that we're trying to extend.
    if (internalState != kState_WatchingLiveTV &&
        curRecording && curRecording->IsSameProgramWeakCheck(*rcinfo))
    {
        int post_roll_seconds  = curRecording->GetRecordingEndTime()
            .secsTo(recordEndTime);

        curRecording->SetRecordingRuleType(rcinfo->GetRecordingRuleType());
        curRecording->SetRecordingRuleID(rcinfo->GetRecordingRuleID());
        curRecording->SetRecordingEndTime(rcinfo->GetRecordingEndTime());
        curRecording->UpdateRecordingEnd();

        recordEndTime = curRecording->GetRecordingEndTime()
            .addSecs(post_roll_seconds);

        msg = QString("updating recording: %1 %2 %3 %4")
            .arg(curRecording->GetTitle()).arg(curRecording->GetChanID())
            .arg(curRecording->GetRecordingStartTime(ISODate))
            .arg(curRecording->GetRecordingEndTime(ISODate));
        VERBOSE(VB_RECORD, LOC + msg);

        ClearFlags(kFlagCancelNextRecording);

        pendingRecLock.lock();
        m_recStatus = rsRecording;
        pendingRecLock.unlock();
        return rsRecording;
    }

    bool cancelNext = false;
    PendingInfo pendinfo;
    PendingMap::iterator it;
    bool has_pending;

    pendingRecLock.lock();
    if ((it = pendingRecordings.find(cardid)) != pendingRecordings.end())
    {
        (*it).ask = (*it).doNotAsk = false;
        cancelNext = (*it).canceled;
    }
    pendingRecLock.unlock();

    // Flush out events...
    WaitForEventThreadSleep();

    // Rescan pending recordings since the event loop may have deleted
    // a stale entry.  If this happens the info pointer will not be valid
    // since the HandlePendingRecordings loop will have deleted it.
    pendingRecLock.lock();
    it = pendingRecordings.find(cardid);
    has_pending = (it != pendingRecordings.end());
    if (has_pending)
        pendinfo = *it;
    pendingRecLock.unlock();

    // If the needed input is in a shared input group, and we are
    // not canceling the recording anyway, check other recorders
    if (!cancelNext && has_pending && pendinfo.possibleConflicts.size())
    {
        VERBOSE(VB_RECORD, LOC + "Checking input group recorders - begin");
        vector<uint> &cardids = pendinfo.possibleConflicts;

        uint mplexid = 0, sourceid = 0;
        vector<uint> cardids2;
        vector<TVState> states;

        // Stop remote recordings if needed
        for (uint i = 0; i < cardids.size(); i++)
        {
            TunedInputInfo busy_input;
            bool is_busy = RemoteIsBusy(cardids[i], busy_input);

            // if the other recorder is busy, but the input is
            // not in a shared input group, then as far as we're
            // concerned here it isn't busy.
            if (is_busy)
            {
                is_busy = (bool) igrp.GetSharedInputGroup(
                    busy_input.inputid, rcinfo->GetInputID());
            }

            if (is_busy && !sourceid)
            {
                mplexid  = pendinfo.info->QueryMplexID();
                sourceid = pendinfo.info->GetSourceID();
            }

            if (is_busy &&
                ((sourceid != busy_input.sourceid) ||
                 (mplexid  != busy_input.mplexid)))
            {
                states.push_back((TVState) RemoteGetState(cardids[i]));
                cardids2.push_back(cardids[i]);
            }
        }

        bool ok = true;
        for (uint i = 0; (i < cardids2.size()) && ok; i++)
        {
            VERBOSE(VB_RECORD, LOC +
                    QString("Attempting to stop card %1 in state %2")
                    .arg(cardids2[i]).arg(StateToString(states[i])));

            bool success = RemoteStopRecording(cardids2[i]);
            if (success)
            {
                uint state = RemoteGetState(cardids2[i]);
                VERBOSE(VB_IMPORTANT, LOC + QString("a %1: %2")
                        .arg(cardids2[i]).arg(StateToString((TVState)state)));
                success = (kState_None == state);
            }

            // If we managed to stop LiveTV recording, restart playback..
            if (success && states[i] == kState_WatchingLiveTV)
            {
                QString message = QString("QUIT_LIVETV %1").arg(cardids2[i]);
                MythEvent me(message);
                gCoreContext->dispatch(me);
            }

            VERBOSE(VB_RECORD, LOC + QString(
                        "Stopping recording on %1, %2")
                    .arg(cardids2[i])
                    .arg(success ? "succeeded" : "failed"));

            ok &= success;
        }

        // If we failed to stop the remote recordings, don't record
        if (!ok)
        {
            CancelNextRecording(true);
            cancelNext = true;
        }

        cardids.clear();

        VERBOSE(VB_RECORD, LOC + "Checking input group recorders - done");
    }

    // If in post-roll, end recording
    if (!cancelNext && (GetState() == kState_RecordingOnly))
    {
        stateChangeLock.unlock();
        StopRecording();
        stateChangeLock.lock();
    }

    if (!cancelNext && (GetState() == kState_None))
    {
        if (tvchain)
        {
            QString message = QString("LIVETV_EXITED");
            MythEvent me(message, tvchain->GetID());
            gCoreContext->dispatch(me);
            tvchain = NULL;
        }

        recordEndTime = GetRecordEndTime(rcinfo);

        // Tell event loop to begin recording.
        curRecording = new RecordingInfo(*rcinfo);
        curRecording->MarkAsInUse(true, kRecorderInUseID);
        StartedRecording(curRecording);

        // Make sure scheduler is allowed to end this recording
        ClearFlags(kFlagCancelNextRecording);

        pendingRecLock.lock();
        m_recStatus = rsTuning;
        pendingRecLock.unlock();
        ChangeState(kState_RecordingOnly);
    }
    else if (!cancelNext && (GetState() == kState_WatchingLiveTV))
    {
        SetPseudoLiveTVRecording(new ProgramInfo(*rcinfo));
        recordEndTime = GetRecordEndTime(rcinfo);
        pendingRecLock.lock();
        m_recStatus = rsRecording;
        pendingRecLock.unlock();

        // We want the frontend to change channel for recording
        // and disable the UI for channel change, PiP, etc.

        QString message = QString("LIVETV_WATCH %1 1").arg(cardid);
        QStringList prog;
        rcinfo->ToStringList(prog);
        MythEvent me(message, prog);
        gCoreContext->dispatch(me);
    }
    else
    {
        msg = QString("Wanted to record: %1 %2 %3 %4\n\t\t\t")
            .arg(rcinfo->GetTitle()).arg(rcinfo->GetChanID())
            .arg(rcinfo->GetRecordingStartTime(ISODate))
            .arg(rcinfo->GetRecordingEndTime(ISODate));

        if (cancelNext)
        {
            msg += "But a user has canceled this recording";
            pendingRecLock.lock();
            m_recStatus = rsCancelled;
            pendingRecLock.unlock();
        }
        else
        {
            msg += QString("But the current state is: %1")
                .arg(StateToString(internalState));
            pendingRecLock.lock();
            m_recStatus = rsTunerBusy;
            pendingRecLock.unlock();
        }

        if (curRecording && internalState == kState_RecordingOnly)
            msg += QString("\n\t\t\tCurrently recording: %1 %2 %3 %4")
                .arg(curRecording->GetTitle()).arg(curRecording->GetChanID())
                .arg(curRecording->GetRecordingStartTime(ISODate))
                .arg(curRecording->GetRecordingEndTime(ISODate));

        VERBOSE(VB_IMPORTANT, LOC + msg);
    }

    for (int i = 0; i < pendingRecordings.size(); i++)
        delete pendingRecordings[i].info;
    pendingRecordings.clear();

    WaitForEventThreadSleep();

    RecStatusType  status;

    pendingRecLock.lock();
    if ((curRecording) && (curRecording->GetRecordingStatus() == rsFailed) &&
        (m_recStatus == rsRecording || m_recStatus == rsTuning))
        m_recStatus = rsFailed;
    status = m_recStatus;
    pendingRecLock.unlock();

    return status;
}

RecStatusType TVRec::GetRecordingStatus(void) const
{
    QMutexLocker pendlock(&pendingRecLock);
    return m_recStatus;
}


/** \fn TVRec::StopRecording(bool killFile)
 *  \brief Changes from a recording state to kState_None.
 *  \sa StartRecording(const ProgramInfo *rec), FinishRecording()
 */
void TVRec::StopRecording(bool killFile)
{
    if (StateIsRecording(GetState()))
    {
        QMutexLocker lock(&stateChangeLock);
        if (killFile)
            SetFlags(kFlagKillRec);
        ChangeState(RemoveRecording(GetState()));
        // wait for state change to take effect
        WaitForEventThreadSleep();
        ClearFlags(kFlagCancelNextRecording|kFlagKillRec);

        pendingRecLock.lock();
        m_recStatus = rsUnknown;
        pendingRecLock.unlock();
    }
}

/** \fn TVRec::StateIsRecording(TVState)
 *  \brief Returns true if "state" is kState_RecordingOnly,
 *         or kState_WatchingLiveTV.
 *  \param state TVState to check.
 */
bool TVRec::StateIsRecording(TVState state)
{
    return (state == kState_RecordingOnly ||
            state == kState_WatchingLiveTV);
}

/** \fn TVRec::StateIsPlaying(TVState)
 *  \brief Returns true if we are in any state associated with a player.
 *  \param state TVState to check.
 */
bool TVRec::StateIsPlaying(TVState state)
{
    return (state == kState_WatchingPreRecorded);
}

/** \fn TVRec::RemoveRecording(TVState)
 *  \brief If "state" is kState_RecordingOnly or kState_WatchingLiveTV,
 *         returns a kState_None, otherwise returns kState_Error.
 *  \param state TVState to check.
 */
TVState TVRec::RemoveRecording(TVState state)
{
    if (StateIsRecording(state))
        return kState_None;

    VERBOSE(VB_IMPORTANT, LOC_ERR +
            QString("Unknown state in RemoveRecording: %1")
            .arg(StateToString(state)));
    return kState_Error;
}

/** \fn TVRec::RemovePlaying(TVState)
 *  \brief Returns TVState that would remove the playing, but potentially
 *         keep recording if we are watching an in progress recording.
 *  \param state TVState to check.
 */
TVState TVRec::RemovePlaying(TVState state)
{
    if (StateIsPlaying(state))
    {
        if (state == kState_WatchingPreRecorded)
            return kState_None;
        return kState_RecordingOnly;
    }

    QString msg = "Unknown state in RemovePlaying: %1";
    VERBOSE(VB_IMPORTANT, LOC_ERR + msg.arg(StateToString(state)));

    return kState_Error;
}

/** \fn TVRec::StartedRecording(RecordingInfo *curRec)
 *  \brief Inserts a "curRec" into the database
 *  \param curRec Recording to add to database.
 *  \sa ProgramInfo::StartedRecording(const QString&)
 */
void TVRec::StartedRecording(RecordingInfo *curRec)
{
    if (!curRec)
        return;

    curRec->StartedRecording(rbFileExt);
    VERBOSE(VB_RECORD, LOC + "StartedRecording("<<curRec<<") fn("
            <<curRec->GetPathname()<<")");

    if (curRec->IsCommercialFree())
        curRec->SaveCommFlagged(COMM_FLAG_COMMFREE);

    SendMythSystemRecEvent("REC_STARTED", curRec);
}

/** \fn TVRec::FinishedRecording(RecordingInfo *curRec)
 *  \brief If not a premature stop, adds program to history of recorded
 *         programs. If the recording type is kFindOneRecord this find
 *         is removed.
 *  \sa ProgramInfo::FinishedRecording(bool prematurestop)
 *  \param curRec ProgramInfo or recording to mark as done
 */
void TVRec::FinishedRecording(RecordingInfo *curRec)
{
    if (!curRec)
        return;

    const QString recgrp = curRec->QueryRecordingGroup();
    curRec->SetRecordingGroup(recgrp);

    VERBOSE(VB_RECORD, LOC + QString("FinishedRecording(%1) in recgroup: %2")
            .arg(curRec->GetTitle()).arg(recgrp));

    if (curRec->GetRecordingStatus() == rsRecording)
        curRec->SetRecordingStatus(rsRecorded);
    else if (curRec->GetRecordingStatus() != rsRecorded)
        curRec->SetRecordingStatus(rsFailed);
    curRec->SetRecordingEndTime(mythCurrentDateTime());

    if (tvchain)
        tvchain->FinishedRecording(curRec);

    // Make sure really short recordings have positive run time.
    if (curRec->GetRecordingEndTime() <= curRec->GetRecordingStartTime())
    {
        curRec->SetRecordingEndTime(
            curRec->GetRecordingStartTime().addSecs(60));
    }

    // Round up recording end time (probably should be done in the UI only)
    QDateTime recendts = curRec->GetRecordingEndTime();
    recendts.setTime(QTime(recendts.addSecs(30).time().hour(),
                           recendts.addSecs(30).time().minute()));
    curRec->SetRecordingEndTime(recendts);

    if (recgrp != "LiveTV")
    {
        MythEvent me(QString("UPDATE_RECORDING_STATUS %1 %2 %3 %4 %5")
                     .arg(curRec->GetCardID())
                     .arg(curRec->GetChanID())
                     .arg(curRec->GetScheduledStartTime(ISODate))
                     .arg(curRec->GetRecordingStatus())
                     .arg(curRec->GetRecordingEndTime(ISODate)));
        gCoreContext->dispatch(me);
    }

    curRec->FinishedRecording(curRec->GetRecordingStatus() != rsRecorded);
}

#define TRANSITION(ASTATE,BSTATE) \
   ((internalState == ASTATE) && (desiredNextState == BSTATE))
#define SET_NEXT() do { nextState = desiredNextState; changed = true; } while(0)
#define SET_LAST() do { nextState = internalState; changed = true; } while(0)

/** \fn TVRec::HandleStateChange(void)
 *  \brief Changes the internalState to the desiredNextState if possible.
 *
 *   Note: There must exist a state transition from any state we can enter
 *   to the kState_None state, as this is used to shutdown TV in RunTV.
 *
 */
void TVRec::HandleStateChange(void)
{
    TVState nextState = internalState;

    bool changed = false;

    QString transMsg = QString(" %1 to %2")
        .arg(StateToString(nextState))
        .arg(StateToString(desiredNextState));

    if (desiredNextState == internalState)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "HandleStateChange(): "
                "Null transition" + transMsg);
        changeState = false;
        return;
    }

    // Make sure EIT scan is stopped before any tuning,
    // to avoid race condition with it's tuning requests.
    if (HasFlags(kFlagEITScannerRunning))
    {
        scanner->StopActiveScan();
        ClearFlags(kFlagEITScannerRunning);
    }

    // Handle different state transitions
    if (TRANSITION(kState_None, kState_WatchingLiveTV))
    {
        tuningRequests.enqueue(TuningRequest(kFlagLiveTV));
        SET_NEXT();
    }
    else if (TRANSITION(kState_WatchingLiveTV, kState_None))
    {
        tuningRequests.enqueue(TuningRequest(kFlagKillRec|kFlagKillRingBuffer));
        SET_NEXT();
    }
    else if (TRANSITION(kState_WatchingLiveTV, kState_RecordingOnly))
    {
        SetPseudoLiveTVRecording(NULL);

        SET_NEXT();
    }
    else if (TRANSITION(kState_None, kState_RecordingOnly))
    {
        SetPseudoLiveTVRecording(NULL);
        tuningRequests.enqueue(TuningRequest(kFlagRecording, curRecording));
        SET_NEXT();
    }
    else if (TRANSITION(kState_RecordingOnly, kState_None))
    {
        tuningRequests.enqueue(
            TuningRequest(kFlagCloseRec|kFlagKillRingBuffer|
                          (GetFlags()&kFlagKillRec)));
        SET_NEXT();
    }

    QString msg = (changed) ? "Changing from" : "Unknown state transition:";
    VERBOSE(VB_IMPORTANT, LOC + msg + transMsg);

    // update internal state variable
    internalState = nextState;
    changeState = false;

    eitScanStartTime = QDateTime::currentDateTime();
    if ((internalState == kState_None) &&
        scanner)
        eitScanStartTime = eitScanStartTime.addSecs(eitCrawlIdleStart);
    else
        eitScanStartTime = eitScanStartTime.addYears(1);
}
#undef TRANSITION
#undef SET_NEXT
#undef SET_LAST

/** \fn TVRec::ChangeState(TVState)
 *  \brief Puts a state change on the nextState queue.
 */
void TVRec::ChangeState(TVState nextState)
{
    QMutexLocker lock(&stateChangeLock);
    desiredNextState = nextState;
    changeState = true;
    WakeEventLoop();
}

/** \fn TVRec::SetupRecorder(RecordingProfile&)
 *  \brief Allocates and initializes the RecorderBase instance.
 *
 *  Based on the card type, one of the possible recorders are started.
 *  If the card type is "MPEG" or "HDPVR" a MpegRecorder is started,
 *  if the card type is "HDHOMERUN" a HDHRRecorder is started,
 *  if the card type is "FIREWIRE" a FirewireRecorder is started,
 *  if the card type is "DVB" a DVBRecorder is started,
 *  otherwise a NuppelVideoRecorder is started.
 *
 *  If there is any this will return false.
 * \sa IsErrored()
 */
bool TVRec::SetupRecorder(RecordingProfile &profile)
{
    recorder = NULL;
    if (genOpt.cardtype == "MPEG")
    {
#ifdef USING_IVTV
        recorder = new MpegRecorder(this);
#endif // USING_IVTV
    }
    else if (genOpt.cardtype == "HDPVR")
    {
#ifdef USING_HDPVR
        recorder = new MpegRecorder(this);
#endif // USING_HDPVR
    }
    else if (genOpt.cardtype == "FIREWIRE")
    {
#ifdef USING_FIREWIRE
        recorder = new FirewireRecorder(this, GetFirewireChannel());
#endif // USING_FIREWIRE
    }
    else if (genOpt.cardtype == "HDHOMERUN")
    {
#ifdef USING_HDHOMERUN
        recorder = new HDHRRecorder(this, GetHDHRChannel());
        ringBuffer->SetWriteBufferSize(4*1024*1024);
        recorder->SetOption("wait_for_seqstart", genOpt.wait_for_seqstart);
#endif // USING_HDHOMERUN
    }
    else if (genOpt.cardtype == "DVB")
    {
#ifdef USING_DVB
        recorder = new DVBRecorder(this, GetDVBChannel());
        ringBuffer->SetWriteBufferSize(4*1024*1024);
        recorder->SetOption("wait_for_seqstart", genOpt.wait_for_seqstart);
        recorder->SetOption("dvb_on_demand",     dvbOpt.dvb_on_demand);
#endif // USING_DVB
    }
    else if (genOpt.cardtype == "FREEBOX")
    {
#ifdef USING_IPTV
        IPTVChannel *chan = dynamic_cast<IPTVChannel*>(channel);
        recorder = new IPTVRecorder(this, chan);
        ringBuffer->SetWriteBufferSize(4*1024*1024);
        recorder->SetOption("mrl", genOpt.videodev);
#endif // USING_IPTV
    }
    else if (genOpt.cardtype == "IMPORT")
    {
        recorder = new ImportRecorder(this);
    }
    else if (genOpt.cardtype == "DEMO")
    {
#ifdef USING_IVTV
        recorder = new MpegRecorder(this);
#else
        recorder = new ImportRecorder(this);
#endif
    }
    else
    {
#ifdef USING_V4L
        // V4L/MJPEG/GO7007 from here on
        recorder = new NuppelVideoRecorder(this, channel);
        recorder->SetOption("skipbtaudio", genOpt.skip_btaudio);
#endif // USING_V4L
    }

    if (recorder)
    {
        recorder->SetOptionsFromProfile(
            &profile, genOpt.videodev, genOpt.audiodev, genOpt.vbidev);
        // Override the samplerate defined in the profile if this card
        // was configured with a fixed rate.
        if (genOpt.audiosamplerate)
            recorder->SetOption("samplerate", genOpt.audiosamplerate);
        recorder->SetRingBuffer(ringBuffer);
        recorder->Initialize();

        if (recorder->IsErrored())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to initialize recorder!");
            delete recorder;
            recorder = NULL;
            return false;
        }

        return true;
    }

    QString msg = "Need %1 recorder, but compiled without %2 support!";
    msg = msg.arg(genOpt.cardtype).arg(genOpt.cardtype);
    VERBOSE(VB_IMPORTANT, LOC_ERR + msg);

    return false;
}

/** \fn TVRec::TeardownRecorder(bool)
 *  \brief Tears down the recorder.
 *
 *   If a "recorder" exists, RecorderBase::StopRecording() is called.
 *   We then wait for "recorder_thread" to exit, and finally we delete
 *   "recorder".
 *
 *   If a RingBuffer instance exists, RingBuffer::StopReads() is called,
 *   and then we delete the RingBuffer instance.
 *
 *   If killfile is true, the recording is deleted.
 *
 *   Finally, if there was a recording and it was not deleted,
 *   schedule any post-processing jobs.
 *
 *  \param killFile if true the recorded file is deleted.
 */
void TVRec::TeardownRecorder(bool killFile)
{
    pauseNotify = false;
    ispip = false;

    if (recorder && HasFlags(kFlagRecorderRunning))
    {
        // Get the width and set the videoprops
        uint avg_height = curRecording->QueryAverageHeight();
        curRecording->SaveResolutionProperty(
            (avg_height > 1000) ? VID_1080 :
            ((avg_height > 700) ? VID_720 : VID_UNKNOWN));

        int secsSince = curRecording->GetRecordingStartTime()
            .secsTo(QDateTime::currentDateTime());
        QString message = QString("DONE_RECORDING %1 %2 %3")
            .arg(cardid).arg(secsSince).arg(GetFramesWritten());
        MythEvent me(message);
        gCoreContext->dispatch(me);

        recorder->StopRecording();
        pthread_join(recorder_thread, NULL);
    }
    ClearFlags(kFlagRecorderRunning);

    if (recorder)
    {
        if (GetV4LChannel())
            channel->SetFd(-1);

        delete recorder;
        recorder = NULL;
    }

    if (ringBuffer)
        ringBuffer->StopReads();

    if (curRecording)
    {
        if (!killFile)
        {
            if (curRecording->IsLocal())
                PreviewGeneratorQueue::GetPreviewImage(*curRecording, "");

            if (!tvchain)
            {
                int secsSince = curRecording->GetRecordingStartTime()
                    .secsTo(QDateTime::currentDateTime());
                if (secsSince < 120)
                {
                    JobQueue::RemoveJobsFromMask(JOB_COMMFLAG, autoRunJobs);
                    JobQueue::RemoveJobsFromMask(JOB_TRANSCODE, autoRunJobs);
                }

                if (autoRunJobs)
                    JobQueue::QueueRecordingJobs(*curRecording, autoRunJobs);
            }
        }

        FinishedRecording(curRecording);

        SendMythSystemRecEvent("REC_FINISHED", curRecording);

        curRecording->MarkAsInUse(false, kRecorderInUseID);
        delete curRecording;
        curRecording = NULL;
    }

    pauseNotify = true;

    if (GetDTVChannel())
        GetDTVChannel()->EnterPowerSavingMode();
}

DVBRecorder *TVRec::GetDVBRecorder(void)
{
#ifdef USING_DVB
    return dynamic_cast<DVBRecorder*>(recorder);
#else // if !USING_DVB
    return NULL;
#endif // !USING_DVB
}

HDHRRecorder *TVRec::GetHDHRRecorder(void)
{
#ifdef USING_HDHOMERUN
    return dynamic_cast<HDHRRecorder*>(recorder);
#else // if !USING_HDHOMERUN
    return NULL;
#endif // !USING_HDHOMERUN
}

DTVRecorder *TVRec::GetDTVRecorder(void)
{
    return dynamic_cast<DTVRecorder*>(recorder);
}

/** \fn TVRec::InitChannel(const QString&, const QString&)
 *  \brief Performs ChannelBase instance init from database and
 *         tuner hardware (requires that channel be open).
 */
void TVRec::InitChannel(const QString &inputname, const QString &startchannel)
{
    if (!channel)
        return;

    QString input   = inputname;
    QString channum = startchannel;

    channel->Init(input, channum, true);
}

void TVRec::CloseChannel(void)
{
    if (!channel)
        return;

    if (GetDVBChannel() && !dvbOpt.dvb_on_demand)
        return;

    channel->Close();
}

DTVChannel *TVRec::GetDTVChannel(void)
{
    return dynamic_cast<DTVChannel*>(channel);
}

HDHRChannel *TVRec::GetHDHRChannel(void)
{
#ifdef USING_HDHOMERUN
    return dynamic_cast<HDHRChannel*>(channel);
#else
    return NULL;
#endif // USING_HDHOMERUN
}

DVBChannel *TVRec::GetDVBChannel(void)
{
#ifdef USING_DVB
    return dynamic_cast<DVBChannel*>(channel);
#else
    return NULL;
#endif // USING_DVB
}

FirewireChannel *TVRec::GetFirewireChannel(void)
{
#ifdef USING_FIREWIRE
    return dynamic_cast<FirewireChannel*>(channel);
#else
    return NULL;
#endif // USING_FIREWIRE
}

V4LChannel *TVRec::GetV4LChannel(void)
{
#ifdef USING_V4L
    return dynamic_cast<V4LChannel*>(channel);
#else
    return NULL;
#endif // USING_V4L
}

/** \fn TVRec::EventThread(void*)
 *  \brief Thunk that allows event pthread to call RunTV().
 */
void *TVRec::EventThread(void *param)
{
    TVRec *thetv = (TVRec *)param;
    thetv->RunTV();
    return NULL;
}

/** \fn TVRec::RecorderThread(void*)
 *  \brief Thunk that allows recorder pthread to
 *         call RecorderBase::StartRecording().
 */
void *TVRec::RecorderThread(void *param)
{
    RecorderBase *recorder = (RecorderBase *)param;
    recorder->StartRecording();
    return NULL;
}

static bool get_use_eit(uint cardid)
{
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT SUM(useeit) "
        "FROM videosource, cardinput "
        "WHERE videosource.sourceid = cardinput.sourceid AND"
        "      cardinput.cardid     = :CARDID");
    query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("get_use_eit", query);
        return false;
    }
    else if (query.next())
        return query.value(0).toBool();
    return false;
}

static bool is_dishnet_eit(uint cardid)
{
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT SUM(dishnet_eit) "
        "FROM videosource, cardinput "
        "WHERE videosource.sourceid = cardinput.sourceid AND"
        "      cardinput.cardid     = :CARDID");
    query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("is_dishnet_eit", query);
        return false;
    }
    else if (query.next())
        return query.value(0).toBool();
    return false;
}

static int no_capturecards(uint cardid)
{
    MSqlQuery query(MSqlQuery::InitCon());

    QString str =
        "SELECT COUNT(cardid) "
        "FROM capturecard ";

    if (cardid)
        str += "WHERE cardid < :CARDID";

    query.prepare(str);

    if (cardid)
        query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("no_capturecards", query);
        return -1;
    }
    else if (query.next())
        return query.value(0).toInt();
    return -1;
}

/** \fn TVRec::RunTV(void)
 *  \brief Event handling method, contains event loop.
 */
void TVRec::RunTV(void)
{
    QMutexLocker lock(&stateChangeLock);
    SetFlags(kFlagRunMainLoop);
    ClearFlags(kFlagExitPlayer | kFlagFinishRecording);

    eitScanStartTime = QDateTime::currentDateTime();
    // check whether we should use the EITScanner in this TVRec instance
    if (CardUtil::IsEITCapable(genOpt.cardtype) &&
        (!GetDVBChannel() || GetDVBChannel()->IsMaster()))
    {
        scanner = new EITScanner(cardid);
        uint timeout = eitCrawlIdleStart;
        // get the number of capture cards and the position of the current card
        // to distribute the the scan start evenly over eitTransportTimeout
        int card_pos = no_capturecards(cardid);
        int no_cards = no_capturecards(0);
        if (no_cards > 0 && card_pos >= 0)
            timeout += eitTransportTimeout * card_pos / no_cards;
        else
            timeout += random() % eitTransportTimeout;

        eitScanStartTime = eitScanStartTime.addSecs(timeout);
    }
    else
        eitScanStartTime = eitScanStartTime.addYears(1);

    while (HasFlags(kFlagRunMainLoop))
    {
        // If there is a state change queued up, do it...
        if (changeState)
        {
            HandleStateChange();
            ClearFlags(kFlagFrontendReady | kFlagCancelNextRecording);
        }

        // Quick exit on fatal errors.
        if (IsErrored())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    "RunTV encountered fatal error, exiting event thread.");
            ClearFlags(kFlagRunMainLoop);
            return;
        }

        // Handle any tuning events..
        HandleTuning();

        // Tell frontends about pending recordings
        HandlePendingRecordings();

        // If we are recording a program, check if the recording is
        // over or someone has asked us to finish the recording.
        if (GetState() == kState_RecordingOnly &&
            (QDateTime::currentDateTime() > recordEndTime ||
             HasFlags(kFlagFinishRecording)))
        {
            ChangeState(kState_None);
            ClearFlags(kFlagFinishRecording);
        }

        if (curRecording)
        {
            curRecording->UpdateInUseMark();

            if (recorder)
            {
                recorder->SavePositionMap();

                // Check for recorder errors
                if (recorder->IsErrored())
                {
                    curRecording->SetRecordingStatus(rsFailed);

                    if (GetState() == kState_WatchingLiveTV)
                    {
                        QString message = QString("QUIT_LIVETV %1").arg(cardid);
                        MythEvent me(message);
                        gCoreContext->dispatch(me);
                    }
                    else
                        ChangeState(kState_None);
                }
            }
        }

        // Check for the end of the current program..
        if (GetState() == kState_WatchingLiveTV)
        {
            QDateTime now   = QDateTime::currentDateTime();
            bool has_finish = HasFlags(kFlagFinishRecording);
            bool has_rec    = pseudoLiveTVRecording;
            bool enable_ui  = true;

            pendingRecLock.lock();
            bool rec_soon   =
                pendingRecordings.find(cardid) != pendingRecordings.end();
            pendingRecLock.unlock();

            if (has_rec && (has_finish || (now > recordEndTime)))
            {
                if (pseudoLiveTVRecording && curRecording)
                {
                    int secsSince = curRecording->GetRecordingStartTime()
                        .secsTo(QDateTime::currentDateTime());
                    if (secsSince < 120)
                    {
                        JobQueue::RemoveJobsFromMask(JOB_COMMFLAG,
                            autoRunJobs);
                        JobQueue::RemoveJobsFromMask(JOB_TRANSCODE,
                            autoRunJobs);
                    }

                    if (autoRunJobs)
                    {
                        JobQueue::QueueRecordingJobs(
                            *curRecording, autoRunJobs);
                    }
                }

                SetPseudoLiveTVRecording(NULL);
            }
            else if (!has_rec && !rec_soon && curRecording &&
                     (now >= curRecording->GetScheduledEndTime()))
            {
                if (!m_switchingBuffer)
                {
                    m_switchingBuffer = true;

                    SwitchLiveTVRingBuffer(channel->GetCurrentName(),
                                           false, true);

                    QDateTime starttime; starttime.setTime_t(0);
                    if (curRecording)
                        starttime = curRecording->GetRecordingStartTime();

                    VERBOSE(VB_RECORD, LOC
                            <<"!has_rec("<<!has_rec<<") "
                            <<"!rec_soon("<<!rec_soon<<") "
                            <<"curRec("<<curRecording<<") "
                            <<"starttm("
                            <<starttime.toString(Qt::ISODate)<<")");
                }
                else
                {
                    VERBOSE(VB_RECORD, "Waiting for ringbuffer switch");
                }
            }
            else
                enable_ui = false;

            if (enable_ui)
            {
                VERBOSE(VB_RECORD, LOC + "Enabling Full LiveTV UI.");
                QString message = QString("LIVETV_WATCH %1 0").arg(cardid);
                MythEvent me(message);
                gCoreContext->dispatch(me);
            }
        }

        // Check for ExitPlayer flag, and if set change to a non-watching
        // state (either kState_RecordingOnly or kState_None).
        if (HasFlags(kFlagExitPlayer))
        {
            if (internalState == kState_WatchingLiveTV)
                ChangeState(kState_None);
            else if (StateIsPlaying(internalState))
                ChangeState(RemovePlaying(internalState));
            ClearFlags(kFlagExitPlayer);
        }

        if (channel && scanner &&
            QDateTime::currentDateTime() > eitScanStartTime)
        {
            if (!dvbOpt.dvb_eitscan)
            {
                VERBOSE(VB_EIT, LOC + "EIT scanning disabled for this card.");
                eitScanStartTime = eitScanStartTime.addYears(1);
            }
            else if (!get_use_eit(GetCaptureCardNum()))
            {
                VERBOSE(VB_EIT, LOC + "EIT scanning disabled "
                        "for all sources on this card.");
                eitScanStartTime = eitScanStartTime.addYears(1);
            }
            else
            {
                scanner->StartActiveScan(this, eitTransportTimeout);
                SetFlags(kFlagEITScannerRunning);
                eitScanStartTime = QDateTime::currentDateTime().addYears(1);
            }
        }

        // We should be no more than a few thousand milliseconds,
        // as the end recording code does not have a trigger...
        // NOTE: If you change anything here, make sure that
        // WaitforEventThreadSleep() will still work...
        if (tuningRequests.empty() && !changeState)
        {
            lock.unlock(); // stateChangeLock

            {
                QMutexLocker locker(&triggerEventSleepLock);
                triggerEventSleepSignal = true;
                triggerEventSleepWait.wakeAll();
            }

            sched_yield();

            {
                QMutexLocker locker(&triggerEventLoopLock);
                // We check triggerEventLoopSignal because it is possible
                // that WakeEventLoop() was called since we
                // unlocked the stateChangeLock
                if (!triggerEventLoopSignal)
                {
                    triggerEventLoopWait.wait(
                        &triggerEventLoopLock, 1000 /* ms */);
                }
                triggerEventLoopSignal = false;
            }

            lock.relock(); // stateChangeLock
        }
    }

    if (GetState() != kState_None)
    {
        ChangeState(kState_None);
        HandleStateChange();
    }
}

/** \fn TVRec::WaitForEventThreadSleep(bool wake, ulong time)
 *
 *  You MUST HAVE the stateChange-lock locked when you call this method!
 */

bool TVRec::WaitForEventThreadSleep(bool wake, ulong time)
{
    bool ok = false;
    MythTimer t;
    t.start();

    while (!ok && ((unsigned long) t.elapsed()) < time)
    {
        if (wake)
            WakeEventLoop();

        stateChangeLock.unlock();

        sched_yield();

        {
            QMutexLocker locker(&triggerEventSleepLock);
            if (!triggerEventSleepSignal)
                triggerEventSleepWait.wait(&triggerEventSleepLock);
            triggerEventSleepSignal = false;
        }

        stateChangeLock.lock();

        // verify that we were triggered.
        ok = (tuningRequests.empty() && !changeState);
    }
    return ok;
}

void TVRec::HandlePendingRecordings(void)
{
    QMutexLocker pendlock(&pendingRecLock);

    if (pendingRecordings.empty())
        return;

    // If we have a pending recording and AskAllowRecording
    // or DoNotAskAllowRecording is set and the frontend is
    // ready send an ASK_RECORDING query to frontend.

    PendingMap::iterator it, next;

    for (it = pendingRecordings.begin(); it != pendingRecordings.end();)
    {
        next = it; ++next;
        if (QDateTime::currentDateTime() > (*it).recordingStart.addSecs(30))
        {
            VERBOSE(VB_RECORD, LOC + "Deleting stale pending recording " +
                    QString("%1 '%2'")
                    .arg((*it).info->GetCardID())
                    .arg((*it).info->GetTitle()));

            delete (*it).info;
            pendingRecordings.erase(it);
        }
        it = next;
    }

    bool has_rec = false;
    it = pendingRecordings.begin();
    if ((1 == pendingRecordings.size()) &&
        (*it).ask &&
        ((*it).info->GetCardID() == cardid) &&
        (GetState() == kState_WatchingLiveTV))
    {
        CheckForRecGroupChange();
        has_rec = pseudoLiveTVRecording &&
            (pseudoLiveTVRecording->GetRecordingEndTime() >
             (*it).recordingStart);
    }

    for (it = pendingRecordings.begin(); it != pendingRecordings.end(); ++it)
    {
        if (!(*it).ask && !(*it).doNotAsk)
            continue;

        int timeuntil = ((*it).doNotAsk) ?
            -1: QDateTime::currentDateTime().secsTo((*it).recordingStart);

        if (has_rec)
            (*it).canceled = true;

        QString query = QString("ASK_RECORDING %1 %2 %3 %4")
            .arg(cardid)
            .arg(timeuntil)
            .arg(has_rec ? 1 : 0)
            .arg((*it).hasLaterShowing ? 1 : 0);

        VERBOSE(VB_IMPORTANT, LOC + query);

        QStringList msg;
        (*it).info->ToStringList(msg);
        MythEvent me(query, msg);
        gCoreContext->dispatch(me);

        (*it).ask = (*it).doNotAsk = false;
    }
}

bool TVRec::GetDevices(uint cardid,
                       GeneralDBOptions   &gen_opts,
                       DVBDBOptions       &dvb_opts,
                       FireWireDBOptions  &firewire_opts)
{
    int testnum = 0;
    QString test;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT videodevice,      vbidevice,           audiodevice,     "
        "       audioratelimit,   defaultinput,        cardtype,        "
        "       skipbtaudio,      signal_timeout,      channel_timeout, "
        "       dvb_wait_for_seqstart, "
        ""
        "       dvb_on_demand,    dvb_tuning_delay,    dvb_eitscan,"
        ""
        "       firewire_speed,   firewire_model,      firewire_connection "
        ""
        "FROM capturecard "
        "WHERE cardid = :CARDID");
    query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("getdevices", query);
        return false;
    }

    if (!query.next())
        return false;

    // General options
    test = query.value(0).toString();
    if (test != QString::null)
        gen_opts.videodev = test;

    test = query.value(1).toString();
    if (test != QString::null)
        gen_opts.vbidev = test;

    test = query.value(2).toString();
    if (test != QString::null)
        gen_opts.audiodev = test;

    gen_opts.audiosamplerate = max(testnum, query.value(3).toInt());

    test = query.value(4).toString();
    if (test != QString::null)
        gen_opts.defaultinput = test;

    test = query.value(5).toString();
    if (test != QString::null)
        gen_opts.cardtype = test;

    gen_opts.skip_btaudio = query.value(6).toUInt();

    gen_opts.signal_timeout  = (uint) max(query.value(7).toInt(), 0);
    gen_opts.channel_timeout = (uint) max(query.value(8).toInt(), 0);

    // We should have at least 100 ms to acquire tables...
    int table_timeout = ((int)gen_opts.channel_timeout -
                         (int)gen_opts.signal_timeout);
    if (table_timeout < 100)
        gen_opts.channel_timeout = gen_opts.signal_timeout + 2500;

    gen_opts.wait_for_seqstart = query.value(9).toUInt();

    // DVB options
    uint dvboff = 10;
    dvb_opts.dvb_on_demand    = query.value(dvboff + 0).toUInt();
    dvb_opts.dvb_tuning_delay = query.value(dvboff + 1).toUInt();
    dvb_opts.dvb_eitscan      = query.value(dvboff + 2).toUInt();

    // Firewire options
    uint fireoff = dvboff + 3;
    firewire_opts.speed       = query.value(fireoff + 0).toUInt();

    test = query.value(fireoff + 1).toString();
    if (test != QString::null)
        firewire_opts.model = test;

    firewire_opts.connection  = query.value(fireoff + 2).toUInt();

    return true;
}

QString TVRec::GetStartChannel(uint cardid, const QString &defaultinput)
{
    QString startchan = QString::null;

    // Get last tuned channel from database, to use as starting channel
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT startchan "
        "FROM cardinput "
        "WHERE cardinput.cardid   = :CARDID    AND "
        "      inputname          = :INPUTNAME");
    query.bindValue(":CARDID",    cardid);
    query.bindValue(":INPUTNAME", defaultinput);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("getstartchan", query);
    }
    else if (query.next())
    {
        startchan = query.value(0).toString();
        if (!startchan.isEmpty())
        {
            VERBOSE(VB_CHANNEL, LOC + QString("Start channel: %1.")
                    .arg(startchan));
            return startchan;
        }
    }

    // If we failed to get the last tuned channel,
    // get a valid channel on our current input.
    query.prepare(
        "SELECT channum "
        "FROM capturecard, cardinput, channel "
        "WHERE capturecard.cardid = cardinput.cardid   AND "
        "      channel.sourceid   = cardinput.sourceid AND "
        "      capturecard.cardid = :CARDID AND "
        "      inputname          = :INPUTNAME");
    query.bindValue(":CARDID",    cardid);
    query.bindValue(":INPUTNAME", defaultinput);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("getstartchan2", query);
    }
    while (query.next())
    {
        startchan = query.value(0).toString();
        if (!startchan.isEmpty())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + QString("Start channel from DB is "
                    "empty, setting to '%1' instead.").arg(startchan));
            return startchan;
        }
    }

    // If we failed to get a channel on our current input,
    // widen search to any input.
    query.prepare(
        "SELECT channum, inputname "
        "FROM capturecard, cardinput, channel "
        "WHERE capturecard.cardid = cardinput.cardid   AND "
        "      channel.sourceid   = cardinput.sourceid AND "
        "      capturecard.cardid = :CARDID");
    query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("getstartchan3", query);
    }
    while (query.next())
    {
        startchan = query.value(0).toString();
        if (!startchan.isEmpty())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + QString("Start channel invalid, "
                    "setting to '%1' on input %2 instead.").arg(startchan)
                    .arg(query.value(1).toString()));
            return startchan;
        }
    }

    // If there are no valid channels, just use a random channel
    startchan = "3";
    VERBOSE(VB_IMPORTANT, LOC_ERR + QString("Problem finding starting channel, "
            "setting to default of '%1'.").arg(startchan));
    return startchan;
}

static void GetPidsToCache(DTVSignalMonitor *dtvMon, pid_cache_t &pid_cache)
{
    if (!dtvMon->GetATSCStreamData())
        return;

    const MasterGuideTable *mgt = dtvMon->GetATSCStreamData()->GetCachedMGT();
    if (!mgt)
        return;

    for (uint i = 0; i < mgt->TableCount(); ++i)
    {
        pid_cache_item_t item(mgt->TablePID(i), mgt->TableType(i));
        pid_cache.push_back(item);
    }
    dtvMon->GetATSCStreamData()->ReturnCachedTable(mgt);
}

static bool ApplyCachedPids(DTVSignalMonitor *dtvMon, const DTVChannel* channel)
{
    pid_cache_t pid_cache;
    channel->GetCachedPids(pid_cache);
    pid_cache_t::const_iterator it = pid_cache.begin();
    bool vctpid_cached = false;
    for (; it != pid_cache.end(); ++it)
    {
        if ((it->second == TableID::TVCT) ||
            (it->second == TableID::CVCT))
        {
            vctpid_cached = true;
            dtvMon->GetATSCStreamData()->AddListeningPID(it->first);
        }
    }
    return vctpid_cached;
}

/** \fn bool TVRec::SetupDTVSignalMonitor(void)
 *  \brief Tells DTVSignalMonitor what channel to look for.
 *
 *   If the major and minor channels are set we tell the signal
 *   monitor to look for those in the VCT.
 *
 *   Otherwise, we tell the signal monitor to look for the MPEG
 *   program number in the PAT.
 *
 *   This method also grabs the ATSCStreamData() from the recorder
 *   if possible, or creates one if needed.
 */
bool TVRec::SetupDTVSignalMonitor(bool EITscan)
{
    VERBOSE(VB_RECORD, LOC + "Setting up table monitoring.");

    DTVSignalMonitor *sm = GetDTVSignalMonitor();
    DTVChannel *dtvchan = GetDTVChannel();
    if (!sm || !dtvchan)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Setting up table monitoring.");
        return false;
    }

    MPEGStreamData *sd = NULL;
    if (GetDTVRecorder())
    {
        sd = GetDTVRecorder()->GetStreamData();
        sd->SetCaching(true);
    }

    QString recording_type = "all";
    RecordingInfo *rec = lastTuningRequest.program;
    RecordingProfile profile;
    load_profile(genOpt.cardtype, tvchain, rec, profile);
    const Setting *setting = profile.byName("recordingtype");
    if (setting)
        recording_type = setting->getValue();

    const QString tuningmode = dtvchan->GetTuningMode();

    // Check if this is an ATSC Channel
    int major = dtvchan->GetMajorChannel();
    int minor = dtvchan->GetMinorChannel();
    if ((minor > 0) && (tuningmode == "atsc"))
    {
        QString msg = QString("ATSC channel: %1_%2").arg(major).arg(minor);
        VERBOSE(VB_RECORD, LOC + msg);

        ATSCStreamData *asd = dynamic_cast<ATSCStreamData*>(sd);
        if (!asd)
        {
            sd = asd = new ATSCStreamData(major, minor);
            sd->SetCaching(true);
            if (GetDTVRecorder())
                GetDTVRecorder()->SetStreamData(asd);
        }

        asd->Reset();
        sm->SetStreamData(sd);
        sm->SetChannel(major, minor);
        sd->SetRecordingType(recording_type);

        // Try to get pid of VCT from cache and
        // require MGT if we don't have VCT pid.
        if (!ApplyCachedPids(sm, dtvchan))
            sm->AddFlags(SignalMonitor::kDTVSigMon_WaitForMGT);

        VERBOSE(VB_RECORD, LOC + "Successfully set up ATSC table monitoring.");
        return true;
    }

    // Check if this is an DVB channel
    int progNum = dtvchan->GetProgramNumber();
#ifdef USING_DVB
    if ((progNum >= 0) && (tuningmode == "dvb"))
    {
        int netid   = dtvchan->GetOriginalNetworkID();
        int tsid    = dtvchan->GetTransportID();

        DVBStreamData *dsd = dynamic_cast<DVBStreamData*>(sd);
        if (!dsd)
        {
            if (GetDVBChannel())
            {
                DVBKind dvbkind;
                if (GetDVBChannel()->GetFrontendName().indexOf("ISDB") >= 0)
                    dvbkind = kKindISDB;
                else
                    dvbkind = kKindDVB;
                sd = dsd = new DVBStreamData(netid, tsid, progNum, false, dvbkind);
            }
            else
                sd = dsd = new DVBStreamData(netid, tsid, progNum);
            sd->SetCaching(true);
            if (GetDTVRecorder())
                GetDTVRecorder()->SetStreamData(dsd);
        }

        VERBOSE(VB_RECORD, LOC +
                QString("DVB service_id %1 on net_id %2 tsid %3")
                .arg(progNum).arg(netid).arg(tsid));

        // Some DVB devices munge the PMT and/or PAT so the CRC check fails.
        // We need to tell the stream data class to not check the CRC on
        // these devices.
        if (GetDVBChannel())
            sd->SetIgnoreCRC(GetDVBChannel()->HasCRCBug());

        dsd->Reset();
        sm->SetStreamData(sd);
        sm->SetDVBService(netid, tsid, progNum);
        sd->SetRecordingType(recording_type);

        sm->AddFlags(SignalMonitor::kDTVSigMon_WaitForPMT |
                     SignalMonitor::kDTVSigMon_WaitForSDT |
                     SignalMonitor::kDVBSigMon_WaitForPos);
        sm->SetRotorTarget(1.0f);

        VERBOSE(VB_RECORD, LOC + "Successfully set up DVB table monitoring.");
        return true;
    }
#endif // USING_DVB

    // Check if this is an MPEG channel
    if (progNum >= 0)
    {
        if (!sd)
        {
            sd = new MPEGStreamData(progNum, true);
            sd->SetCaching(true);
            if (GetDTVRecorder())
                GetDTVRecorder()->SetStreamData(sd);
        }

        QString msg = QString("MPEG program number: %1").arg(progNum);
        VERBOSE(VB_RECORD, LOC + msg);

#ifdef USING_DVB
        // Some DVB devices munge the PMT and/or PAT so the CRC check fails.
        // We need to tell the stream data class to not check the CRC on
        // these devices.
        if (GetDVBChannel())
            sd->SetIgnoreCRC(GetDVBChannel()->HasCRCBug());
#endif // USING_DVB

        sd->Reset();
        sm->SetStreamData(sd);
        sm->SetProgramNumber(progNum);
        sd->SetRecordingType(recording_type);

        sm->AddFlags(SignalMonitor::kDTVSigMon_WaitForPAT |
                     SignalMonitor::kDTVSigMon_WaitForPMT |
                     SignalMonitor::kDVBSigMon_WaitForPos);
        sm->SetRotorTarget(1.0f);

        if (EITscan)
        {
            sm->GetStreamData()->SetVideoStreamsRequired(0);
            sm->IgnoreEncrypted(true);
        }

        VERBOSE(VB_RECORD, LOC + "Successfully set up MPEG table monitoring.");
        return true;
    }

    QString msg = "No valid DTV info, ATSC maj(%1) min(%2), MPEG pn(%3)";
    VERBOSE(VB_IMPORTANT, LOC_ERR + msg.arg(major).arg(minor).arg(progNum));
    return false;
}

/** \fn TVRec::SetupSignalMonitor(bool,bool)
 *  \brief This creates a SignalMonitor instance and
 *         begins signal monitoring.
 *
 *   If the channel exists a SignalMonitor instance is created and
 *   SignalMonitor::Start() is called to start the signal monitoring thread.
 *
 *  \param tablemon If set we enable table monitoring
 *  \param notify   If set we notify the frontend of the signal values
 *  \return true on success, false on failure
 */
bool TVRec::SetupSignalMonitor(bool tablemon, bool EITscan, bool notify)
{
    VERBOSE(VB_RECORD, LOC + "SetupSignalMonitor("
            <<tablemon<<", "<<notify<<")");

    // if it already exists, there no need to initialize it
    if (signalMonitor)
        return true;

    // if there is no channel object we can't monitor it
    if (!channel)
        return false;

    // nothing to monitor here either (DummyChannel)
    if (genOpt.cardtype == "IMPORT" || genOpt.cardtype == "DEMO")
        return true;

    // make sure statics are initialized
    SignalMonitorValue::Init();

    if (channel->Open())
        signalMonitor = SignalMonitor::Init(genOpt.cardtype, cardid,
                                            channel);

    if (signalMonitor)
    {
        VERBOSE(VB_RECORD, LOC + "Signal monitor successfully created");

        signalMonitor->SetMonitoring(this, EITscan,
                                     GetDTVSignalMonitor() && tablemon);
        signalMonitor->AddListener(this);
        signalMonitor->SetUpdateRate(kSignalMonitoringRate);
        signalMonitor->SetNotifyFrontend(notify);

        // Start the monitoring thread
        signalMonitor->Start(true);
    }

    return true;
}

/** \fn TVRec::TeardownSignalMonitor()
 *  \brief If a SignalMonitor instance exists, the monitoring thread is
 *         stopped and the instance is deleted.
 */
void TVRec::TeardownSignalMonitor()
{
    if (!signalMonitor)
        return;

    VERBOSE(VB_RECORD, LOC + "TeardownSignalMonitor() -- begin");

    // If this is a DTV signal monitor, save any pids we know about.
    DTVSignalMonitor *dtvMon  = GetDTVSignalMonitor();
    DTVChannel       *dtvChan = GetDTVChannel();
    if (dtvMon && dtvChan)
    {
        pid_cache_t pid_cache;
        GetPidsToCache(dtvMon, pid_cache);
        if (pid_cache.size())
            dtvChan->SaveCachedPids(pid_cache);
    }

    if (signalMonitor)
    {
        delete signalMonitor;
        signalMonitor = NULL;
    }

    VERBOSE(VB_RECORD, LOC + "TeardownSignalMonitor() -- end");
}

/** \fn TVRec::SetSignalMonitoringRate(int,int)
 *  \brief Sets the signal monitoring rate.
 *
 *  \sa EncoderLink::SetSignalMonitoringRate(int,int),
 *      RemoteEncoder::SetSignalMonitoringRate(int,int)
 *  \param rate           The update rate to use in milliseconds,
 *                        use 0 to disable signal monitoring.
 *  \param notifyFrontend If 1, SIGNAL messages will be sent to
 *                        the frontend using this recorder.
 *  \return 1 if it signal monitoring is turned on, 0 otherwise.
 */
int TVRec::SetSignalMonitoringRate(int rate, int notifyFrontend)
{
    QString msg = "SetSignalMonitoringRate(%1, %2)";
    VERBOSE(VB_RECORD, LOC + msg.arg(rate).arg(notifyFrontend) + "-- start");

    QMutexLocker lock(&stateChangeLock);

    if (!SignalMonitor::IsSupported(genOpt.cardtype))
    {
        VERBOSE(VB_IMPORTANT, LOC + "Signal Monitoring is not"
                "supported by your hardware.");
        return 0;
    }

    if (GetState() != kState_WatchingLiveTV)
    {
        VERBOSE(VB_IMPORTANT, LOC + "Signal can only "
                "be monitored in LiveTV Mode.");
        return 0;
    }

    ClearFlags(kFlagRingBufferReady);

    TuningRequest req = (rate > 0) ?
        TuningRequest(kFlagAntennaAdjust, channel->GetCurrentName()) :
        TuningRequest(kFlagLiveTV);

    tuningRequests.enqueue(req);

    // Wait for RingBuffer reset
    while (!HasFlags(kFlagRingBufferReady))
        WaitForEventThreadSleep();
    VERBOSE(VB_RECORD, LOC + msg.arg(rate).arg(notifyFrontend) + " -- end");
    return 1;
}

DTVSignalMonitor *TVRec::GetDTVSignalMonitor(void)
{
    return dynamic_cast<DTVSignalMonitor*>(signalMonitor);
}

/** \fn TVRec::ShouldSwitchToAnotherCard(QString)
 *  \brief Checks if named channel exists on current tuner, or
 *         another tuner.
 *
 *  \param chanid channel to verify against tuners.
 *  \return true if the channel on another tuner and not current tuner,
 *          false otherwise.
 *  \sa EncoderLink::ShouldSwitchToAnotherCard(const QString&),
 *      RemoteEncoder::ShouldSwitchToAnotherCard(QString),
 *      CheckChannel(QString)
 */
bool TVRec::ShouldSwitchToAnotherCard(QString chanid)
{
    QString msg("");
    MSqlQuery query(MSqlQuery::InitCon());

    if (!query.isConnected())
        return false;

    query.prepare("SELECT channel.channum, channel.callsign "
                  "FROM channel "
                  "WHERE channel.chanid = :CHANID");
    query.bindValue(":CHANID", chanid);
    if (!query.exec() || !query.isActive() || query.size() == 0)
    {
        MythDB::DBError("ShouldSwitchToAnotherCard", query);
        return false;
    }

    query.next();
    QString channelname = query.value(0).toString();
    QString callsign = query.value(1).toString();

    query.prepare(
        "SELECT channel.channum "
        "FROM channel,cardinput "
        "WHERE ( channel.chanid = :CHANID OR             "
        "        ( channel.channum  = :CHANNUM AND       "
        "          channel.callsign = :CALLSIGN    )     "
        "      )                                     AND "
        "      channel.sourceid = cardinput.sourceid AND "
        "      cardinput.cardid = :CARDID");
    query.bindValue(":CHANID", chanid);
    query.bindValue(":CHANNUM", channelname);
    query.bindValue(":CALLSIGN", callsign);
    query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("ShouldSwitchToAnotherCard", query);
    }
    else if (query.size() > 0)
    {
        msg = "Found channel (%1) on current card(%2).";
        VERBOSE(VB_RECORD, LOC + msg.arg(channelname).arg(cardid));
        return false;
    }

    // We didn't find it on the current card, so now we check other cards.
    query.prepare(
        "SELECT channel.channum, cardinput.cardid "
        "FROM channel,cardinput "
        "WHERE ( channel.chanid = :CHANID OR              "
        "        ( channel.channum  = :CHANNUM AND        "
        "          channel.callsign = :CALLSIGN    )      "
        "      )                                      AND "
        "      channel.sourceid  = cardinput.sourceid AND "
        "      cardinput.cardid != :CARDID");
    query.bindValue(":CHANID", chanid);
    query.bindValue(":CHANNUM", channelname);
    query.bindValue(":CALLSIGN", callsign);
    query.bindValue(":CARDID", cardid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("ShouldSwitchToAnotherCard", query);
    }
    else if (query.next())
    {
        msg = QString("Found channel (%1) on different card(%2).")
            .arg(query.value(0).toString()).arg(query.value(1).toString());
        VERBOSE(VB_RECORD, LOC + msg);
        return true;
    }

    msg = QString("Did not find channel(%1) on any card.").arg(channelname);
    VERBOSE(VB_RECORD, LOC + msg);
    return false;
}

/** \fn TVRec::CheckChannel(QString) const
 *  \brief Checks if named channel exists on current tuner.
 *
 *  \param name channel to verify against current tuner.
 *  \return true if it succeeds, false otherwise.
 *  \sa EncoderLink::CheckChannel(const QString&),
 *      RemoteEncoder::CheckChannel(QString),
 *      CheckChannel(ChannelBase*,const QString&,QString&),
 *      ShouldSwitchToAnotherCard(QString)
 */
bool TVRec::CheckChannel(QString name) const
{
    if (!channel)
        return false;

    QString dummyID;
    return channel->CheckChannel(name, dummyID);
}

/** \fn QString add_spacer(const QString&, const QString&)
 *  \brief Adds the spacer before the last character in chan.
 */
static QString add_spacer(const QString &channel, const QString &spacer)
{
    QString chan = channel;
    chan.detach();
    if ((chan.length() >= 2) && !spacer.isEmpty())
        return chan.left(chan.length()-1) + spacer + chan.right(1);
    return chan;
}

/** \fn TVRec::CheckChannelPrefix(const QString&,uint&,bool&,QString&)
 *  \brief Checks a prefix against the channels in the DB.
 *
 *   If the prefix matches a channel on any recorder this function returns
 *   true, otherwise it returns false.
 *
 *   If the prefix matches any channel entirely (i.e. prefix == channum),
 *   then the cardid of the recorder it matches is returned in
 *   'is_complete_valid_channel_on_rec'; if it matches multiple recorders,
 *   and one of them is this recorder, this recorder is returned in
 *   'is_complete_valid_channel_on_rec'; if it isn't complete for any channel
 *    on any recorder 'is_complete_valid_channel_on_rec' is set to zero.
 *
 *   If adding another character could reduce the number of channels the
 *   prefix matches 'is_extra_char_useful' is set to true, otherwise it
 *   is set to false.
 *
 *   Finally, if in order for the prefix to match a channel, a spacer needs
 *   to be added, the first matching spacer is returned in needed_spacer.
 *   If there is more than one spacer that might be employed and one of them
 *   is used for the current recorder, and others are used for other
 *   recorders, then the one for the current recorder is returned. The
 *   spacer must be inserted before the last character of the prefix for
 *   anything else returned from the function to be valid.
 *
 *  \return true if this is a valid prefix for a channel, false otherwise
 */
bool TVRec::CheckChannelPrefix(const QString &prefix,
                               uint          &is_complete_valid_channel_on_rec,
                               bool          &is_extra_char_useful,
                               QString       &needed_spacer)
{
#if DEBUG_CHANNEL_PREFIX
    VERBOSE(VB_IMPORTANT, QString("CheckChannelPrefix(%1)").arg(prefix));
#endif

    static const uint kSpacerListSize = 5;
    static const char* spacers[kSpacerListSize] = { "", "_", "-", "#", "." };

    MSqlQuery query(MSqlQuery::InitCon());
    QString basequery = QString(
        "SELECT channel.chanid, channel.channum, cardinput.cardid "
        "FROM channel, capturecard, cardinput "
        "WHERE channel.channum LIKE '%1%'            AND "
        "      channel.sourceid = cardinput.sourceid AND "
        "      cardinput.cardid = capturecard.cardid");

    QString cardquery[2] =
    {
        QString(" AND capturecard.cardid  = '%1'").arg(cardid),
        QString(" AND capturecard.cardid != '%1'").arg(cardid),
    };

    vector<uint>    fchanid;
    vector<QString> fchannum;
    vector<uint>    fcardid;
    vector<QString> fspacer;

    for (uint i = 0; i < 2; i++)
    {
        for (uint j = 0; j < kSpacerListSize; j++)
        {
            QString qprefix = add_spacer(
                prefix, (QString(spacers[j]) == "_") ? "\\_" : spacers[j]);
            query.prepare(basequery.arg(qprefix) + cardquery[i]);

            if (!query.exec() || !query.isActive())
            {
                MythDB::DBError("checkchannel -- locate channum", query);
            }
            else if (query.size())
            {
                while (query.next())
                {
                    fchanid.push_back(query.value(0).toUInt());
                    fchannum.push_back(query.value(1).toString());
                    fcardid.push_back(query.value(2).toUInt());
                    fspacer.push_back(spacers[j]);
#if DEBUG_CHANNEL_PREFIX
                    VERBOSE(VB_IMPORTANT, QString("(%1,%2) Adding %3 rec %4")
                            .arg(i).arg(j).arg(query.value(1).toString(),6)
                            .arg(query.value(2).toUInt()));
#endif
                }
            }

            if (prefix.length() < 2)
                break;
        }
    }

    // Now process the lists for the info we need...
    is_extra_char_useful = false;
    is_complete_valid_channel_on_rec = 0;
    needed_spacer = "";

    if (fchanid.size() == 0)
        return false;

    if (fchanid.size() == 1) // Unique channel...
    {
        needed_spacer = fspacer[0];
        bool nc       = (fchannum[0] != add_spacer(prefix, fspacer[0]));

        is_complete_valid_channel_on_rec = (nc) ? 0 : fcardid[0];
        is_extra_char_useful             = nc;
        return true;
    }

    // If we get this far there is more than one channel
    // sharing the prefix we were given.

    // Is an extra characher useful for disambiguation?
    is_extra_char_useful = false;
    for (uint i = 0; (i < fchannum.size()) && !is_extra_char_useful; i++)
    {
        is_extra_char_useful = (fchannum[i] != add_spacer(prefix, fspacer[i]));
#if DEBUG_CHANNEL_PREFIX
        VERBOSE(VB_IMPORTANT, "is_extra_char_useful("
                <<fchannum[i]<<"!="<<add_spacer(prefix, fspacer[i])
                <<"): "<<is_extra_char_useful);
#endif
    }

    // Are any of the channels complete w/o spacer?
    // If so set is_complete_valid_channel_on_rec,
    // with a preference for our cardid.
    for (uint i = 0; i < fchannum.size(); i++)
    {
        if (fchannum[i] == prefix)
        {
            is_complete_valid_channel_on_rec = fcardid[i];
            if (fcardid[i] == (uint)cardid)
                break;
        }
    }

    if (is_complete_valid_channel_on_rec)
        return true;

    // Add a spacer, if one is needed to select a valid channel.
    bool spacer_needed = true;
    for (uint i = 0; (i < fspacer.size() && spacer_needed); i++)
        spacer_needed = !fspacer[i].isEmpty();
    if (spacer_needed)
        needed_spacer = fspacer[0];

    // If it isn't useful to wait for more characters,
    // then try to commit to any true match immediately.
    for (uint i = 0; i < ((is_extra_char_useful) ? 0 : fchanid.size()); i++)
    {
        if (fchannum[i] == add_spacer(prefix, fspacer[i]))
        {
            needed_spacer = fspacer[i];
            is_complete_valid_channel_on_rec = fcardid[i];
            return true;
        }
    }

    return true;
}

bool TVRec::SetVideoFiltersForChannel(uint  sourceid,
                                      const QString &channum)
{
    if (!recorder)
        return false;

    QString videoFilters = ChannelUtil::GetVideoFilters(sourceid, channum);
    if (!videoFilters.isEmpty())
    {
        recorder->SetVideoFilters(videoFilters);
        return true;
    }

    return false;
}

/** \fn TVRec::IsReallyRecording()
 *  \brief Returns true if frontend can consider the recorder started.
 *  \sa IsRecording()
 */
bool TVRec::IsReallyRecording(void)
{
    return ((recorder && recorder->IsRecording()) ||
            HasFlags(kFlagDummyRecorderRunning));
}

/** \fn TVRec::IsBusy(TunedInputInfo*,int) const
 *  \brief Returns true if the recorder is busy, or will be within
 *         the next time_buffer seconds.
 *  \sa EncoderLink::IsBusy(TunedInputInfo*, int time_buffer)
 */
bool TVRec::IsBusy(TunedInputInfo *busy_input, int time_buffer) const
{
    TunedInputInfo dummy;
    if (!busy_input)
        busy_input = &dummy;

    busy_input->Clear();

    if (!channel)
        return false;

    QStringList list = channel->GetConnectedInputs();
    if (list.empty())
        return false;

    uint chanid = 0;

    if (GetState() != kState_None)
    {
        busy_input->inputid = channel->GetCurrentInputNum();
        chanid              = channel->GetChanID();
    }

    PendingInfo pendinfo;
    bool        has_pending;
    {
        pendingRecLock.lock();
        PendingMap::const_iterator it = pendingRecordings.find(cardid);
        has_pending = (it != pendingRecordings.end());
        if (has_pending)
            pendinfo = *it;
        pendingRecLock.unlock();
    }

    if (!busy_input->inputid && has_pending)
    {
        int timeLeft = QDateTime::currentDateTime()
            .secsTo(pendinfo.recordingStart);

        if (timeLeft <= time_buffer)
        {
            QString channum = QString::null, input = QString::null;
            if (pendinfo.info->QueryTuningInfo(channum, input))
            {
                busy_input->inputid = channel->GetInputByName(input);
                chanid = pendinfo.info->GetChanID();
            }
        }
    }

    if (busy_input->inputid)
    {
        CardUtil::GetInputInfo(*busy_input);
        busy_input->chanid  = chanid;
        busy_input->mplexid = ChannelUtil::GetMplexID(busy_input->chanid);
        busy_input->mplexid =
            (32767 == busy_input->mplexid) ? 0 : busy_input->mplexid;
    }

    return busy_input->inputid;
}


/** \fn TVRec::GetFramerate()
 *  \brief Returns recordering frame rate from the recorder.
 *  \sa RemoteEncoder::GetFrameRate(), EncoderLink::GetFramerate(void),
 *      RecorderBase::GetFrameRate()
 *  \return Frames per second if query succeeds -1 otherwise.
 */
float TVRec::GetFramerate(void)
{
    QMutexLocker lock(&stateChangeLock);

    if (recorder)
        return recorder->GetFrameRate();
    return -1.0f;
}

/** \fn TVRec::GetFramesWritten()
 *  \brief Returns number of frames written to disk by recorder.
 *
 *  \sa EncoderLink::GetFramesWritten(), RemoteEncoder::GetFramesWritten()
 *  \return Number of frames if query succeeds, -1 otherwise.
 */
long long TVRec::GetFramesWritten(void)
{
    QMutexLocker lock(&stateChangeLock);

    if (recorder)
        return recorder->GetFramesWritten();
    return -1;
}

/** \fn TVRec::GetFilePosition()
 *  \brief Returns total number of bytes written by RingBuffer.
 *
 *  \sa EncoderLink::GetFilePosition(), RemoteEncoder::GetFilePosition()
 *  \return Bytes written if query succeeds, -1 otherwise.
 */
long long TVRec::GetFilePosition(void)
{
    QMutexLocker lock(&stateChangeLock);

    if (ringBuffer)
        return ringBuffer->GetWritePosition();
    return -1;
}

/** \brief Returns byte position in RingBuffer
 *         of a keyframe according to recorder.
 *
 *  \sa EncoderLink::GetKeyframePosition(uint64_t),
 *      RemoteEncoder::GetKeyframePosition(uint64_t)
 *  \return Byte position of keyframe if query succeeds, -1 otherwise.
 */
int64_t TVRec::GetKeyframePosition(uint64_t desired) const
{
    QMutexLocker lock(&stateChangeLock);

    if (recorder)
        return recorder->GetKeyframePosition(desired);
    return -1;
}

/**
 *  \brief Returns byte position in RingBuffer of a keyframes
 *         according to recorder.
 *
 *  \sa EncoderLink::GetKeyframePositions(int64_t, int64_t, frm_pos_map_t&),
 *      RemoteEncoder::GetKeyframePositions(int64_t, int64_t, frm_pos_map_t&)
 *  \return Byte position of keyframe if query succeeds, -1 otherwise.
 */
bool TVRec::GetKeyframePositions(
    int64_t start, int64_t end, frm_pos_map_t &map) const
{
    QMutexLocker lock(&stateChangeLock);

    if (recorder)
        return recorder->GetKeyframePositions(start, end, map);

    return false;
}

/** \fn TVRec::GetMaxBitrate(void) const
 *  \brief Returns the maximum bits per second this recorder can produce.
 *
 *  \sa EncoderLink::GetMaxBitrate(void), RemoteEncoder::GetMaxBitrate(void)
 */
long long TVRec::GetMaxBitrate(void) const
{
    long long bitrate;
    if (genOpt.cardtype == "MPEG")
        bitrate = 10080000LL; // use DVD max bit rate
    if (genOpt.cardtype == "HDPVR")
        bitrate = 20200000LL; // Peek bit rate for HD-PVR
    else if (!CardUtil::IsEncoder(genOpt.cardtype))
        bitrate = 22200000LL; // 1080i
    else // frame grabber
        bitrate = 10080000LL; // use DVD max bit rate, probably too big

    return bitrate;
}

/** \fn TVRec::SpawnLiveTV(LiveTVChain*,bool,QString)
 *  \brief Tells TVRec to spawn a "Live TV" recorder.
 *  \sa EncoderLink::SpawnLiveTV(LiveTVChain*,bool,QString),
 *      RemoteEncoder::SpawnLiveTV(QString,bool,QSting)
 */
void TVRec::SpawnLiveTV(LiveTVChain *newchain, bool pip, QString startchan)
{
    QMutexLocker lock(&stateChangeLock);

    tvchain = newchain;
    tvchain->ReloadAll();

    QString hostprefix = QString("myth://%1:%2/")
                                .arg(gCoreContext->GetSetting("BackendServerIP"))
                                .arg(gCoreContext->GetSetting("BackendServerPort"));

    tvchain->SetHostPrefix(hostprefix);
    tvchain->SetCardType(genOpt.cardtype);

    ispip = pip;
    LiveTVStartChannel = startchan;

    // Change to WatchingLiveTV
    ChangeState(kState_WatchingLiveTV);
    // Wait for state change to take effect
    WaitForEventThreadSleep();

    // Make sure StartRecording can't steal our tuner
    SetFlags(kFlagCancelNextRecording);
}

/** \fn TVRec::GetChainID()
 *  \brief Get the chainid of the livetv instance
 */
QString TVRec::GetChainID(void)
{
    if (tvchain)
        return tvchain->GetID();
    return "";
}

/** \fn TVRec::CheckForRecGroupChange(void)
 *  \brief Check if frontend changed the recording group.
 *
 *   This is needed because the frontend may toggle whether something
 *   should be kept as a recording in the frontend, but this class may
 *   not find out about it in time unless we check the DB when this
 *   information is important.
 */
void TVRec::CheckForRecGroupChange(void)
{
    QMutexLocker lock(&stateChangeLock);

    if (internalState == kState_None)
        return; // already stopped

    if (!curRecording)
        return;

    const QString recgrp = curRecording->QueryRecordingGroup();
    curRecording->SetRecordingGroup(recgrp);

    if (recgrp != "LiveTV" && !pseudoLiveTVRecording)
    {
        // User wants this recording to continue
        SetPseudoLiveTVRecording(new ProgramInfo(*curRecording));
    }
    else if (recgrp == "LiveTV" && pseudoLiveTVRecording)
    {
        // User wants to abandon scheduled recording
        SetPseudoLiveTVRecording(NULL);
    }
}

static uint get_input_id(uint cardid, const QString &inputname)
{
    MSqlQuery query(MSqlQuery::InitCon());

    query.prepare(
        "SELECT cardinputid "
        "FROM cardinput "
        "WHERE cardid    = :CARDID AND "
        "      inputname = :INNAME");

    query.bindValue(":CARDID", cardid);
    query.bindValue(":INNAME", inputname);

    if (!query.exec() || !query.isActive())
        MythDB::DBError("get_input_id", query);
    else if (query.next())
        return query.value(0).toUInt();

    return 0;
}

/** \fn TVRec::NotifySchedulerOfRecording(RecordingInfo*)
 *  \brief Tell scheduler about the recording.
 *
 *   This is needed if the frontend has marked the LiveTV
 *   buffer for recording after we exit LiveTV. In this case
 *   the scheduler needs to know about the recording so it
 *   can properly take overrecord into account, and to properly
 *   reschedule other recordings around to avoid this recording.
 */
void TVRec::NotifySchedulerOfRecording(RecordingInfo *rec)
{
    if (!channel)
        return;

    // Notify scheduler of the recording.
    // + set up recording so it can be resumed
    rec->SetCardID(cardid);
    rec->SetInputID(get_input_id(cardid, channel->GetCurrentInput()));
    rec->SetRecordingRuleType(rec->GetRecordingRule()->m_type);

    if (rec->GetRecordingRuleType() == kNotRecording)
    {
        rec->SetRecordingRuleType(kSingleRecord);
        rec->GetRecordingRule()->m_type = kSingleRecord;
    }

    // + remove DefaultEndOffset which would mismatch the live session
    rec->GetRecordingRule()->m_endOffset = 0;

    // + save rsInactive recstatus to so that a reschedule call
    //   doesn't start recording this on another card before we
    //   send the SCHEDULER_ADD_RECORDING message to the scheduler.
    rec->SetRecordingStatus(rsInactive);
    rec->AddHistory(false);

    // + save RecordingRule so that we get a recordid
    //   (don't allow signalChange(), avoiding unneeded reschedule)
    rec->GetRecordingRule()->Save(false);

    // + save recordid to recorded entry
    rec->ApplyRecordRecID();

    // + set proper recstatus (saved later)
    rec->SetRecordingStatus(rsRecording);

    // + pass proginfo to scheduler and reschedule
    QStringList prog;
    rec->ToStringList(prog);
    MythEvent me("SCHEDULER_ADD_RECORDING", prog);
    gCoreContext->dispatch(me);

    // Allow scheduler to end this recording before post-roll,
    // if it has another recording for this recorder.
    ClearFlags(kFlagCancelNextRecording);
}

/** \fn TVRec::SetLiveRecording(int)
 *  \brief Tells the Scheduler about changes to the recording status
 *         of the LiveTV recording.
 *
 *   NOTE: Currently the 'recording' parameter is ignored and decisions
 *         are based on the recording group alone.
 *
 *  \param recording Set to 1 to mark as rsRecording, set to 0 to mark as
 *         rsCancelled, and set to -1 to base the decision of the recording
 *         group.
 */
void TVRec::SetLiveRecording(int recording)
{
    VERBOSE(VB_IMPORTANT, LOC + "SetLiveRecording("<<recording<<")");
    QMutexLocker locker(&stateChangeLock);

    (void) recording;

    RecStatusType recstat = rsCancelled;
    bool was_rec = pseudoLiveTVRecording;
    CheckForRecGroupChange();
    if (was_rec && !pseudoLiveTVRecording)
    {
        VERBOSE(VB_IMPORTANT, LOC + "SetLiveRecording() -- cancel");
        // cancel -- 'recording' should be 0 or -1
        SetFlags(kFlagCancelNextRecording);
        curRecording->SetRecordingGroup("LiveTV");
        autoRunJobs = JOB_NONE;
    }
    else if (!was_rec && pseudoLiveTVRecording)
    {
        VERBOSE(VB_IMPORTANT, LOC + "SetLiveRecording() -- record");
        // record -- 'recording' should be 1 or -1

        // If the last recording was flagged for keeping
        // in the frontend, then add the recording rule
        // so that transcode, commfrag, etc can be run.
        recordEndTime = GetRecordEndTime(pseudoLiveTVRecording);
        NotifySchedulerOfRecording(curRecording);
        recstat = curRecording->GetRecordingStatus();
        curRecording->SetRecordingGroup("Default");

        RecordingProfile profile;
        load_profile(genOpt.cardtype, NULL, curRecording, profile);
        autoRunJobs = init_jobs(curRecording, profile, runJobOnHostOnly,
                                transcodeFirst, earlyCommFlag);
    }

    MythEvent me(QString("UPDATE_RECORDING_STATUS %1 %2 %3 %4 %5")
                 .arg(curRecording->GetCardID())
                 .arg(curRecording->GetChanID())
                 .arg(curRecording->GetScheduledStartTime(ISODate))
                 .arg(recstat)
                 .arg(curRecording->GetRecordingEndTime(ISODate)));

    gCoreContext->dispatch(me);
}

/** \fn TVRec::StopLiveTV(void)
 *  \brief Tells TVRec to stop a "Live TV" recorder.
 *  \sa EncoderLink::StopLiveTV(), RemoteEncoder::StopLiveTV()
 */
void TVRec::StopLiveTV(void)
{
    QMutexLocker lock(&stateChangeLock);
    VERBOSE(VB_RECORD, LOC + "StopLiveTV(void) curRec: "<<curRecording
            <<" pseudoRec: "<<pseudoLiveTVRecording);

    if (internalState != kState_WatchingLiveTV)
        return;

    bool hadPseudoLiveTVRec = pseudoLiveTVRecording;
    CheckForRecGroupChange();

    if (!hadPseudoLiveTVRec && pseudoLiveTVRecording)
        NotifySchedulerOfRecording(curRecording);

    // Figure out next state and if needed recording end time.
    TVState next_state = kState_None;
    if (pseudoLiveTVRecording)
    {
        recordEndTime = GetRecordEndTime(pseudoLiveTVRecording);
        next_state = kState_RecordingOnly;
    }

    // Change to the appropriate state
    ChangeState(next_state);

    // Wait for state change to take effect...
    WaitForEventThreadSleep();

    // We are done with the tvchain...
    tvchain = NULL;
}

/** \fn TVRec::PauseRecorder(void)
 *  \brief Tells "recorder" to pause, used for channel and input changes.
 *
 *   When the RecorderBase instance has paused it calls RecorderPaused(void)
 *
 *  \sa EncoderLink::PauseRecorder(void), RemoteEncoder::PauseRecorder(void),
 *      RecorderBase::Pause(void)
 */
void TVRec::PauseRecorder(void)
{
    QMutexLocker lock(&stateChangeLock);

    if (!recorder)
    {
        VERBOSE(VB_IMPORTANT, LOC + "PauseRecorder() "
                "called with no recorder");
        return;
    }

    recorder->Pause();
}

/** \fn TVRec::RecorderPaused(void)
 *  \brief This is a callback, called by the "recorder" instance when
 *         it has actually paused.
 *  \sa PauseRecorder(void)
 */
void TVRec::RecorderPaused(void)
{
    if (pauseNotify)
        WakeEventLoop();
}

/**
 *  \brief Toggles whether the current channel should be on our favorites list.
 */
void TVRec::ToggleChannelFavorite(QString changroupname)
{
    QMutexLocker lock(&stateChangeLock);

    if (!channel)
        return;

    // Get current channel id...
    uint    sourceid = channel->GetCurrentSourceID();
    QString channum  = channel->GetCurrentName();
    uint chanid = ChannelUtil::GetChanID(sourceid, channum);

    if (!chanid)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + QString(
                "Channel: \'%1\' was not found in the database.\n"
                "\t\t\tMost likely, your DefaultTVChannel setting is wrong.\n"
                "\t\t\tCould not toggle favorite.").arg(channum));
        return;
    }

    int  changrpid;
    bool result;

    changrpid = ChannelGroup::GetChannelGroupId(changroupname);

    if (changrpid <1)
    {
          VERBOSE(VB_RECORD, LOC + QString("ToggleChannelFavorite: Invalid "
                   "channel group name %1, ").arg(changroupname));
    }
    else
    {
        result = ChannelGroup::ToggleChannel(chanid, changrpid, true);

        if (!result)
           VERBOSE(VB_RECORD, LOC + "Unable to toggle channel favorite.");
        else
           VERBOSE(VB_RECORD, LOC + QString("Toggled channel favorite."
                   "channum %1, chan group %2").arg(channum).arg(changroupname));
    }
}

/** \fn TVRec::ChangePictureAttribute(PictureAdjustType,PictureAttribute,bool)
 *  \brief Returns current value [0,100] if it succeeds, -1 otherwise.
 *
 *  Note: In practice this only works with frame grabbing recorders.
 */
int TVRec::GetPictureAttribute(PictureAttribute attr)
{
    QMutexLocker lock(&stateChangeLock);
    if (!channel)
        return -1;

    int ret = channel->GetPictureAttribute(attr);

    return (ret < 0) ? -1 : ret / 655;
}

/** \fn TVRec::ChangePictureAttribute(PictureAdjustType,PictureAttribute,bool)
 *  \brief Changes brightness/contrast/colour/hue of a recording.
 *
 *  Note: In practice this only works with frame grabbing recorders.
 *
 *  \return current value [0,100] if it succeeds, -1 otherwise.
 */
int TVRec::ChangePictureAttribute(PictureAdjustType type,
                                  PictureAttribute  attr,
                                  bool              direction)
{
    QMutexLocker lock(&stateChangeLock);
    if (!channel)
        return -1;

    int ret = channel->ChangePictureAttribute(type, attr, direction);

    return (ret < 0) ? -1 : ret / 655;
}

/** \fn TVRec::GetFreeInputs(const vector<uint>&) const
 *  \brief Returns the recorder's available inputs.
 *
 *   This filters out the connected inputs that belong to an input
 *   group which is busy. Recorders in the excluded cardids will
 *   not be considered busy for the sake of determining free inputs.
 *
 */
vector<InputInfo> TVRec::GetFreeInputs(
    const vector<uint> &excluded_cardids) const
{
    vector<InputInfo> list;
    if (channel)
        list = channel->GetFreeInputs(excluded_cardids);
    return list;
}

/** \fn TVRec::GetInput(void) const
 *  \brief Returns current input.
 */
QString TVRec::GetInput(void) const
{
    if (channel)
        return channel->GetCurrentInput();
    return QString::null;
}

/** \fn TVRec::SetInput(QString, uint)
 *  \brief Changes to the specified input.
 *
 *   You must call PauseRecorder(void) before calling this.
 *
 *  \param input Input to switch to, or "SwitchToNextInput".
 *  \return input we have switched to
 */
QString TVRec::SetInput(QString input, uint requestType)
{
    QMutexLocker lock(&stateChangeLock);
    QString origIn = input;
    VERBOSE(VB_RECORD, LOC + "SetInput("<<input<<") -- begin");

    if (!channel)
    {
        VERBOSE(VB_RECORD, LOC + "SetInput() -- end  no channel class");
        return QString::null;
    }

    input = (input == "SwitchToNextInput") ? channel->GetNextInput() : input;

    if (input == channel->GetCurrentInput())
    {
        VERBOSE(VB_RECORD, LOC + "SetInput("<<origIn<<":"<<input
                <<") -- end  nothing to do");
        return input;
    }

    QString name = channel->GetNextInputStartChan();

    // Detect tuning request type if needed
    if (requestType & kFlagDetect)
    {
        WaitForEventThreadSleep();
        requestType = lastTuningRequest.flags & (kFlagRec | kFlagNoRec);
    }

    // Clear the RingBuffer reset flag, in case we wait for a reset below
    ClearFlags(kFlagRingBufferReady);

    // Actually add the tuning request to the queue, and
    // then wait for it to start tuning
    tuningRequests.enqueue(TuningRequest(requestType, name, input));
    WaitForEventThreadSleep();

    // If we are using a recorder, wait for a RingBuffer reset
    if (requestType & kFlagRec)
    {
        while (!HasFlags(kFlagRingBufferReady))
            WaitForEventThreadSleep();
    }
    VERBOSE(VB_RECORD, LOC + "SetInput("<<origIn<<":"<<input<<") -- end");

    return GetInput();
}

/** \fn TVRec::SetChannel(QString,uint)
 *  \brief Changes to a named channel on the current tuner.
 *
 *   You must call PauseRecorder() before calling this.
 *
 *  \param name channum of channel to change to
 *  \param requestType tells us what kind of request to actually send to
 *                     the tuning thread, kFlagDetect is usually sufficient
 */
void TVRec::SetChannel(QString name, uint requestType)
{
    QMutexLocker lock(&stateChangeLock);
    VERBOSE(VB_CHANNEL, LOC + QString("SetChannel(%1) -- begin").arg(name));

    // Detect tuning request type if needed
    if (requestType & kFlagDetect)
    {
        WaitForEventThreadSleep();
        requestType = lastTuningRequest.flags & (kFlagRec | kFlagNoRec);
    }

    // Clear the RingBuffer reset flag, in case we wait for a reset below
    ClearFlags(kFlagRingBufferReady);

    // Actually add the tuning request to the queue, and
    // then wait for it to start tuning
    tuningRequests.enqueue(TuningRequest(requestType, name));
    WaitForEventThreadSleep();

    // If we are using a recorder, wait for a RingBuffer reset
    if (requestType & kFlagRec)
    {
        while (!HasFlags(kFlagRingBufferReady))
            WaitForEventThreadSleep();
    }
    VERBOSE(VB_CHANNEL, LOC + QString("SetChannel(%1) -- end").arg(name));
}

void TVRec::GetNextProgram(BrowseDirection direction,
                           QString &title,       QString &subtitle,
                           QString &desc,        QString &category,
                           QString &starttime,   QString &endtime,
                           QString &callsign,    QString &iconpath,
                           QString &channum,     uint    &sourceChanid,
                           QString &seriesid,    QString &programid)
{
    QString compare     = "<=";
    QString sortorder   = "desc";
    uint    chanid      = 0;

    if (sourceChanid)
    {
        chanid = sourceChanid;

        if (BROWSE_UP == direction)
            chanid = channel->GetNextChannel(chanid, CHANNEL_DIRECTION_UP);
        else if (BROWSE_DOWN == direction)
            chanid = channel->GetNextChannel(chanid, CHANNEL_DIRECTION_DOWN);
        else if (BROWSE_FAVORITE == direction)
            chanid = channel->GetNextChannel(
                chanid, CHANNEL_DIRECTION_FAVORITE);

        else if (BROWSE_LEFT == direction)
        {
            compare = "<";
        }
        else if (BROWSE_RIGHT == direction)
        {
            compare = ">";
            sortorder = "asc";
        }
    }

    if (!chanid)
    {
        if (BROWSE_SAME == direction)
            chanid = channel->GetNextChannel(channum, CHANNEL_DIRECTION_SAME);
        else if (BROWSE_UP == direction)
            chanid = channel->GetNextChannel(channum, CHANNEL_DIRECTION_UP);
        else if (BROWSE_DOWN == direction)
            chanid = channel->GetNextChannel(channum, CHANNEL_DIRECTION_DOWN);
        else if (BROWSE_FAVORITE == direction)
            chanid = channel->GetNextChannel(channum,
                                             CHANNEL_DIRECTION_FAVORITE);
        else if (BROWSE_LEFT == direction)
        {
            chanid = channel->GetNextChannel(channum, CHANNEL_DIRECTION_SAME);
            compare = "<";
        }
        else if (BROWSE_RIGHT == direction)
        {
                chanid = channel->GetNextChannel(channum,
                                                 CHANNEL_DIRECTION_SAME);
            compare = ">";
            sortorder = "asc";
        }
    }

    QString querystr = QString(
        "SELECT title,     subtitle, description, category, "
        "       starttime, endtime,  callsign,    icon,     "
        "       channum,   seriesid, programid "
        "FROM program, channel "
        "WHERE program.chanid = channel.chanid AND "
        "      channel.chanid = :CHANID        AND "
        "      starttime %1 :STARTTIME "
        "ORDER BY starttime %2 "
        "LIMIT 1").arg(compare).arg(sortorder);

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(querystr);
    query.bindValue(":CHANID",    chanid);
    query.bindValue(":STARTTIME", starttime);

    // Clear everything now in case either query fails.
    title     = subtitle  = desc      = category  = "";
    starttime = endtime   = callsign  = iconpath  = "";
    channum   = seriesid  = programid = "";
    sourceChanid = 0;

    // Try to get the program info
    if (!query.exec() && !query.isActive())
    {
        MythDB::DBError("GetNextProgram -- get program info", query);
    }
    else if (query.next())
    {
        title     = query.value(0).toString();
        subtitle  = query.value(1).toString();
        desc      = query.value(2).toString();
        category  = query.value(3).toString();
        starttime = query.value(4).toString();
        endtime   = query.value(5).toString();
        callsign  = query.value(6).toString();
        iconpath  = query.value(7).toString();
        channum   = query.value(8).toString();
        seriesid  = query.value(9).toString();
        programid = query.value(10).toString();
        sourceChanid = chanid;
        return;
    }

    // Couldn't get program info, so get the channel info instead
    query.prepare(
        "SELECT channum, callsign, icon "
        "FROM channel "
        "WHERE chanid = :CHANID");
    query.bindValue(":CHANID", chanid);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("GetNextProgram -- get channel info", query);
    }
    else if (query.next())
    {
        sourceChanid = chanid;
        channum  = query.value(0).toString();
        callsign = query.value(1).toString();
        iconpath = query.value(2).toString();
    }
}

bool TVRec::GetChannelInfo(uint &chanid, uint &sourceid,
                           QString &callsign, QString &channum,
                           QString &channame, QString &xmltvid) const
{
    callsign = "";
    channum  = "";
    channame = "";
    xmltvid  = "";

    if ((!chanid || !sourceid) && !channel)
        return false;

    if (!chanid)
        chanid = (uint) max(channel->GetChanID(), 0);

    if (!sourceid)
        sourceid = channel->GetCurrentSourceID();

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT callsign, channum, name, xmltvid "
        "FROM channel "
        "WHERE chanid = :CHANID");
    query.bindValue(":CHANID", chanid);
    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("GetChannelInfo", query);
        return false;
    }

    if (!query.next())
        return false;

    callsign = query.value(0).toString();
    channum  = query.value(1).toString();
    channame = query.value(2).toString();
    xmltvid  = query.value(3).toString();

    return true;
}

bool TVRec::SetChannelInfo(uint chanid, uint sourceid,
                           QString oldchannum,
                           QString callsign, QString channum,
                           QString channame, QString xmltvid)
{
    if (!chanid || !sourceid || channum.isEmpty())
        return false;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "UPDATE channel "
        "SET callsign = :CALLSIGN, "
        "    channum  = :CHANNUM,  "
        "    name     = :CHANNAME, "
        "    xmltvid  = :XMLTVID   "
        "WHERE chanid   = :CHANID AND "
        "      sourceid = :SOURCEID");
    query.bindValue(":CALLSIGN", callsign);
    query.bindValue(":CHANNUM",  channum);
    query.bindValue(":CHANNAME", channame);
    query.bindValue(":XMLTVID",  xmltvid);
    query.bindValue(":CHANID",   chanid);
    query.bindValue(":SOURCEID", sourceid);

    if (!query.exec())
    {
        MythDB::DBError("SetChannelInfo", query);
        return false;
    }

    if (channel)
        channel->Renumber(sourceid, oldchannum, channum);

    return true;
}

/** \fn TVRec::SetRingBuffer(RingBuffer*)
 *  \brief Sets "ringBuffer", deleting any existing RingBuffer.
 */
void TVRec::SetRingBuffer(RingBuffer *rb)
{
    QMutexLocker lock(&stateChangeLock);

    RingBuffer *rb_old = ringBuffer;
    ringBuffer = rb;

    if (rb_old && (rb_old != rb))
    {
        if (HasFlags(kFlagDummyRecorderRunning))
            ClearFlags(kFlagDummyRecorderRunning);
        delete rb_old;
    }

    m_switchingBuffer = false;
}

void TVRec::RingBufferChanged(RingBuffer *rb, ProgramInfo *pginfo)
{
    VERBOSE(VB_IMPORTANT, LOC + "RingBufferChanged()");

    if (pginfo)
    {
        if (curRecording)
        {
            FinishedRecording(curRecording);
            curRecording->MarkAsInUse(false, kRecorderInUseID);
            delete curRecording;
        }
        curRecording = new RecordingInfo(*pginfo);
        curRecording->MarkAsInUse(true, kRecorderInUseID);
    }

    SetRingBuffer(rb);
}

QString TVRec::TuningGetChanNum(const TuningRequest &request,
                                QString &input) const
{
    QString channum = QString::null;

    if (request.program)
    {
        request.program->QueryTuningInfo(channum, input);
        return channum;
    }

    channum = request.channel;
    input   = request.input;

    // If this is Live TV startup, we need a channel...
    if (channum.isEmpty() && (request.flags & kFlagLiveTV))
    {
        if (!LiveTVStartChannel.isEmpty())
            channum = LiveTVStartChannel;
        else
        {
            input   = genOpt.defaultinput;
            channum = GetStartChannel(cardid, input);
        }
    }
    if (request.flags & kFlagLiveTV)
        channel->Init(input, channum, false);

    if (channel && !channum.isEmpty() && (channum.indexOf("NextChannel") >= 0))
    {
        int dir     = channum.right(channum.length() - 12).toInt();
        uint chanid = channel->GetNextChannel(0, dir);
        channum     = ChannelUtil::GetChanNum(chanid);
    }

    return channum;
}

bool TVRec::TuningOnSameMultiplex(TuningRequest &request)
{
    if ((request.flags & kFlagAntennaAdjust) || request.input.isEmpty() ||
        !GetDTVRecorder() || signalMonitor || !channel || !channel->IsOpen())
    {
        return false;
    }

    uint    sourceid   = channel->GetCurrentSourceID();
    QString oldchannum = channel->GetCurrentName();
    QString newchannum = request.channel;

    if (ChannelUtil::IsOnSameMultiplex(sourceid, newchannum, oldchannum))
    {
        MPEGStreamData *mpeg = GetDTVRecorder()->GetStreamData();
        ATSCStreamData *atsc = dynamic_cast<ATSCStreamData*>(mpeg);

        if (atsc)
        {
            uint major, minor = 0;
            ChannelUtil::GetATSCChannel(sourceid, newchannum, major, minor);

            if (minor && atsc->HasChannel(major, minor))
            {
                request.majorChan = major;
                request.minorChan = minor;
                return true;
            }
        }

        if (mpeg)
        {
            uint progNum = ChannelUtil::GetProgramNumber(sourceid, newchannum);
            if (mpeg->HasProgram(progNum))
            {
                request.progNum = progNum;
                return true;
            }
        }
    }

    return false;
}

/** \fn TVRec::HandleTuning(void)
 *  \brief Handles all tuning events.
 *
 *   This method pops tuning events off the tuningState queue
 *   and does what needs to be done, mostly by calling one of
 *   the Tuning... methods.
 */
void TVRec::HandleTuning(void)
{
    if (tuningRequests.size())
    {
        TuningRequest request = tuningRequests.front();
        VERBOSE(VB_RECORD, LOC + "HandleTuning Request: "<<request.toString());

        QString input;
        request.channel = TuningGetChanNum(request, input);
        request.input   = input;

        if (TuningOnSameMultiplex(request))
            VERBOSE(VB_PLAYBACK, LOC + "On same multiplex");

        TuningShutdowns(request);

        // The dequeue isn't safe to do until now because we
        // release the stateChangeLock to teardown a recorder
        tuningRequests.dequeue();

        // Now we start new stuff
        if (request.flags & (kFlagRecording|kFlagLiveTV|
                             kFlagEITScan|kFlagAntennaAdjust))
        {
            if (!recorder)
            {
                VERBOSE(VB_RECORD, LOC +
                        "No recorder yet, calling TuningFrequency");
                TuningFrequency(request);
            }
            else
            {
                VERBOSE(VB_RECORD, LOC + "Waiting for recorder pause..");
                SetFlags(kFlagWaitingForRecPause);
            }
        }
        lastTuningRequest = request;
    }

    if (HasFlags(kFlagWaitingForRecPause))
    {
        if (!recorder->IsPaused())
            return;

        ClearFlags(kFlagWaitingForRecPause);
        VERBOSE(VB_RECORD, LOC + "Recorder paused, calling TuningFrequency");
        TuningFrequency(lastTuningRequest);
    }

    MPEGStreamData *streamData = NULL;
    if (HasFlags(kFlagWaitingForSignal) && !(streamData = TuningSignalCheck()))
        return;

    if (HasFlags(kFlagNeedToStartRecorder))
    {
        if (recorder)
            TuningRestartRecorder();
        else
            TuningNewRecorder(streamData);

        // If we got this far it is safe to set a new starting channel...
        if (channel)
            channel->StoreInputChannels();
    }
}

/** \fn TVRec::TuningCheckForHWChange(const TuningRequest&,QString&,QString&)
 *  \brief Returns cardid for device info row in capturecard if it changes.
 */
uint TVRec::TuningCheckForHWChange(const TuningRequest &request,
                                   QString &channum,
                                   QString &inputname)
{
    if (!channel)
        return 0;

    uint curCardID = 0, newCardID = 0;
    channum   = request.channel;
    inputname = request.input;

    if (request.program)
        request.program->QueryTuningInfo(channum, inputname);

    if (!channum.isEmpty() && inputname.isEmpty())
        channel->CheckChannel(channum, inputname);

    if (!inputname.isEmpty())
    {
        int current_input = channel->GetCurrentInputNum();
        int new_input     = channel->GetInputByName(inputname);
        curCardID = channel->GetInputCardID(current_input);
        newCardID = channel->GetInputCardID(new_input);
        VERBOSE(VB_IMPORTANT, LOC<<"HW Tuner: "<<curCardID<<"->"<<newCardID);
    }

    if (curCardID != newCardID)
    {
        if (channum.isEmpty())
            channum = GetStartChannel(newCardID, inputname);
        return newCardID;
    }

    return 0;
}

/** \fn TVRec::TuningShutdowns(const TuningRequest&)
 *  \brief This shuts down anything that needs to be shut down
 *         before handling the passed in tuning request.
 */
void TVRec::TuningShutdowns(const TuningRequest &request)
{
    QString channum, inputname;
    uint newCardID = TuningCheckForHWChange(request, channum, inputname);

    if (!(request.flags & kFlagEITScan) && HasFlags(kFlagEITScannerRunning))
    {
        scanner->StopActiveScan();
        ClearFlags(kFlagEITScannerRunning);
    }

    if (scanner && !request.IsOnSameMultiplex())
        scanner->StopPassiveScan();

    if (HasFlags(kFlagSignalMonitorRunning))
    {
        MPEGStreamData *sd = NULL;
        if (GetDTVSignalMonitor())
            sd = GetDTVSignalMonitor()->GetStreamData();
        TeardownSignalMonitor();
        ClearFlags(kFlagSignalMonitorRunning);

        // Delete StreamData if it is not in use by the recorder.
        MPEGStreamData *rec_sd = NULL;
        if (GetDTVRecorder())
            rec_sd = GetDTVRecorder()->GetStreamData();
        if (sd && (sd != rec_sd))
            delete sd;
    }
    if (HasFlags(kFlagWaitingForSignal))
        ClearFlags(kFlagWaitingForSignal);

    // At this point any waits are canceled.

    if (newCardID || (request.flags & kFlagNoRec))
    {
        if (HasFlags(kFlagDummyRecorderRunning))
        {
            ClearFlags(kFlagDummyRecorderRunning);
            FinishedRecording(curRecording);
            curRecording->MarkAsInUse(false, kRecorderInUseID);
        }

        if (request.flags & kFlagCloseRec)
            FinishedRecording(lastTuningRequest.program);

        if (HasFlags(kFlagRecorderRunning) ||
            (curRecording && curRecording->GetRecordingStatus() == rsFailed))
        {
            stateChangeLock.unlock();
            TeardownRecorder(request.flags & kFlagKillRec);
            stateChangeLock.lock();
            ClearFlags(kFlagRecorderRunning);
        }
        // At this point the recorders are shut down

        CloseChannel();
        // At this point the channel is shut down
    }

    // handle HW change for digital/analog cards
    if (newCardID)
    {
        VERBOSE(VB_IMPORTANT, "Recreating channel...");
        channel->Close();
        delete channel;
        channel = NULL;

        GetDevices(newCardID, genOpt, dvbOpt, fwOpt);
        genOpt.defaultinput = inputname;
        CreateChannel(channum);
        if (!(request.flags & kFlagNoRec))
            channel->Open();
    }

    if (ringBuffer && (request.flags & kFlagKillRingBuffer))
    {
        VERBOSE(VB_RECORD, LOC + "Tearing down RingBuffer");
        SetRingBuffer(NULL);
        // At this point the ringbuffer is shut down
    }

    // Clear pending actions from last request
    ClearFlags(kFlagPendingActions);
}

/** \fn TVRec::TuningFrequency(const TuningRequest&)
 *  \brief Performs initial tuning required for any tuning event.
 *
 *   This figures out the channel name, and possibly the
 *   input name we need to pass to "channel" and then calls
 *   channel appropriately.
 *
 *   Then it adds any filters and sets any video capture attributes
 *   that need to be set.
 *
 *   The signal monitoring is started if possible. If it is started
 *   the kFlagWaitForSignal flag is set.
 *
 *   The kFlagNeedToStartRecorder flag is ald set if this isn't
 *   an EIT scan so that the recorder is started or restarted a
 *   appropriate.
 */
void TVRec::TuningFrequency(const TuningRequest &request)
{
    if (!channel)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "TuningFrequency called without a valid channel object");
        return;
    }

    DTVChannel *dtvchan = GetDTVChannel();
    bool livetv = request.flags & kFlagLiveTV;
    bool antadj = request.flags & kFlagAntennaAdjust;
    bool has_dummy = false;
    bool ok = true;

    if (dtvchan)
    {
        MPEGStreamData *mpeg = NULL;

        if (GetDTVRecorder())
            mpeg = GetDTVRecorder()->GetStreamData();

        const QString tuningmode = (HasFlags(kFlagEITScannerRunning)) ?
                                   dtvchan->GetSIStandard() :
                                   dtvchan->GetSuggestedTuningMode
                                   (kState_WatchingLiveTV == internalState);

        dtvchan->SetTuningMode(tuningmode);

        if (request.minorChan && (tuningmode == "atsc"))
        {
            channel->SelectChannel(request.channel, false);
            ATSCStreamData *atsc = dynamic_cast<ATSCStreamData*>(mpeg);
            if (atsc)
                atsc->SetDesiredChannel(request.majorChan, request.minorChan);
        }
        else if (request.progNum >= 0)
        {
            channel->SelectChannel(request.channel, false);
            if (mpeg)
                mpeg->SetDesiredProgram(request.progNum);
        }
    }

    if (request.IsOnSameMultiplex())
    {
        QStringList slist;
        slist<<"message"<<QObject::tr("On known multiplex...");
        MythEvent me(QString("SIGNAL %1").arg(cardid), slist);
        gCoreContext->dispatch(me);

        SetFlags(kFlagNeedToStartRecorder);
        return;
    }

    QString input   = request.input;
    QString channum = request.channel;

    channel->Open();

    if (livetv || antadj)
    {
        // We need there to be a ringbuffer for these modes
        ProgramInfo *tmp = pseudoLiveTVRecording;
        pseudoLiveTVRecording = NULL;

        tvchain->SetCardType("DUMMY");

        if (!ringBuffer)
            ok = CreateLiveTVRingBuffer(channum);
        else
            ok = SwitchLiveTVRingBuffer(channum, true, false);
        pseudoLiveTVRecording = tmp;

        tvchain->SetCardType(genOpt.cardtype);

        if (!ok)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to create RingBuffer 1");
            return;
        }

        has_dummy = true;
    }

    if (!channum.isEmpty())
    {
        if (!input.isEmpty())
            channel->SelectInput(input, channum, true);
        else
            channel->SelectChannel(channum, true);
    }

    // Start signal (or channel change) monitoring
    VERBOSE(VB_RECORD, LOC + "Starting Signal Monitor");
    bool error = false;
    if (!SetupSignalMonitor(!antadj, request.flags & kFlagEITScan,
                            livetv | antadj))
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to setup signal monitor");
        if (signalMonitor)
        {
            delete signalMonitor;
            signalMonitor = NULL;
        }

        // pretend the signal monitor is running to prevent segfault
        SetFlags(kFlagSignalMonitorRunning);
        ClearFlags(kFlagWaitingForSignal);
        error = true;
    }
    else if (signalMonitor)
    {
        SetFlags(kFlagSignalMonitorRunning);
        ClearFlags(kFlagWaitingForSignal);
        if (!antadj)
            SetFlags(kFlagWaitingForSignal);
    }

    if (has_dummy && ringBuffer)
    {
        // Make sure recorder doesn't point to bogus ringbuffer before
        // it is potentially restarted without a new ringbuffer, if
        // the next channel won't tune and the user exits LiveTV.
        if (recorder)
            recorder->SetRingBuffer(NULL);

        SetFlags(kFlagDummyRecorderRunning);
        VERBOSE(VB_RECORD, "DummyDTVRecorder -- started");
        SetFlags(kFlagRingBufferReady);
    }

    // if we had problems starting the signal monitor,
    // we don't want to start the recorder...
    if (error)
        return;

    // Request a recorder, if the command is a recording command
    ClearFlags(kFlagNeedToStartRecorder);
    if (request.flags & kFlagRec && !antadj)
        SetFlags(kFlagNeedToStartRecorder);
}

/** \fn TVRec::TuningSignalCheck(void)
 *  \brief This checks if we have a channel lock.
 *
 *   If we have a channel lock this shuts down the signal monitoring.
 *
 *  \return MPEGStreamData pointer if we have a complete lock, NULL otherwise
 */
MPEGStreamData *TVRec::TuningSignalCheck(void)
{
    if (signalMonitor->IsAllGood())
    {
        VERBOSE(VB_RECORD, LOC + "Got good signal");

        pendingRecLock.lock();
        m_recStatus = rsRecording;
        pendingRecLock.unlock();
        if (curRecording)
            curRecording->SetRecordingStatus(rsRecording);
    }
    else if (signalMonitor->IsErrored())
    {
        VERBOSE(VB_RECORD, LOC_ERR + "SignalMonitor failed");
        ClearFlags(kFlagNeedToStartRecorder);

        pendingRecLock.lock();
        m_recStatus = rsFailed;
        pendingRecLock.unlock();
        if (curRecording)
            curRecording->SetRecordingStatus(rsFailed);

        if (HasFlags(kFlagEITScannerRunning))
        {
            scanner->StopActiveScan();
            ClearFlags(kFlagEITScannerRunning);
        }
    }
    else
        return NULL;

    if (curRecording)
    {
        MythEvent me(QString("UPDATE_RECORDING_STATUS %1 %2 %3 %4 %5")
                     .arg(curRecording->GetCardID())
                     .arg(curRecording->GetChanID())
                     .arg(curRecording->GetScheduledStartTime(ISODate))
                     .arg(curRecording->GetRecordingStatus())
                     .arg(curRecording->GetRecordingEndTime(ISODate)));
        gCoreContext->dispatch(me);
    }

    // grab useful data from DTV signal monitor before we kill it...
    MPEGStreamData *streamData = NULL;
    if (GetDTVSignalMonitor())
        streamData = GetDTVSignalMonitor()->GetStreamData();

    if (!HasFlags(kFlagEITScannerRunning))
    {
        // shut down signal monitoring
        TeardownSignalMonitor();
        ClearFlags(kFlagSignalMonitorRunning);
    }
    ClearFlags(kFlagWaitingForSignal);

    if (streamData)
    {
        DVBStreamData *dsd = dynamic_cast<DVBStreamData*>(streamData);
        if (dsd)
            dsd->SetDishNetEIT(is_dishnet_eit(cardid));
        if (!get_use_eit(GetCaptureCardNum()))
        {
            VERBOSE(VB_EIT, LOC + "EIT scanning disabled "
                    "for all sources on this card.");
        }
        else if (scanner)
            scanner->StartPassiveScan(channel, streamData);
    }

    return streamData;
}

static int init_jobs(const RecordingInfo *rec, RecordingProfile &profile,
                      bool on_host, bool transcode_bfr_comm, bool on_line_comm)
{
    if (!rec)
        return 0; // no jobs for Live TV recordings..

    int jobs = 0; // start with no jobs

    // grab standard jobs flags from program info
    JobQueue::AddJobsToMask(rec->GetAutoRunJobs(), jobs);

    // disable commercial flagging on PBS, BBC, etc.
    if (rec->IsCommercialFree())
        JobQueue::RemoveJobsFromMask(JOB_COMMFLAG, jobs);

    // disable transcoding if the profile does not allow auto transcoding
    const Setting *autoTrans = profile.byName("autotranscode");
    if ((!autoTrans) || (autoTrans->getValue().toInt() == 0))
        JobQueue::RemoveJobsFromMask(JOB_TRANSCODE, jobs);

    // is commercial flagging enabled, and is on-line comm flagging enabled?
    bool rt = JobQueue::JobIsInMask(JOB_COMMFLAG, jobs) && on_line_comm;
    // also, we either need transcoding to be disabled or
    // we need to be allowed to commercial flag before transcoding?
    rt &= JobQueue::JobIsNotInMask(JOB_TRANSCODE, jobs) ||
        !transcode_bfr_comm;
    if (rt)
    {
        // queue up real-time (i.e. on-line) commercial flagging.
        QString host = (on_host) ? gCoreContext->GetHostName() : "";
        JobQueue::QueueJob(JOB_COMMFLAG,
                           rec->GetChanID(),
                           rec->GetRecordingStartTime(), "", "",
                           host, JOB_LIVE_REC);

        // don't do regular comm flagging, we won't need it.
        JobQueue::RemoveJobsFromMask(JOB_COMMFLAG, jobs);
    }

    return jobs;
}

static QString load_profile(QString cardtype, void *tvchain,
                            RecordingInfo *rec, RecordingProfile &profile)
{
    // Determine the correct recording profile.
    // In LiveTV mode use "Live TV" profile, otherwise use the
    // recording's specified profile. If the desired profile can't
    // be found, fall back to the "Default" profile for card type.
    QString profileName = "Live TV";
    if (!tvchain && rec)
        profileName = rec->GetRecordingRule()->m_recProfile;

    if (!profile.loadByType(profileName, cardtype))
    {
        profileName = "Default";
        profile.loadByType(profileName, cardtype);
    }

    VERBOSE(VB_RECORD, QString("Using profile '%1' to record")
            .arg(profileName));

    return profileName;
}

/** \fn TVRec::TuningNewRecorder(MPEGStreamData*)
 *  \brief Creates a recorder instance.
 */
void TVRec::TuningNewRecorder(MPEGStreamData *streamData)
{
    VERBOSE(VB_RECORD, LOC + "Starting Recorder");

    bool had_dummyrec = false;
    if (HasFlags(kFlagDummyRecorderRunning))
    {
        ClearFlags(kFlagDummyRecorderRunning);
        FinishedRecording(curRecording);
        curRecording->MarkAsInUse(false, kRecorderInUseID);
        had_dummyrec = true;
    }

    RecordingInfo *rec = lastTuningRequest.program;

    RecordingProfile profile;
    QString profileName = load_profile(genOpt.cardtype, tvchain, rec, profile);

    if (tvchain)
    {
        bool ok;
        if (!ringBuffer)
        {
            ok = CreateLiveTVRingBuffer(channel->GetCurrentName());
            SetFlags(kFlagRingBufferReady);
        }
        else
            ok = SwitchLiveTVRingBuffer(channel->GetCurrentName(),
                                        true, !had_dummyrec && recorder);
        if (!ok)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to create RingBuffer 2");
            goto err_ret;
        }
        rec = curRecording;  // new'd in Create/SwitchLiveTVRingBuffer()
    }

    if (lastTuningRequest.flags & kFlagRecording)
    {
        bool write = genOpt.cardtype != "IMPORT";
        VERBOSE(VB_IMPORTANT, LOC + QString("rec->GetPathname(): '%1'")
                .arg(rec->GetPathname()));
        SetRingBuffer(new RingBuffer(rec->GetPathname(), write));
        if (!ringBuffer->IsOpen() && write)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    QString("RingBuffer '%1' not open...")
                    .arg(rec->GetPathname()));
            SetRingBuffer(NULL);
            ClearFlags(kFlagPendingActions);
            goto err_ret;
        }
    }

    if (!ringBuffer)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + QString(
                    "Failed to start recorder!  ringBuffer is NULL\n"
                    "\t\t\t\t  Tuning request was %1\n")
                .arg(lastTuningRequest.toString()));

        if (HasFlags(kFlagLiveTV))
        {
            QString message = QString("QUIT_LIVETV %1").arg(cardid);
            MythEvent me(message);
            gCoreContext->dispatch(me);
        }
        goto err_ret;
    }

    if (channel && genOpt.cardtype == "MJPEG")
        channel->Close(); // Needed because of NVR::MJPEGInit()

    if (!SetupRecorder(profile))
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + QString(
                    "Failed to start recorder!\n"
                    "\t\t\t\t  Tuning request was %1\n")
                .arg(lastTuningRequest.toString()));

        if (HasFlags(kFlagLiveTV))
        {
            QString message = QString("QUIT_LIVETV %1").arg(cardid);
            MythEvent me(message);
            gCoreContext->dispatch(me);
        }
        TeardownRecorder(true);
        goto err_ret;
    }

    if (GetDTVRecorder() && streamData)
    {
        const Setting *setting = profile.byName("recordingtype");
        if (setting)
            streamData->SetRecordingType(setting->getValue());
        GetDTVRecorder()->SetStreamData(streamData);
    }

    if (channel && genOpt.cardtype == "MJPEG")
        channel->Open(); // Needed because of NVR::MJPEGInit()

    if (rec)
        recorder->SetRecording(rec);

    // Setup for framebuffer capture devices..
    if (channel)
    {
        SetVideoFiltersForChannel(channel->GetCurrentSourceID(),
                                  channel->GetCurrentName());
    }

#ifdef USING_V4L
    if (GetV4LChannel())
    {
        channel->InitPictureAttributes();
        CloseChannel();
    }
#endif

    pthread_create(&recorder_thread, NULL, TVRec::RecorderThread, recorder);

    // Wait for recorder to start.
    stateChangeLock.unlock();
    while (!recorder->IsRecording() && !recorder->IsErrored())
        usleep(5 * 1000);
    stateChangeLock.lock();

    if (GetV4LChannel())
        channel->SetFd(recorder->GetVideoFd());

    SetFlags(kFlagRecorderRunning | kFlagRingBufferReady);

    if (!tvchain)
        autoRunJobs = init_jobs(rec, profile, runJobOnHostOnly,
                                transcodeFirst, earlyCommFlag);

    ClearFlags(kFlagNeedToStartRecorder);
    return;

  err_ret:
    ChangeState(kState_None);
    if (tvchain)
        delete rec;
}

/** \fn TVRec::TuningRestartRecorder(void)
 *  \brief Restarts a stopped recorder or unpauses a paused recorder.
 */
void TVRec::TuningRestartRecorder(void)
{
    VERBOSE(VB_RECORD, LOC + "Restarting Recorder");

    bool had_dummyrec = false;
    if (HasFlags(kFlagDummyRecorderRunning))
    {
        ClearFlags(kFlagDummyRecorderRunning);
        had_dummyrec = true;
    }

    if (curRecording)
    {
        FinishedRecording(curRecording);
        curRecording->MarkAsInUse(false, kRecorderInUseID);

        if (pseudoLiveTVRecording)
        {
            int secsSince = curRecording->GetRecordingStartTime()
                .secsTo(QDateTime::currentDateTime());
            if (secsSince < 120)
            {
                JobQueue::RemoveJobsFromMask(JOB_COMMFLAG, autoRunJobs);
                JobQueue::RemoveJobsFromMask(JOB_TRANSCODE, autoRunJobs);
            }

            if (autoRunJobs)
                JobQueue::QueueRecordingJobs(*curRecording, autoRunJobs);
        }
    }

    SwitchLiveTVRingBuffer(channel->GetCurrentName(), true, !had_dummyrec);

    if (had_dummyrec)
    {
        recorder->SetRingBuffer(ringBuffer);
        ProgramInfo *progInfo = tvchain->GetProgramAt(-1);
        recorder->SetRecording(progInfo);
        delete progInfo;
    }
    recorder->Reset();

    // Set file descriptor of channel from recorder for V4L
    channel->SetFd(recorder->GetVideoFd());

    // Some recorders unpause on Reset, others do not...
    recorder->Unpause();

    if (pseudoLiveTVRecording)
    {
        ProgramInfo *rcinfo1 = pseudoLiveTVRecording;
        QString msg1 = QString("Recording: %1 %2 %3 %4")
            .arg(rcinfo1->GetTitle()).arg(rcinfo1->GetChanID())
            .arg(rcinfo1->GetRecordingStartTime(ISODate))
            .arg(rcinfo1->GetRecordingEndTime(ISODate));
        ProgramInfo *rcinfo2 = tvchain->GetProgramAt(-1);
        QString msg2 = QString("Recording: %1 %2 %3 %4")
            .arg(rcinfo2->GetTitle()).arg(rcinfo2->GetChanID())
            .arg(rcinfo2->GetRecordingStartTime(ISODate))
            .arg(rcinfo2->GetRecordingEndTime(ISODate));
        delete rcinfo2;
        VERBOSE(VB_RECORD, LOC + "Pseudo LiveTV recording starting." +
                "\n\t\t\t" + msg1 + "\n\t\t\t" + msg2);

        curRecording->SaveAutoExpire(
            curRecording->GetRecordingRule()->GetAutoExpire());

        curRecording->ApplyRecordRecGroupChange(
            curRecording->GetRecordingRule()->m_recGroup);

        RecordingProfile profile;
        QString profileName = load_profile(genOpt.cardtype, NULL,
                                           curRecording, profile);
        autoRunJobs = init_jobs(curRecording, profile, runJobOnHostOnly,
                                transcodeFirst, earlyCommFlag);
    }

    ClearFlags(kFlagNeedToStartRecorder);
}

void TVRec::SetFlags(uint f)
{
    QMutexLocker lock(&stateChangeLock);
    stateFlags |= f;
    VERBOSE(VB_RECORD, LOC + QString("SetFlags(%1) -> %2")
            .arg(FlagToString(f)).arg(FlagToString(stateFlags)));
    WakeEventLoop();
}

void TVRec::ClearFlags(uint f)
{
    QMutexLocker lock(&stateChangeLock);
    stateFlags &= ~f;
    VERBOSE(VB_RECORD, LOC + QString("ClearFlags(%1) -> %2")
            .arg(FlagToString(f)).arg(FlagToString(stateFlags)));
    WakeEventLoop();
}

QString TVRec::FlagToString(uint f)
{
    QString msg("");

    // General flags
    if (kFlagFrontendReady & f)
        msg += "FrontendReady,";
    if (kFlagRunMainLoop & f)
        msg += "RunMainLoop,";
    if (kFlagExitPlayer & f)
        msg += "ExitPlayer,";
    if (kFlagFinishRecording & f)
        msg += "FinishRecording,";
    if (kFlagErrored & f)
        msg += "Errored,";
    if (kFlagCancelNextRecording & f)
        msg += "CancelNextRecording,";

    // Tuning flags
    if ((kFlagRec & f) == kFlagRec)
        msg += "REC,";
    else
    {
        if (kFlagLiveTV & f)
            msg += "LiveTV,";
        if (kFlagRecording & f)
            msg += "Recording,";
    }
    if ((kFlagNoRec & f) == kFlagNoRec)
        msg += "NOREC,";
    else
    {
        if (kFlagEITScan & f)
            msg += "EITScan,";
        if (kFlagCloseRec & f)
            msg += "CloseRec,";
        if (kFlagKillRec & f)
            msg += "KillRec,";
        if (kFlagAntennaAdjust & f)
            msg += "AntennaAdjust,";
    }
    if ((kFlagPendingActions & f) == kFlagPendingActions)
        msg += "PENDINGACTIONS,";
    else
    {
        if (kFlagWaitingForRecPause & f)
            msg += "WaitingForRecPause,";
        if (kFlagWaitingForSignal & f)
            msg += "WaitingForSignal,";
        if (kFlagNeedToStartRecorder & f)
            msg += "NeedToStartRecorder,";
        if (kFlagKillRingBuffer & f)
            msg += "KillRingBuffer,";
    }
    if ((kFlagAnyRunning & f) == kFlagAnyRunning)
        msg += "ANYRUNNING,";
    else
    {
        if (kFlagSignalMonitorRunning & f)
            msg += "SignalMonitorRunning,";
        if (kFlagEITScannerRunning & f)
            msg += "EITScannerRunning,";
        if ((kFlagAnyRecRunning & f) == kFlagAnyRecRunning)
            msg += "ANYRECRUNNING,";
        else
        {
            if (kFlagDummyRecorderRunning & f)
                msg += "DummyRecorderRunning,";
            if (kFlagRecorderRunning & f)
                msg += "RecorderRunning,";
        }
    }
    if (kFlagRingBufferReady & f)
        msg += "RingBufferReady,";

    if (msg.isEmpty())
        msg = QString("0x%1").arg(f,0,16);

    return msg;
}

bool TVRec::WaitForNextLiveTVDir(void)
{
    QMutexLocker lock(&nextLiveTVDirLock);

    bool found = !nextLiveTVDir.isEmpty();
    if (!found && triggerLiveTVDir.wait(&nextLiveTVDirLock, 500))
    {
        found = !nextLiveTVDir.isEmpty();
    }

    return found;
}

void TVRec::SetNextLiveTVDir(QString dir)
{
    QMutexLocker lock(&nextLiveTVDirLock);

    nextLiveTVDir = dir;
    triggerLiveTVDir.wakeAll();
}

bool TVRec::GetProgramRingBufferForLiveTV(RecordingInfo **pginfo,
                                          RingBuffer **rb,
                                          const QString & channum,
                                          int inputID)
{
    VERBOSE(VB_RECORD, LOC + "GetProgramRingBufferForLiveTV()");
    if (!channel || !tvchain || !pginfo || !rb)
        return false;

    nextLiveTVDirLock.lock();
    nextLiveTVDir.clear();
    nextLiveTVDirLock.unlock();

    // Dispatch this early, the response can take a while.
    MythEvent me(QString("QUERY_NEXT_LIVETV_DIR %1").arg(cardid));
    gCoreContext->dispatch(me);

    uint    sourceid = channel->GetSourceID(inputID);
    int     chanid   = ChannelUtil::GetChanID(sourceid, channum);

    if (chanid < 0)
    {
        // Test setups might have zero channels
        if (genOpt.cardtype == "IMPORT" || genOpt.cardtype == "DEMO")
            chanid = 9999;
        else
        {
            VERBOSE(VB_IMPORTANT, (LOC_ERR +
                "Channel: \'%1\' was not found in the database.\n"
                "\t\t\tMost likely, your DefaultTVChannel setting is wrong.\n"
                "\t\t\tCould not start livetv.").arg(channum));
            return false;
        }
    }

    int hoursMax = gCoreContext->GetNumSetting("MaxHoursPerLiveTVRecording", 8);
    if (hoursMax <= 0)
        hoursMax = 8;

    RecordingInfo *prog = NULL;
    if (pseudoLiveTVRecording)
        prog = new RecordingInfo(*pseudoLiveTVRecording);
    else
    {
        prog = new RecordingInfo(
            chanid, mythCurrentDateTime(), true, hoursMax);
    }

    prog->SetCardID(cardid);

    if (prog->GetRecordingStartTime() == prog->GetRecordingEndTime())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "GetProgramRingBufferForLiveTV()"
                "\n\t\t\tProgramInfo is invalid."
                "\n" + prog->toString());
        prog->SetScheduledEndTime(prog->GetRecordingStartTime().addSecs(3600));
        prog->SetRecordingEndTime(prog->GetScheduledEndTime());

        prog->SetChanID(chanid);
    }

    if (!pseudoLiveTVRecording)
        prog->SetRecordingStartTime(mythCurrentDateTime());

    prog->SetStorageGroup("LiveTV");

    if (WaitForNextLiveTVDir())
    {
        QMutexLocker lock(&nextLiveTVDirLock);
        prog->SetPathname(nextLiveTVDir);
    }
    else
    {
        StorageGroup sgroup("LiveTV", gCoreContext->GetHostName());
        prog->SetPathname(sgroup.FindNextDirMostFree());
    }

    StartedRecording(prog);

    *rb = new RingBuffer(prog->GetPathname(), true);
    if (!(*rb)->IsOpen())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("RingBuffer '%1' not open...")
                .arg(prog->GetPathname()));

        delete *rb;
        delete prog;

        return false;
    }

    *pginfo = prog;
    return true;
}

bool TVRec::CreateLiveTVRingBuffer(const QString & channum)
{
    VERBOSE(VB_RECORD, LOC + QString("CreateLiveTVRingBuffer(%1)")
            .arg(channum));

    RecordingInfo *pginfo = NULL;
    RingBuffer    *rb = NULL;
    QString        inputName;
    int            inputID = -1;

    if (!channel->CheckChannel(channum, inputName))
    {
        ChangeState(kState_None);
        return false;
    }

    inputID = inputName.isEmpty() ?
      channel->GetCurrentInputNum() : channel->GetInputByName(inputName);

    if (!GetProgramRingBufferForLiveTV(&pginfo, &rb, channum, inputID))
    {
        ClearFlags(kFlagPendingActions);
        ChangeState(kState_None);
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("CreateLiveTVRingBuffer(%1) failed").arg(channum));
        return false;
    }

    SetRingBuffer(rb);

    pginfo->SaveAutoExpire(kLiveTVAutoExpire);
    pginfo->ApplyRecordRecGroupChange("LiveTV");

    bool discont = (tvchain->TotalSize() > 0);
    tvchain->AppendNewProgram(pginfo, channel->GetCurrentName(),
                              channel->GetCurrentInput(), discont);

    if (curRecording)
    {
        curRecording->MarkAsInUse(false, kRecorderInUseID);
        delete curRecording;
    }

    curRecording = pginfo;
    curRecording->MarkAsInUse(true, kRecorderInUseID);

    return true;
}

bool TVRec::SwitchLiveTVRingBuffer(const QString & channum,
                                   bool discont, bool set_rec)
{
    VERBOSE(VB_RECORD, LOC + "SwitchLiveTVRingBuffer(discont "
            <<discont<<", set_rec "<<set_rec<<")");

    RecordingInfo *pginfo = NULL;
    RingBuffer    *rb = NULL;
    QString        inputName;
    int            inputID = -1;

    if (!channel->CheckChannel(channum, inputName))
    {
        ChangeState(kState_None);
        return false;
    }

    inputID = inputName.isEmpty() ?
      channel->GetCurrentInputNum() : channel->GetInputByName(inputName);

    if (!GetProgramRingBufferForLiveTV(&pginfo, &rb, channum, inputID))
    {
        ChangeState(kState_None);
        return false;
    }

    ProgramInfo *pi = tvchain->GetProgramAt(-1);
    if (pi)
    {
        RecordingInfo *oldinfo = new RecordingInfo(*pi);
        delete pi;
        FinishedRecording(oldinfo);
        if (tvchain->GetCardType(-1) != "DUMMY")
        {
            if (!oldinfo->IsLocal())
                oldinfo->SetPathname(oldinfo->GetPlaybackURL(false,true));
            if (oldinfo->IsLocal())
                PreviewGeneratorQueue::GetPreviewImage(*oldinfo, "");
        }
        delete oldinfo;
    }

    pginfo->MarkAsInUse(true, kRecorderInUseID);
    pginfo->SaveAutoExpire(kLiveTVAutoExpire);
    pginfo->ApplyRecordRecGroupChange("LiveTV");
    tvchain->AppendNewProgram(pginfo, channel->GetCurrentName(),
                              channel->GetCurrentInput(), discont);

    if (set_rec && recorder)
    {
        recorder->SetNextRecording(pginfo, rb);
        if (discont)
            recorder->CheckForRingBufferSwitch();
        delete pginfo;
        SetFlags(kFlagRingBufferReady);
    }
    else if (!set_rec)
    {
        if (curRecording)
        {
            curRecording->MarkAsInUse(false, kRecorderInUseID);
            delete curRecording;
        }
        curRecording = pginfo;
        SetRingBuffer(rb);
    }

    return true;
}

TVRec* TVRec::GetTVRec(uint cardid)
{
    QMutexLocker locker(&cardsLock);
    QMap<uint,TVRec*>::const_iterator it = cards.find(cardid);
    if (it == cards.end())
        return NULL;
    return *it;
}

QString TuningRequest::toString(void) const
{
    return QString("Program(%1) channel(%2) input(%3) flags(%4)")
        .arg((program != 0) ? "yes" : "no").arg(channel).arg(input)
        .arg(TVRec::FlagToString(flags));
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */

