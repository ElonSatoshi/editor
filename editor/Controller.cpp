/* Controller.cpp from QTau http://github.com/qtau-devgroup/editor by digited, BSD license */

#include "mainwindow.h"

#include "Session.h"
#include "Controller.h"
#include "PluginInterfaces.h"
#include "Utils.h"

#include "audio/Player.h"
#include "audio/codecs/Wav.h"
#include "audio/codecs/AIFF.h"
#include "audio/codecs/Flac.h"
#include "audio/codecs/Ogg.h"

#include <QApplication>
#include <QPluginLoader>


qtauController::qtauController(QObject *parent) :
    QObject(parent), activeSession(0)
{
    qtauCodecRegistry *cr = qtauCodecRegistry::instance();
    cr->addCodec(new qtauWavCodecFactory ());
    cr->addCodec(new qtauAIFFCodecFactory());
//    cr->addCodec(new qtauFlacCodecFactory());
//    cr->addCodec(new qtauOggCodecFactory ());

    player = new qtmmPlayer();

    setupTranslations();
    setupPlugins();
    setupVoicebanks();
}

qtauController::~qtauController()
{
    delete mw;
}

//------------------------------------------

bool qtauController::run()
{
    mw = new MainWindow();
    mw->show();

    // TODO: load previous one from settings
    newEmptySession();
    mw->setController(*this, *this->activeSession);

    return true;
}


bool qtauController::setupTranslations()
{
    return false;
}


bool qtauController::setupPlugins()
{
    pluginsDir = QDir(qApp->applicationDirPath());
    pluginsDir.cd("plugins");

    vsLog::i("Searching for plugins in " + pluginsDir.absolutePath());

    foreach (QString fileName, pluginsDir.entryList(QDir::Files))
    {
        QPluginLoader loader(pluginsDir.absoluteFilePath(fileName));
        QObject *plugin = loader.instance();

        if (plugin)
        {
            ISynth* s = qobject_cast<ISynth*>(plugin);

            if (s)
                initSynth(s);
        }
        else vsLog::d("Incompatible plugin: " + fileName);
    }

    return false;
}


void qtauController::initSynth(ISynth *s)
{
    if (!synths.contains(s->name()))
    {

        SSynthConfig sconf(*vsLog::instance());
        s->setup(sconf);

        vsLog::s("Adding synthesizer " + s->name());
        synths[s->name()] = s;
    }
    else vsLog::d("Synthesizer " + s->name() + " is already registered!");
}


bool qtauController::setupVoicebanks()
{
    return false;
}

void qtauController::newEmptySession()
{
    activeSession = new qtauSession(this);

    connect(activeSession, SIGNAL(requestSynthesis()),      SLOT(onRequestSynthesis     ()));
    connect(activeSession, SIGNAL(requestStartPlayback()),  SLOT(onRequestStartPlayback ()));
    connect(activeSession, SIGNAL(requestPausePlayback()),  SLOT(onRequestPausePlayback ()));
    connect(activeSession, SIGNAL(requestStopPlayback()),   SLOT(onRequestStopPlayback  ()));
    connect(activeSession, SIGNAL(requestResetPlayback()),  SLOT(onRequestResetPlayback ()));
    connect(activeSession, SIGNAL(requestRepeatPlayback()), SLOT(onRequestRepeatPlayback()));
}

//------------------------------------------

void qtauController::onLoadUST(QString fileName)
{
    if (!fileName.isEmpty())
    {
        if (!activeSession)
            newEmptySession();

        activeSession->loadUST(fileName);
    }
    else vsLog::d("Controller: empty UST file name");
}

void qtauController::onSaveUST(QString fileName, bool rewrite)
{
    if (activeSession && !activeSession->isSessionEmpty())
    {
        QFile uf(fileName);

        if (uf.open(QFile::WriteOnly))
        {
            if (uf.size() == 0 || rewrite)
            {
                uf.reset(); // maybe it's redundant?..
                uf.write(activeSession->ustBinary());
                uf.close();

                activeSession->setFilePath(fileName);
                activeSession->setSaved();

                vsLog::s("UST saved to " + fileName);
            }
            else vsLog::e("File " + fileName + " is not empty, rewriting cancelled");
        }
        else vsLog::e("Could not open file " + fileName + " to save UST");
    }
    else vsLog::e("Trying to save ust from empty session!");
}

void qtauController::onSaveAudio(QString fileName, bool rewrite)
{
    if (activeSession && activeSession->getVocal().vocalWave->size() > 0)
    {
        QFileInfo fi(fileName);
        QString ext = QFileInfo(fileName).suffix();

        if (fi.exists() && !rewrite)
        {
            vsLog::e(tr("File %1 exists, rewriting cancelled.").arg(fileName));
            return;
        }

        if (!isAudioExtSupported(ext))
        {
            vsLog::e(tr("No codec for ") + ext);
            return;
        }

        QFile af(fileName);

        if (!af.open(QFile::WriteOnly))
        {
            vsLog::e(tr("Could not open file %1 to save audio").arg(fileName));
            return;
        }

        af.reset();
        qtauAudioCodec *codec = codecForExt(QFileInfo(fileName).suffix(), af, this);

        if (codec)
        {
            // TODO: mixdown of vocal + bgm?

            // TODO: copy audio data when file io will be threaded
            qtauAudioSource *v = activeSession->getVocal().vocalWave;
            codec->setAudioFormat(v->getAudioFormat());
            codec->buffer() = v->buffer();
            codec->saveToDevice();
            delete codec;

            vsLog::s(tr("Audio saved: ") + fileName);
        }
        else vsLog::e(tr("Could not make a codec for ").arg(fileName));

        af.close();
    }
    else vsLog::e(tr("Trying to save audio from empty session!"));
}


