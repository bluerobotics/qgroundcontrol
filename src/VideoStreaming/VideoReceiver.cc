/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


/**
 * @file
 *   @brief QGC Video Receiver
 *   @author Gus Grubba <mavlink@grubba.com>
 */

#include "VideoReceiver.h"
#include "SettingsManager.h"
#include "QGCApplication.h"
#include "VideoManager.h"

#include <QDebug>
#include <QUrl>
#include <QDir>
#include <QDateTime>
#include <QSysInfo>

QGC_LOGGING_CATEGORY(VideoReceiverLog, "VideoReceiverLog")

#if defined(QGC_GST_STREAMING)

static const char* kVideoExtensions[] =
{
    "mkv",
    "mov",
    "mp4"
};

static const char* kVideoMuxes[] =
{
    "matroskamux",
    "qtmux",
    "mp4mux"
};

#define NUM_MUXES (sizeof(kVideoMuxes) / sizeof(char*))

#endif


VideoReceiver::VideoReceiver(QObject* parent)
    : QObject(parent)
#if defined(QGC_GST_STREAMING)
    , _running(false)
    , _recording(false)
    , _streaming(false)
    , _starting(false)
    , _stopping(false)
    , _sink(NULL)
    , _tee(NULL)
    , _volume(1.0)
    , _pipeline(NULL)
    , _pipelineStopRec(NULL)
    , _videoSink(NULL)
    , _audioPipeline(NULL)
    , _gstVolume(NULL)
    , _socket(NULL)
    , _serverPresent(false)
#endif
    , _videoSurface(NULL)
    , _videoRunning(false)
    , _showFullScreen(false)
{
    _videoSurface  = new VideoSurface;
#if defined(QGC_GST_STREAMING)
    _setVideoSink(_videoSurface->videoSink());
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &VideoReceiver::_timeout);
    connect(this, &VideoReceiver::msgErrorReceived, this, &VideoReceiver::_handleError);
    connect(this, &VideoReceiver::msgEOSReceived, this, &VideoReceiver::_handleEOS);
    connect(this, &VideoReceiver::msgStateChangedReceived, this, &VideoReceiver::_handleStateChanged);
    connect(&_frameTimer, &QTimer::timeout, this, &VideoReceiver::_updateTimer);
    _frameTimer.start(1000);
    _startAudio();
    loadSettings();
#endif
}

VideoReceiver::~VideoReceiver()
{
#if defined(QGC_GST_STREAMING)
    stop();
    if(_socket) {
        delete _socket;
    }
    if(_audioPipeline) {
        if(_gstVolume) {
            gst_object_unref(_gstVolume);
            _gstVolume = NULL;
        }
        gst_element_set_state(_audioPipeline, GST_STATE_NULL);
        gst_object_unref(_audioPipeline);
        _audioPipeline = NULL;
    }
    if (_videoSink) {
        gst_object_unref(_videoSink);
    }
    storeSettings();
#endif
    if(_videoSurface)
        delete _videoSurface;
}

#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_setVideoSink(GstElement* sink)
{
    if (_videoSink) {
        gst_object_unref(_videoSink);
        _videoSink = NULL;
    }
    if (sink) {
        _videoSink = sink;
        gst_object_ref_sink(_videoSink);
    }
}
#endif

//-----------------------------------------------------------------------------
void
VideoReceiver::grabImage(QString imageFile)
{
    _imageFile = imageFile;
    emit imageFileChanged();
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
static void
newPadCB(GstElement* element, GstPad* pad, gpointer data)
{
    gchar* name;
    name = gst_pad_get_name(pad);
    g_print("A new pad %s was created\n", name);
    GstCaps* p_caps = gst_pad_get_pad_template_caps (pad);
    gchar* description = gst_caps_to_string(p_caps);
    qCDebug(VideoReceiverLog) << p_caps << ", " << description;
    g_free(description);
    GstElement* p_rtph264depay = GST_ELEMENT(data);
    if(gst_element_link_pads(element, name, p_rtph264depay, "sink") == false)
        qCritical() << "newPadCB : failed to link elements\n";
    g_free(name);
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_connected()
{
    //-- Server showed up. Now we start the stream.
    _timer.stop();
    _socket->deleteLater();
    _socket = NULL;
    _serverPresent = true;
    start();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_socketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    _socket->deleteLater();
    _socket = NULL;
    //-- Try again in 5 seconds
    _timer.start(5000);
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_timeout()
{
    //-- If socket is live, we got no connection nor a socket error
    if(_socket) {
        delete _socket;
        _socket = NULL;
    }
    //-- RTSP will try to connect to the server. If it cannot connect,
    //   it will simply give up and never try again. Instead, we keep
    //   attempting a connection on this timer. Once a connection is
    //   found to be working, only then we actually start the stream.
    QUrl url(_uri);
    _socket = new QTcpSocket;
    connect(_socket, static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error), this, &VideoReceiver::_socketError);
    connect(_socket, &QTcpSocket::connected, this, &VideoReceiver::_connected);
    //qCDebug(VideoReceiverLog) << "Trying to connect to:" << url.host() << url.port();
    _socket->connectToHost(url.host(), url.port());
    _timer.start(5000);
}
#endif

//-----------------------------------------------------------------------------
// When we finish our pipeline will look like this:
//
//                                   +-->queue-->decoder-->_videosink
//                                   |
//    datasource-->demux-->parser-->tee
//
//                                   ^
//                                   |
//                                   +-Here we will later link elements for recording
void
VideoReceiver::start()
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "start()";

    if (_uri.isEmpty()) {
        qCritical() << "VideoReceiver::start() failed because URI is not specified";
        return;
    }
    if (_videoSink == NULL) {
        qCritical() << "VideoReceiver::start() failed because video sink is not set";
        return;
    }
    if(_running) {
        qCDebug(VideoReceiverLog) << "Already running!";
        return;
    }

    _starting = true;

    bool isUdp  = _uri.contains("udp://");
    bool isRtsp = _uri.contains("rtsp://");

    //-- For RTSP, check to see if server is there first
    if(!_serverPresent && isRtsp) {
        _timer.start(100);
        return;
    }

    bool running = false;

    GstElement*     dataSource  = NULL;
    GstCaps*        caps        = NULL;
    GstElement*     demux       = NULL;
    GstElement*     parser      = NULL;
    GstElement*     queue       = NULL;
    GstElement*     decoder     = NULL;

    do {
        if ((_pipeline = gst_pipeline_new("receiver")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_pipeline_new()";
            break;
        }

        if(isUdp) {
            dataSource = gst_element_factory_make("udpsrc", "udp-source");
        } else {
            dataSource = gst_element_factory_make("rtspsrc", "rtsp-source");
        }

        if (!dataSource) {
            qCritical() << "VideoReceiver::start() failed. Error with data source for gst_element_factory_make()";
            break;
        }

        if(isUdp) {
            if ((caps = gst_caps_from_string("application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264")) == NULL) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_caps_from_string()";
                break;
            }
            g_object_set(G_OBJECT(dataSource), "uri", qPrintable(_uri), "caps", caps, NULL);
        } else {
            g_object_set(G_OBJECT(dataSource), "location", qPrintable(_uri), "latency", 17, "udp-reconnect", 1, "timeout", static_cast<guint64>(5000000), NULL);
        }

        if ((demux = gst_element_factory_make("rtph264depay", "rtp-h264-depacketizer")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('rtph264depay')";
            break;
        }

        if(!isUdp) {
            g_signal_connect(dataSource, "pad-added", G_CALLBACK(newPadCB), demux);
        }

        if ((parser = gst_element_factory_make("h264parse", "h264-parser")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('h264parse')";
            break;
        }

        if ((decoder = gst_element_factory_make("avdec_h264", "h264-decoder")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('avdec_h264')";
            break;
        }

        if((_tee = gst_element_factory_make("tee", NULL)) == NULL)  {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('tee')";
            break;
        }

        if((queue = gst_element_factory_make("queue", NULL)) == NULL)  {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('queue')";
            break;
        }

        gst_bin_add_many(GST_BIN(_pipeline), dataSource, demux, parser, _tee, queue, decoder, _videoSink, NULL);

        if(isUdp) {
            // Link the pipeline in front of the tee
            if(!gst_element_link_many(dataSource, demux, parser, _tee, queue, decoder, _videoSink, NULL)) {
                qCritical() << "Unable to link elements.";
                break;
            }
        } else {
            if(!gst_element_link_many(demux, parser, _tee, queue, decoder, _videoSink, NULL)) {
                qCritical() << "Unable to link elements.";
                break;
            }
        }

        dataSource = demux = parser = queue = decoder = NULL;

        GstBus* bus = NULL;

        if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != NULL) {
            gst_bus_enable_sync_message_emission(bus);
            g_signal_connect(bus, "sync-message", G_CALLBACK(_onBusMessage), this);
            gst_object_unref(bus);
            bus = NULL;
        }

        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-paused");
        running = gst_element_set_state(_pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;

    } while(0);

    if (caps != NULL) {
        gst_caps_unref(caps);
        caps = NULL;
    }

    if (!running) {
        qCritical() << "VideoReceiver::start() failed";

        if (decoder != NULL) {
            gst_object_unref(decoder);
            decoder = NULL;
        }

        if (parser != NULL) {
            gst_object_unref(parser);
            parser = NULL;
        }

        if (demux != NULL) {
            gst_object_unref(demux);
            demux = NULL;
        }

        if (dataSource != NULL) {
            gst_object_unref(dataSource);
            dataSource = NULL;
        }

        if (_tee != NULL) {
            gst_object_unref(_tee);
            dataSource = NULL;
        }

        if (queue != NULL) {
            gst_object_unref(queue);
            dataSource = NULL;
        }

        if (_pipeline != NULL) {
            gst_object_unref(_pipeline);
            _pipeline = NULL;
        }

        _running = false;
    } else {
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-playing");
        _running = true;
        qCDebug(VideoReceiverLog) << "Running";
    }
    _starting = false;
#endif
}

void
VideoReceiver::_startAudio()
{
    // Run audio
    GError *error = NULL;
    _audioPipeline = gst_parse_launch("udpsrc port=5601 ! application/x-rtp, media=audio, clock-rate=44100, encoding-name=L16, encoding-params=1, channels=1, payload=96 ! rtpL16depay ! audioconvert ! volume volume=1.0 ! queue ! autoaudiosink sync=false", &error);

    if (_audioPipeline) {
        gst_element_set_state(_audioPipeline, GST_STATE_PLAYING);

        _gstVolume = gst_bin_get_by_name(GST_BIN(_audioPipeline), "volume0");
    }
}

void
VideoReceiver::setVolume(float vol)
{
    if(vol < 0 || vol > 1) {
        vol = 1;
    }

    _volume = vol;

    if(_gstVolume) {
        g_object_set(_gstVolume, "volume", _volume, NULL);
        qCDebug(VideoReceiverLog) << "Set volume:" << vol;
    } else {
        qCDebug(VideoReceiverLog) << "No volume control";
    }
}

//-----------------------------------------------------------------------------
void
VideoReceiver::stop()
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "stop()";
    if(!_streaming) {
        _shutdownPipeline();
    } else if (_pipeline != NULL && !_stopping) {
        qCDebug(VideoReceiverLog) << "Stopping _pipeline";
        gst_element_send_event(_pipeline, gst_event_new_eos());
        _stopping = true;
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
        GstMessage* message = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            _shutdownPipeline();
            qCritical() << "Error stopping pipeline!";
        } else if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            _handleEOS();
        }
        gst_message_unref(message);
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::setUri(const QString & uri)
{
    _uri = uri;
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_shutdownPipeline() {
    if(!_pipeline) {
        qCDebug(VideoReceiverLog) << "No pipeline";
        return;
    }
    GstBus* bus = NULL;
    if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != NULL) {
        gst_bus_disable_sync_message_emission(bus);
        gst_object_unref(bus);
        bus = NULL;
    }
    gst_element_set_state(_pipeline, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(_pipeline), _videoSink);
    gst_object_unref(_pipeline);
    _pipeline = NULL;
    delete _sink;
    _sink = NULL;
    _serverPresent = false;
    _streaming = false;
    _recording = false;
    _stopping = false;
    _running = false;
    emit recordingChanged();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleError() {
    qCDebug(VideoReceiverLog) << "Gstreamer error!";
    _shutdownPipeline();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleEOS() {
    if(_stopping) {
        _shutdownPipeline();
        qCDebug(VideoReceiverLog) << "Stopped";
    } else if(_recording && _sink->removing) {
        _shutdownRecordingBranch();
    } else {
        qCritical() << "VideoReceiver: Unexpected EOS!";
        _shutdownPipeline();
    }
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleStateChanged() {
    if(_pipeline) {
        _streaming = GST_STATE(_pipeline) == GST_STATE_PLAYING;
        qCDebug(VideoReceiverLog) << "State changed, _streaming:" << _streaming;
    }
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
gboolean
VideoReceiver::_onBusMessage(GstBus* bus, GstMessage* msg, gpointer data)
{
    Q_UNUSED(bus)
    Q_ASSERT(msg != NULL && data != NULL);
    VideoReceiver* pThis = (VideoReceiver*)data;

    switch(GST_MESSAGE_TYPE(msg)) {
    case(GST_MESSAGE_ERROR): {
        gchar* debug;
        GError* error;
        gst_message_parse_error(msg, &error, &debug);
        g_free(debug);
        qCritical() << error->message;
        g_error_free(error);
        pThis->msgErrorReceived();
    }
        break;
    case(GST_MESSAGE_EOS):
        pThis->msgEOSReceived();
        break;
    case(GST_MESSAGE_STATE_CHANGED):
        pThis->msgStateChangedReceived();
        break;
    default:
        break;
    }

    return TRUE;
}
#endif

//-----------------------------------------------------------------------------
// When we finish our pipeline will look like this:
//
//                                   +-->queue-->decoder-->_videosink
//                                   |
//    datasource-->demux-->parser-->tee
//                                   |
//                                   |    +--------------_sink-------------------+
//                                   |    |                                      |
//   we are adding these elements->  +->teepad-->queue-->matroskamux-->_filesink |
//                                        |                                      |
//                                        +--------------------------------------+
void
VideoReceiver::startRecording(void)
{
#if defined(QGC_GST_STREAMING)

    qCDebug(VideoReceiverLog) << "startRecording()";
    // exit immediately if we are already recording
    if(_pipeline == NULL || _recording) {
        qCDebug(VideoReceiverLog) << "Already recording!";
        return;
    }

    QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();
    if(savePath.isEmpty()) {
        qgcApp()->showMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    uint32_t muxIdx = qgcApp()->toolbox()->settingsManager()->videoSettings()->recordingFormat()->rawValue().toUInt();
    if(muxIdx >= NUM_MUXES) {
        qgcApp()->showMessage(tr("Invalid video format defined."));
        return;
    }

    _sink           = new Sink();
    _sink->teepad   = gst_element_get_request_pad(_tee, "src_%u");
    _sink->queue    = gst_element_factory_make("queue", NULL);
    _sink->parse    = gst_element_factory_make("h264parse", NULL);
    _sink->mux      = gst_element_factory_make(kVideoMuxes[muxIdx], NULL);
    _sink->filesink = gst_element_factory_make("filesink", NULL);
    _sink->removing = false;

    if(!_sink->teepad || !_sink->queue || !_sink->mux || !_sink->filesink || !_sink->parse) {
        qCritical() << "VideoReceiver::startRecording() failed to make _sink elements";
        return;
    }

    QString videoFile;
    videoFile = savePath + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") + "." + kVideoExtensions[muxIdx];

    g_object_set(G_OBJECT(_sink->filesink), "location", qPrintable(videoFile), NULL);
    qCDebug(VideoReceiverLog) << "New video file:" << videoFile;

    gst_object_ref(_sink->queue);
    gst_object_ref(_sink->parse);
    gst_object_ref(_sink->mux);
    gst_object_ref(_sink->filesink);

    gst_bin_add_many(GST_BIN(_pipeline), _sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);
    gst_element_link_many(_sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);

    gst_element_sync_state_with_parent(_sink->queue);
    gst_element_sync_state_with_parent(_sink->parse);
    gst_element_sync_state_with_parent(_sink->mux);
    gst_element_sync_state_with_parent(_sink->filesink);

    // Install a probe on the recording branch to drop buffers until we hit our first keyframe
    // When we hit our first keyframe, we can offset the timestamps appropriately according to the first keyframe time
    // This will ensure the first frame is a keyframe at t=0, and decoding can begin immediately on playback
    GstPad* probepad = gst_element_get_static_pad(_sink->queue, "src");
    gst_pad_add_probe(probepad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BUFFER /* | GST_PAD_PROBE_TYPE_BLOCK */), _keyframeWatch, this, NULL); // to drop the buffer or to block the buffer?
    gst_object_unref(probepad);

    // Link the recording branch to the pipeline
    GstPad* sinkpad = gst_element_get_static_pad(_sink->queue, "sink");
    gst_pad_link(_sink->teepad, sinkpad);
    gst_object_unref(sinkpad);

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-recording");

    _recording = true;
    emit recordingChanged();
    qCDebug(VideoReceiverLog) << "Recording started";
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::stopRecording(void)
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "stopRecording()";
    // exit immediately if we are not recording
    if(_pipeline == NULL || !_recording) {
        qCDebug(VideoReceiverLog) << "Not recording!";
        return;
    }
    // Wait for data block before unlinking
    gst_pad_add_probe(_sink->teepad, GST_PAD_PROBE_TYPE_IDLE, _unlinkCallBack, this, NULL);
#endif
}

//-----------------------------------------------------------------------------
// This is only installed on the transient _pipelineStopRec in order
// to finalize a video file. It is not used for the main _pipeline.
// -EOS has appeared on the bus of the temporary pipeline
// -At this point all of the recoring elements have been flushed, and the video file has been finalized
// -Now we can remove the temporary pipeline and its elements
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_shutdownRecordingBranch()
{
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->queue);
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->parse);
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->mux);
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->filesink);

    gst_element_set_state(_pipelineStopRec, GST_STATE_NULL);
    gst_object_unref(_pipelineStopRec);
    _pipelineStopRec = NULL;

    gst_element_set_state(_sink->filesink,  GST_STATE_NULL);
    gst_element_set_state(_sink->parse,     GST_STATE_NULL);
    gst_element_set_state(_sink->mux,       GST_STATE_NULL);
    gst_element_set_state(_sink->queue,     GST_STATE_NULL);

    gst_object_unref(_sink->queue);
    gst_object_unref(_sink->parse);
    gst_object_unref(_sink->mux);
    gst_object_unref(_sink->filesink);

    delete _sink;
    _sink = NULL;
    _recording = false;

    emit recordingChanged();
    qCDebug(VideoReceiverLog) << "Recording Stopped";
}
#endif

//-----------------------------------------------------------------------------
// -Unlink the recording branch from the tee in the main _pipeline
// -Create a second temporary pipeline, and place the recording branch elements into that pipeline
// -Setup watch and handler for EOS event on the temporary pipeline's bus
// -Send an EOS event at the beginning of that pipeline
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_detachRecordingBranch(GstPadProbeInfo* info)
{
    Q_UNUSED(info)

    // Also unlinks and unrefs
    gst_bin_remove_many(GST_BIN(_pipeline), _sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);

    // Give tee its pad back
    gst_element_release_request_pad(_tee, _sink->teepad);
    gst_object_unref(_sink->teepad);

    // Create temporary pipeline
    _pipelineStopRec = gst_pipeline_new("pipeStopRec");

    // Put our elements from the recording branch into the temporary pipeline
    gst_bin_add_many(GST_BIN(_pipelineStopRec), _sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);
    gst_element_link_many(_sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);

    // Add handler for EOS event
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipelineStopRec));
    gst_bus_enable_sync_message_emission(bus);
    g_signal_connect(bus, "sync-message", G_CALLBACK(_onBusMessage), this);
    gst_object_unref(bus);

    if(gst_element_set_state(_pipelineStopRec, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        qCDebug(VideoReceiverLog) << "problem starting _pipelineStopRec";
    }

    // Send EOS at the beginning of the pipeline
    GstPad* sinkpad = gst_element_get_static_pad(_sink->queue, "sink");
    gst_pad_send_event(sinkpad, gst_event_new_eos());
    gst_object_unref(sinkpad);
    qCDebug(VideoReceiverLog) << "Recording branch unlinked";
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
GstPadProbeReturn
VideoReceiver::_unlinkCallBack(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(pad);
    if(info != NULL && user_data != NULL) {
        VideoReceiver* pThis = (VideoReceiver*)user_data;
        // We will only act once
        if(g_atomic_int_compare_and_exchange(&pThis->_sink->removing, FALSE, TRUE)) {
            pThis->_detachRecordingBranch(info);
        }
    }
    return GST_PAD_PROBE_REMOVE;
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
GstPadProbeReturn
VideoReceiver::_keyframeWatch(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(pad);
    if(info != NULL && user_data != NULL) {
        GstBuffer* buf = gst_pad_probe_info_get_buffer(info);
        if(GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) { // wait for a keyframe
            return GST_PAD_PROBE_DROP;
        } else {
            VideoReceiver* pThis = (VideoReceiver*)user_data;
            // reset the clock
            GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(pThis->_pipeline));
            GstClockTime time = gst_clock_get_time(clock);
            gst_object_unref(clock);
            gst_element_set_base_time(pThis->_pipeline, time); // offset pipeline timestamps to start at zero again
            buf->dts = 0; // The offset will not apply to this current buffer, our first frame, timestamp is zero
            buf->pts = 0;
            qCDebug(VideoReceiverLog) << "Got keyframe, stop dropping buffers";
        }
    }

    return GST_PAD_PROBE_REMOVE;
}
#endif

//-----------------------------------------------------------------------------
void
VideoReceiver::_updateTimer()
{
#if defined(QGC_GST_STREAMING)
    if(_videoSurface) {
        if(stopping() || starting()) {
            return;
        }
        if(streaming()) {
            if(!_videoRunning) {
                _videoSurface->setLastFrame(0);
                _videoRunning = true;
                emit videoRunningChanged();
            }
        } else {
            if(_videoRunning) {
                _videoRunning = false;
                emit videoRunningChanged();
            }
        }
        if(_videoRunning) {
            time_t elapsed = 0;
            time_t lastFrame = _videoSurface->lastFrame();
            if(lastFrame != 0) {
                elapsed = time(0) - _videoSurface->lastFrame();
            }
            if(elapsed > 2 && _videoSurface) {
                stop();
            }
        } else {
            if(!running() && !_uri.isEmpty()) {
                start();
            }
        }
    }
#endif
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::loadSettings()
{
    // Load defaults from settings
    QSettings settings;
    settings.beginGroup("QGC_VIDEORECEIVER");
    if(settings.contains("VOLUME_LEVEL")) {
        setVolume(settings.value("VOLUME_LEVEL").toFloat());
    }
}

void VideoReceiver::storeSettings()
{
    // Store settings
    QSettings settings;
    settings.beginGroup("QGC_VIDEORECEIVER");
    settings.setValue("VOLUME_LEVEL", volume());
}
#endif