void qtauController::onAppMessage(const QString &msg)
{
    vsLog::i("IPC message: " + msg);
    mw->setWindowState(Qt::WindowActive); // TODO: test
}

qtauAudioSource *tmpAS = 0;

void qtauController::pianoKeyPressed(int keyNum)
{
    if (!synths.isEmpty())
    {
        if (!tmpAS)
            tmpAS = new qtauAudioSource(this);

        ISynth *s = synths.values().first();
        ust u;
        u.tempo = 120;
        u.notes.append(ust_note(0, "a", 0, 480*3, keyNum)); // 480 pulses * 3 @ 120bpm is 3 notes, 1.5 sec
        s->setVocals(u);

        if (s->synthesize(*tmpAS))
            emit playEffect(*tmpAS);
    }
}

void qtauController::pianoKeyReleased(int /*keyNum*/)
{
    //qDebug() << "piano key released: " << octaveNum << keyNum;
}

void qtauController::onLoadAudio(QString fileName)
{
    if (!fileName.isEmpty())
    {
        QFileInfo fi(fileName);

        if (fi.exists() && !fi.isDir() && !fi.suffix().isEmpty())
        {
            QFile f(fileName);

            if (f.open(QFile::ReadOnly))
            {
                qtauAudioCodec  *ac = codecForExt(fi.suffix(), f, this);
                bool cached = ac->cacheAll();
                f.close();

                if (cached)
                    activeSession->setBackgroundAudio(*ac);
                else
                    vsLog::e("Error caching audio");

                f.close();
            }
            else vsLog::e("Could not open file " + fileName);
        }
        else vsLog::e("Wrong file name: " + fileName);
    }
    else vsLog::e("Controller was requested to load audio with empty filename! Spam hatemail to admin@microsoft.com");
}

void qtauController::onAudioPlaybackEnded()
{
//    if (playState.state == Repeating)
//        player->play();

    playState.state = Stopped;
    activeSession->setPlaybackState(EAudioPlayback::stopped);
}

void qtauController::onVolumeChanged(int level)
{
    player->setVolume(level);
}

void qtauController::onRequestSynthesis()
{
    if (!activeSession->isSessionEmpty())
    {
        if (!synths.isEmpty())
        {
            if (playState.state != Stopped)
            {
                player->stop();
                activeSession->setPlaybackState(EAudioPlayback::stopped);
            }

            ISynth *s = synths.values().first();

            s->setVocals(activeSession->ustRef());

            if (s->isVbReady() && s->isVocalsReady())
            {
                if (s->synthesize(*activeSession->getVocal().vocalWave))
                {
                    vsLog::s("Synthesis complete. Yay!");
                    activeSession->getVocal().needsSynthesis = false;
                    activeSession->vocalWaveWasModified();

                    onRequestStartPlayback();
                }
                else vsLog::e("Synthesis failed. Oh no!");
            }
            else vsLog::e("Synthesizer isn't ready for some reason...");
        }
        else vsLog::e("No synthesizers registered! What a shame!");
    }
    else vsLog::e("Session is empty, nothing to synthesize.");
}

void qtauController::onRequestStartPlayback()
{
    // play only vocal or only audio (depending on what's available), or a mixdown of both
    qtauSession::VocalWaveSetup &v = activeSession->getVocal();
    qtauSession::MusicWaveSetup &m = activeSession->getMusic();

    bool gotVocal = v.vocalWave && !v.vocalWave->buffer().isEmpty();
    bool gotMusic = m.musicWave && !m.musicWave->buffer().isEmpty();

    if (gotVocal)
    {
        if (gotMusic)
        {
            // play mixdown
        }
        else
        {
            playState.state = Playing;
            activeSession->setPlaybackState(EAudioPlayback::playing);

            if (!v.vocalWave->isOpen())
                v.vocalWave->open(QIODevice::ReadOnly);

            if (v.vocalWave->isOpen())
                v.vocalWave->reset();

            emit playTrack(*v.vocalWave);
        }
    }
    else if (gotMusic)
    {
        if (!(playState.state == Playing || playState.state == Repeating))
        {
            // play just audio
            if (playState.state != Paused)
            {
                if (!m.musicWave->isOpen())
                    m.musicWave->open(QIODevice::ReadOnly);

                m.musicWave->reset();
            }

            playState.state = Playing;
            activeSession->setPlaybackState(EAudioPlayback::playing);

            emit playTrack(*m.musicWave);
        }
        else vsLog::e("Controller is playing audio already, playback shouldn't be requested!");
    }
    else vsLog::e("Controller can't play anything - session has no audio data at all");
}

void qtauController::onRequestPausePlayback()
{
    if (playState.state == Playing || playState.state == Repeating)
    {
        playState.state = Paused;
        activeSession->setPlaybackState(EAudioPlayback::paused);
        player->pause();
    }
    else vsLog::e("Controller isn't playing anything, can't pause playback.");
}

void qtauController::onRequestStopPlayback()
{
    if (playState.state == Playing || playState.state == Repeating)
    {
        playState.state = Stopped;
        activeSession->setPlaybackState(EAudioPlayback::stopped);
        player->stop();
    }
    else vsLog::e("Controller isn't playing anything, can't pause playback.");
}

void qtauController::onRequestResetPlayback()
{
    player->stop();
    playState.state = Stopped;
    onRequestStartPlayback();
}

void qtauController::onRequestRepeatPlayback()
{
    //
}
