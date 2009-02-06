/* GStreamer
* Copyright (C) 2005 Sebastien Moutte <sebastien@moutte.net>
* Copyright (C) 2007-2009 Pioneers of the Inevitable <songbird@songbirdnest.com>
*
* gstdirectsoundsink.c:
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Library General Public
* License as published by the Free Software Foundation; either
* version 2 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Library General Public License for more details.
*
* You should have received a copy of the GNU Library General Public
* License along with this library; if not, write to the
* Free Software Foundation, Inc., 59 Temple Place - Suite 330,
* Boston, MA 02111-1307, USA.
*
*
* The development of this code was made possible due to the involvement
* of Pioneers of the Inevitable, the creators of the Songbird Music player
*
*/

/**
 * SECTION:element-directsoundsink
 *
 * This element lets you output sound using the DirectSound API.
 *
 * Note that you should almost always use generic audio conversion elements
 * like audioconvert and audioresample in front of an audiosink to make sure
 * your pipeline works under all circumstances (those conversion elements will
 * act in passthrough-mode if no conversion is necessary).
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch -v audiotestsrc ! audioconvert ! volume volume=0.1 ! directsoundsink
 * ]| will output a sine wave (continuous beep sound) to your sound card (with
 * a very low volume as precaution).
 * |[
 * gst-launch -v filesrc location=music.ogg ! decodebin ! audioconvert ! audioresample ! directsoundsink
 * ]| will play an Ogg/Vorbis audio file and output it.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define INITGUID

#include <gst/audio/multichannel.h>
#include "gstdirectsoundsink.h"

#include <math.h>

#define MAX_LOST_RETRIES 10
#define THREAD_ERROR_BUFFER_RESTORE 1
#define THREAD_ERROR_NO_POSITION    2

/* elementfactory information */
static const GstElementDetails gst_directsound_sink_details =
GST_ELEMENT_DETAILS ("DirectSound8 Audio Sink",
    "Sink/Audio",
    "Output to a sound card via DirectSound8",
    "Ghislain 'Aus' Lacroix <aus@songbirdnest.com>");

static void gst_directsound_sink_base_init (gpointer g_class);
static void gst_directsound_sink_class_init (GstDirectSoundSinkClass * klass);
static void gst_directsound_sink_init (GstDirectSoundSink * dsoundsink,
    GstDirectSoundSinkClass * g_class);

static gboolean gst_directsound_sink_event (GstBaseSink * bsink, GstEvent * event);

static void gst_directsound_sink_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec);
static void gst_directsound_sink_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec);

static GstRingBuffer *gst_directsound_sink_create_ringbuffer (GstBaseAudioSink *
    sink);

static GstStaticPadTemplate directsoundsink_sink_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw-int, "
        "endianness = (int) LITTLE_ENDIAN, "
        "signed = (boolean) TRUE, "
        "width = (int) {8, 16}, "
        "depth = (int) {8, 16}, "
        "rate = (int) [ 1, MAX ], " 
        "channels = (int) [ 1, 2 ]"));

enum {
  ARG_0,
  ARG_VOLUME
};

GST_BOILERPLATE (GstDirectSoundSink, gst_directsound_sink, GstBaseAudioSink,
    GST_TYPE_BASE_AUDIO_SINK);

static void gst_directsound_ring_buffer_class_init (GstDirectSoundRingBufferClass * klass);
static void gst_directsound_ring_buffer_init (GstDirectSoundRingBuffer * ringbuffer,
    GstDirectSoundRingBufferClass * g_class);
static void gst_directsound_ring_buffer_dispose (GObject * object);
static void gst_directsound_ring_buffer_finalize (GObject * object);
static gboolean gst_directsound_ring_buffer_open_device (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_close_device (GstRingBuffer * buf);

static gboolean gst_directsound_ring_buffer_acquire (GstRingBuffer * buf,
    GstRingBufferSpec * spec);
static gboolean gst_directsound_ring_buffer_release (GstRingBuffer * buf);

static gboolean gst_directsound_ring_buffer_start (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_pause (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_resume (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_stop (GstRingBuffer * buf);
static guint gst_directsound_ring_buffer_delay (GstRingBuffer * buf);

static DWORD WINAPI gst_directsound_write_proc (LPVOID lpParameter);

static void gst_directsound_ring_buffer_class_init (GstDirectSoundRingBufferClass *g_class);
static void gst_directsound_ring_buffer_init (GstDirectSoundRingBuffer *object,	GstDirectSoundRingBufferClass *g_class);
static GstRingBufferClass *ring_parent_class = NULL;
static void gst_directsound_ring_buffer_class_init_trampoline (gpointer g_class, gpointer data)
{
  ring_parent_class = (GstRingBufferClass *) g_type_class_peek_parent (g_class);
  gst_directsound_ring_buffer_class_init ((GstDirectSoundRingBufferClass *)g_class);
}									
									
GType gst_directsound_ring_buffer_get_type (void);

GType
gst_directsound_ring_buffer_get_type (void)
{
  static volatile gsize gonce_data;
  if (__gst_once_init_enter (&gonce_data)) {
    GType _type;
    _type = gst_type_register_static_full (GST_TYPE_RING_BUFFER,
        g_intern_static_string ("GstDirectSoundRingBuffer"),
	      sizeof (GstDirectSoundRingBufferClass),
        NULL,
        NULL,
        gst_directsound_ring_buffer_class_init_trampoline,
        NULL,
        NULL,
        sizeof (GstDirectSoundRingBuffer),
        0,
        (GInstanceInitFunc) gst_directsound_ring_buffer_init,
        NULL,
        (GTypeFlags) 0);
    __gst_once_init_leave (&gonce_data, (gsize) _type);
  }
  return (GType) gonce_data;
}

static void 
directsound_set_volume (LPDIRECTSOUNDBUFFER8 pDSB8, gdouble volume)
{
  HRESULT hr;
  long dsVolume;

  /* DirectSound controls volume using units of 100th of a decibel,
   * ranging from -10000 to 0. We use a linear scale of 0 - 100
   * here, so remap.
   */
  if (volume == 0)
    dsVolume = -10000;
  else
    dsVolume = 100 * 
      (long)(20 * log10 (volume));
  dsVolume = CLAMP (dsVolume, -10000, 0);

  GST_DEBUG ("Setting volume on secondary buffer to %d", (int)dsVolume);
  
  hr = IDirectSoundBuffer8_SetVolume (pDSB8, dsVolume);
  if (G_UNLIKELY (FAILED(hr))) {
    GST_WARNING ("Setting volume on secondary buffer failed.");
  }
}

static void
gst_directsound_sink_set_volume (GstDirectSoundSink *dsoundsink)
{
  if (dsoundsink->dsoundbuffer && dsoundsink->dsoundbuffer->pDSB8) {
    GST_DSOUND_LOCK (dsoundsink->dsoundbuffer);
    dsoundsink->dsoundbuffer->volume = dsoundsink->volume;
    directsound_set_volume(dsoundsink->dsoundbuffer->pDSB8, 
      dsoundsink->volume);
    GST_DSOUND_UNLOCK (dsoundsink->dsoundbuffer);
  }
}

static void
gst_directsound_sink_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details (element_class, &gst_directsound_sink_details);
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&directsoundsink_sink_factory));
}

static void
gst_directsound_sink_class_init (GstDirectSoundSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstBaseAudioSinkClass *gstbaseaudiosink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstbaseaudiosink_class = (GstBaseAudioSinkClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_directsound_sink_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_directsound_sink_get_property);

  g_object_class_install_property (gobject_class, ARG_VOLUME,
      g_param_spec_double ("volume", "Volume", "Volume of this stream",
          0, 1.0, 1.0, G_PARAM_READWRITE));

  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_directsound_sink_event);

  gstbaseaudiosink_class->create_ringbuffer =
      GST_DEBUG_FUNCPTR (gst_directsound_sink_create_ringbuffer);
}

static void
gst_directsound_sink_init (GstDirectSoundSink * dsoundsink,
    GstDirectSoundSinkClass * g_class)
{
  dsoundsink->dsoundbuffer = NULL;
}

static gboolean
gst_directsound_sink_event (GstBaseSink * bsink, GstEvent * event)
{
  HRESULT hr;
  DWORD dwStatus;
  DWORD dwSizeBuffer = 0;
  LPVOID pLockedBuffer = NULL;

  GstDirectSoundSink *dsoundsink;

  dsoundsink = GST_DIRECTSOUND_SINK (bsink);

  GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);

  /* no buffer, no event to process */
  if (!dsoundsink->dsoundbuffer)
    return TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_DSOUND_LOCK (dsoundsink->dsoundbuffer);
      dsoundsink->dsoundbuffer->flushing = TRUE;
      GST_DSOUND_UNLOCK (dsoundsink->dsoundbuffer);
      break;
    case GST_EVENT_FLUSH_STOP:
      GST_DSOUND_LOCK (dsoundsink->dsoundbuffer);
      dsoundsink->dsoundbuffer->flushing = FALSE;

      if (dsoundsink->dsoundbuffer->pDSB8) {
        hr = IDirectSoundBuffer8_GetStatus (dsoundsink->dsoundbuffer->pDSB8, &dwStatus);

        if (FAILED(hr)) {
          GST_WARNING("gst_directsound_sink_event: IDirectSoundBuffer8_GetStatus, hr = %X", hr);
          return FALSE;
        }

        if (!(dwStatus & DSBSTATUS_PLAYING)) {
          /*reset position */
          hr = IDirectSoundBuffer8_SetCurrentPosition (dsoundsink->dsoundbuffer->pDSB8, 0);
          dsoundsink->dsoundbuffer->buffer_write_offset = 0;

          /*reset the buffer */
          hr = IDirectSoundBuffer8_Lock (dsoundsink->dsoundbuffer->pDSB8,
              dsoundsink->dsoundbuffer->buffer_write_offset, 0L,
              &pLockedBuffer, &dwSizeBuffer, NULL, NULL, DSBLOCK_ENTIREBUFFER);

          if (SUCCEEDED (hr)) {
            memset (pLockedBuffer, 0, dwSizeBuffer);

            hr =
                IDirectSoundBuffer8_Unlock (dsoundsink->dsoundbuffer->pDSB8, pLockedBuffer,
                dwSizeBuffer, NULL, 0);

            if (FAILED(hr)) {
              GST_WARNING("gst_directsound_sink_event: IDirectSoundBuffer8_Unlock, hr = %X", hr);
              return FALSE;
            }
          } else {
            GST_WARNING ("gst_directsound_sink_event: IDirectSoundBuffer8_Lock, hr = %X", hr);
            return FALSE;
          }
        }
      }
      GST_DSOUND_UNLOCK (dsoundsink->dsoundbuffer);
      break;
    default:
      break;
  }

  return TRUE;
}

static void
gst_directsound_sink_set_property (GObject *object,
    guint prop_id, const GValue *value , GParamSpec *pspec)
{
  GstDirectSoundSink *sink = GST_DIRECTSOUND_SINK (object);

  switch (prop_id) {
    case ARG_VOLUME:
      sink->volume = g_value_get_double (value);
      gst_directsound_sink_set_volume (sink);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_directsound_sink_get_property (GObject *object,
    guint prop_id, GValue *value , GParamSpec *pspec)
{
  GstDirectSoundSink *sink = GST_DIRECTSOUND_SINK (object);

  switch (prop_id) {
    case ARG_VOLUME:
      g_value_set_double (value, sink->volume);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstBaseAudioSink vmethod implementations */
static GstRingBuffer *
gst_directsound_sink_create_ringbuffer (GstBaseAudioSink * sink)
{
  GstDirectSoundSink *dsoundsink;
  GstDirectSoundRingBuffer *ringbuffer;

  dsoundsink = GST_DIRECTSOUND_SINK (sink);

  GST_DEBUG ("creating ringbuffer");
  ringbuffer = g_object_new (GST_TYPE_DIRECTSOUND_RING_BUFFER, NULL);
  GST_DEBUG ("directsound sink 0x%p", dsoundsink);

  /* set the sink element on the ringbuffer for error messages */
  ringbuffer->dsoundsink = dsoundsink;

  /* set the ringbuffer on the sink */
  dsoundsink->dsoundbuffer = ringbuffer;

  /* set initial volume on ringbuffer */
  dsoundsink->dsoundbuffer->volume = dsoundsink->volume;

  return GST_RING_BUFFER (ringbuffer);
}

/* 
 * DirectSound RingBuffer Implementation
 */

static void
gst_directsound_ring_buffer_class_init (GstDirectSoundRingBufferClass * klass)
{
  GObjectClass *gobject_class;
  GstObjectClass *gstobject_class;
  GstRingBufferClass *gstringbuffer_class;

  gobject_class = (GObjectClass *) klass;
  gstobject_class = (GstObjectClass *) klass;
  gstringbuffer_class = (GstRingBufferClass *) klass;

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_finalize);

  gstringbuffer_class->open_device =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_open_device);
  gstringbuffer_class->close_device =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_close_device);
  gstringbuffer_class->acquire =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_acquire);
  gstringbuffer_class->release =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_release);
  gstringbuffer_class->start = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_start);
  gstringbuffer_class->pause = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_pause);
  gstringbuffer_class->resume = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_resume);
  gstringbuffer_class->stop = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_stop);

  gstringbuffer_class->delay = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_delay);

  GST_DEBUG ("directsound ring buffer class init");
}

static void
gst_directsound_ring_buffer_init (GstDirectSoundRingBuffer * ringbuffer,
    GstDirectSoundRingBufferClass * g_class)
{
  ringbuffer->dsoundsink = NULL;
  ringbuffer->pDS8 = NULL;
  ringbuffer->pDSB8 = NULL;

  ringbuffer->buffer_size = 0;
  ringbuffer->buffer_write_offset = 0;
  
  ringbuffer->min_buffer_size = 0;
  ringbuffer->min_sleep_time = 10; /* in milliseconds */

  ringbuffer->bytes_per_sample = 0;
  ringbuffer->segoffset = 0;
  ringbuffer->segsize = 0;

  ringbuffer->hThread = NULL;
  ringbuffer->suspended = FALSE;
  ringbuffer->should_run = FALSE;
  ringbuffer->flushing = FALSE;

  ringbuffer->volume = 1.0;

  ringbuffer->dsound_lock = g_mutex_new ();

}

static void
gst_directsound_ring_buffer_dispose (GObject * object)
{
  G_OBJECT_CLASS (ring_parent_class)->dispose (object);
}

static void
gst_directsound_ring_buffer_finalize (GObject * object)
{
  GstDirectSoundRingBuffer *dsoundbuffer = 
     GST_DIRECTSOUND_RING_BUFFER (object);
  
  g_mutex_free (dsoundbuffer->dsound_lock);
  dsoundbuffer->dsound_lock = NULL;

  G_OBJECT_CLASS (ring_parent_class)->finalize (object);
}

static gboolean
gst_directsound_ring_buffer_open_device (GstRingBuffer * buf)
{  
  HRESULT hr;
  GstDirectSoundRingBuffer *dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  if (FAILED (hr = DirectSoundCreate8 (NULL, &dsoundbuffer->pDS8, NULL))) {
    GST_ELEMENT_ERROR (dsoundbuffer->dsoundsink, RESOURCE, FAILED, 
      ("%S.", DXGetErrorDescription9(hr)), 
      ("Failed to create directsound device. (%X)", hr));
    dsoundbuffer->pDS8 = NULL;
    return FALSE;
  }

  if (FAILED (hr = IDirectSound8_SetCooperativeLevel (dsoundbuffer->pDS8,
              GetDesktopWindow (), DSSCL_PRIORITY))) {
    GST_WARNING ("gst_directsound_sink_open: IDirectSound8_SetCooperativeLevel , hr = %X", hr);    
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_close_device (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer *dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);
  
  if (dsoundbuffer->pDS8) {
    IDirectSound8_Release (dsoundbuffer->pDS8);
    dsoundbuffer->pDS8 = NULL;
  }

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_acquire (GstRingBuffer * buf, GstRingBufferSpec * spec)
{
  GstDirectSoundRingBuffer *dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);
  HRESULT hr;
  DSBUFFERDESC descSecondary;
  LPDIRECTSOUNDBUFFER pDSB;
  WAVEFORMATEX wfx;

  /* sanity check, if no DirectSound device, bail out */
  if (!dsoundbuffer->pDS8) {
    GST_WARNING ("gst_directsound_ring_buffer_acquire: DirectSound 8 device is null!");
    return FALSE;
  }

  /*save number of bytes per sample */
  dsoundbuffer->bytes_per_sample = spec->bytes_per_sample;

  /* fill the WAVEFORMATEX struture with spec params */
  memset (&wfx, 0, sizeof (wfx));
  wfx.cbSize = sizeof (wfx);
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = spec->channels;
  wfx.nSamplesPerSec = spec->rate;
  wfx.wBitsPerSample = (spec->bytes_per_sample * 8) / wfx.nChannels;
  wfx.nBlockAlign = spec->bytes_per_sample;
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  /* Create directsound buffer with size based on our configured 
   * buffer_size (which is 200 ms by default) */
  dsoundbuffer->buffer_size = gst_util_uint64_scale_int (wfx.nAvgBytesPerSec, spec->buffer_time, GST_MSECOND);

  spec->segsize = gst_util_uint64_scale_int (wfx.nAvgBytesPerSec, spec->latency_time, GST_MSECOND);
  /* Now round the ringbuffer segment size to a multiple of the bytes per sample - 
     otherwise the ringbuffer subtly fails */
  spec->segsize = (spec->segsize + (spec->bytes_per_sample - 1))/ spec->bytes_per_sample * spec->bytes_per_sample;

  /* And base the total number of segments on the configured buffer size */
  spec->segtotal = dsoundbuffer->buffer_size / spec->segsize;
  
  dsoundbuffer->buffer_size = spec->segsize * spec->segtotal;
  dsoundbuffer->segsize = spec->segsize;
  dsoundbuffer->min_buffer_size = dsoundbuffer->buffer_size / 2;

  GST_INFO_OBJECT (dsoundbuffer,
      "GstRingBufferSpec->channels: %d, GstRingBufferSpec->rate: %d, GstRingBufferSpec->bytes_per_sample: %d\n"
      "WAVEFORMATEX.nSamplesPerSec: %ld, WAVEFORMATEX.wBitsPerSample: %d, WAVEFORMATEX.nBlockAlign: %d, WAVEFORMATEX.nAvgBytesPerSec: %ld\n"
      "Size of dsound cirucular buffer: %d, Size of segment: %d, Total segments: %d\n", spec->channels, spec->rate,
      spec->bytes_per_sample, wfx.nSamplesPerSec, wfx.wBitsPerSample,
      wfx.nBlockAlign, wfx.nAvgBytesPerSec, dsoundbuffer->buffer_size, spec->segsize, spec->segtotal);

  /* create a secondary directsound buffer */
  memset (&descSecondary, 0, sizeof (DSBUFFERDESC));
  descSecondary.dwSize = sizeof (DSBUFFERDESC);
  descSecondary.dwFlags = DSBCAPS_GETCURRENTPOSITION2 |
      DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;

  descSecondary.dwBufferBytes = dsoundbuffer->buffer_size;
  descSecondary.lpwfxFormat = (WAVEFORMATEX *) & wfx;

  hr = IDirectSound8_CreateSoundBuffer (dsoundbuffer->pDS8, &descSecondary,
      &pDSB, NULL);
  if (G_UNLIKELY (FAILED (hr))) {
    GST_WARNING("gst_directsound_ring_buffer_acquire: IDirectSound8_CreateSoundBuffer, hr = %X", hr);
    return FALSE;
  }

  hr = IDirectSoundBuffer_QueryInterface (pDSB, &IID_IDirectSoundBuffer8, &dsoundbuffer->pDSB8);
  if (G_UNLIKELY (FAILED (hr))) {
    IDirectSoundBuffer_Release (pDSB);
    GST_WARNING("gst_directsound_ring_buffer_acquire: IDirectSoundBuffer_QueryInterface, hr = %X", hr);
    return FALSE;
  }

  IDirectSoundBuffer_Release (pDSB);

  buf->data = gst_buffer_new_and_alloc (spec->segtotal * spec->segsize);
  memset (GST_BUFFER_DATA (buf->data), 0, GST_BUFFER_SIZE (buf->data));

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_release (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer *dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  /* first we have to ensure our ring buffer is stopped */
  gst_directsound_ring_buffer_stop (buf);
  
  GST_DSOUND_LOCK (dsoundbuffer);

  /* release secondary DirectSound buffer */
  if (dsoundbuffer->pDSB8) {
    IDirectSoundBuffer8_Release (dsoundbuffer->pDSB8);
    dsoundbuffer->pDSB8 = NULL;
  }

  gst_buffer_unref (buf->data);
  buf->data = NULL;

  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_start (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer *dsoundbuffer;
  HANDLE hThread;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);
  
  GST_DSOUND_LOCK (dsoundbuffer);

  hThread = CreateThread (NULL, 256 * 1024 /* Stack size: 256k */, 
     gst_directsound_write_proc, buf, CREATE_SUSPENDED, NULL);

  if (!hThread) {
    GST_WARNING ("gst_directsound_ring_buffer_start: CreateThread");
    GST_DSOUND_UNLOCK (dsoundbuffer);
    return FALSE;
  }

  dsoundbuffer->hThread = hThread;
  dsoundbuffer->should_run = TRUE;

  directsound_set_volume (dsoundbuffer->pDSB8, dsoundbuffer->volume);

  if (G_UNLIKELY (!SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL)))
    GST_WARNING ("gst_directsound_ring_buffer_start: Failed to set thread priority.");

  ResumeThread (hThread);

  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_pause (GstRingBuffer * buf)
{
  HRESULT hr;
  GstDirectSoundRingBuffer *dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DSOUND_LOCK (dsoundbuffer);

  hr = IDirectSoundBuffer8_Stop (dsoundbuffer->pDSB8);

  if (G_LIKELY (!dsoundbuffer->suspended)) {
    if (G_UNLIKELY(SuspendThread (dsoundbuffer->hThread) == -1))
      GST_WARNING ("gst_directsound_ring_buffer_pause: SuspendThread failed.");
    else 
      dsoundbuffer->suspended = TRUE;
  }

  GST_DSOUND_UNLOCK (dsoundbuffer);

  if (G_UNLIKELY (FAILED(hr))) {
    GST_WARNING ("gst_directsound_ring_buffer_pause: IDirectSoundBuffer8_Stop, hr = %X", hr);
    return FALSE;
  }
 
  return TRUE;
}

static gboolean 
gst_directsound_ring_buffer_resume (GstRingBuffer * buf)
{
  HRESULT hr;
  GstDirectSoundRingBuffer *dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DSOUND_LOCK (dsoundbuffer);

  if (G_LIKELY (dsoundbuffer->suspended) && 
      ResumeThread (dsoundbuffer->hThread) != -1) {
    dsoundbuffer->suspended = FALSE;
  } else {
    GST_WARNING ("gst_directsound_ring_buffer_resume: ResumeThread failed.");
    GST_DSOUND_UNLOCK (dsoundbuffer);
    return FALSE;
  }

  hr = IDirectSoundBuffer8_Play (dsoundbuffer->pDSB8, 0, 0, DSBPLAY_LOOPING);
  
  GST_DSOUND_UNLOCK (dsoundbuffer);

  if (G_UNLIKELY (FAILED(hr))) {
    GST_WARNING ("gst_directsound_ring_buffer_resume: IDirectSoundBuffer8_Start, hr = %X", hr);
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_stop (GstRingBuffer * buf)
{
  HRESULT hr;
  DWORD ret;
  HANDLE hThread;
  GstDirectSoundRingBuffer *dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DSOUND_LOCK (dsoundbuffer);
  dsoundbuffer->should_run = FALSE;
  hr = IDirectSoundBuffer8_Stop (dsoundbuffer->pDSB8);

  if (G_UNLIKELY (FAILED(hr))) {
    GST_WARNING ("gst_directsound_ring_buffer_stop: IDirectSoundBuffer8_Stop, hr = %X", hr);
    GST_DSOUND_UNLOCK (dsoundbuffer);
    return FALSE;
  }

  hThread = dsoundbuffer->hThread;

  if (dsoundbuffer->suspended && 
      ResumeThread (hThread) != -1) {
    dsoundbuffer->suspended = FALSE;
  } else {
    GST_WARNING ("gst_directsound_ring_buffer_stop: ResumeThread failed.");
    return FALSE;
  }

  GST_DSOUND_UNLOCK (dsoundbuffer);

  /* wait without lock held */
  ret = WaitForSingleObject (hThread, 5000);

  if (G_UNLIKELY (ret == WAIT_TIMEOUT)) {
    GST_WARNING ("gst_directsound_ring_buffer_stop: Failed to wait for thread shutdown. (%d)", ret);
    return FALSE;
  }

  GST_DSOUND_LOCK (dsoundbuffer);
  CloseHandle (dsoundbuffer->hThread);
  dsoundbuffer->hThread = NULL;
  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static guint
gst_directsound_ring_buffer_delay (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer *dsoundbuffer;
  HRESULT hr;
  DWORD dwCurrentPlayCursor;
  DWORD dwCurrentWriteCursor;
  DWORD dwBytesInQueue = 0;
  gint nNbSamplesInQueue = 0;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  if (G_LIKELY (dsoundbuffer->pDSB8)) {
    /*evaluate the number of samples in queue in the circular buffer */
    hr = IDirectSoundBuffer8_GetCurrentPosition (dsoundbuffer->pDSB8,
        &dwCurrentPlayCursor, &dwCurrentWriteCursor);

    if (G_LIKELY (SUCCEEDED (hr))) {
      if (dwCurrentPlayCursor <= dsoundbuffer->buffer_write_offset)
        dwBytesInQueue = dsoundbuffer->buffer_size - 
          (dsoundbuffer->buffer_write_offset - dwCurrentPlayCursor);
      else
        dwBytesInQueue = dwCurrentPlayCursor - dsoundbuffer->buffer_write_offset;

      nNbSamplesInQueue = dwBytesInQueue / dsoundbuffer->bytes_per_sample;
    } else {
      GST_WARNING ("gst_directsound_ring_buffer_delay: IDirectSoundBuffer8_GetCurrentPosition, hr = %X", hr);
    }
  }

  return nNbSamplesInQueue;
}

static DWORD WINAPI
gst_directsound_write_proc (LPVOID lpParameter)
{
  GstRingBuffer *buf;
  GstDirectSoundRingBuffer *dsoundbuffer;

  HRESULT hr;
  DWORD dwStatus;
  LPVOID pLockedBuffer1 = NULL, pLockedBuffer2 = NULL;
  DWORD dwSizeBuffer1 = 0, dwSizeBuffer2 = 0;
  DWORD dwCurrentPlayCursor = 0;

  gint64 freeBufferSize = 0;

  guint8 *readptr = NULL;
  gint readseg = 0;
  guint len = 0;
  gint retries = 0;

  gboolean flushing = FALSE;
  gboolean should_run = TRUE;

  buf = (GstRingBuffer *)lpParameter;
  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  do {

    GST_DSOUND_LOCK (dsoundbuffer);

    if (dsoundbuffer->flushing) 
      goto complete;

  restore_buffer:
    /* get current buffer status */
    hr = IDirectSoundBuffer8_GetStatus (dsoundbuffer->pDSB8, &dwStatus);

    if (dwStatus & DSBSTATUS_BUFFERLOST) {
      hr = IDirectSoundBuffer8_Restore (dsoundbuffer->pDSB8);

      /* restore may fail again, ensure we restore the 
       * buffer before we continue */
      if (FAILED(hr) && hr == DSERR_BUFFERLOST) {
        if (retries++ < MAX_LOST_RETRIES) {
          goto restore_buffer;
        } else {
          GST_WARNING ("gst_directsound_write_proc: IDirectSoundBuffer8_Restore, hr = %X", hr);
          GST_DSOUND_UNLOCK (dsoundbuffer);
          return THREAD_ERROR_BUFFER_RESTORE;
        }
      }
    }

    /* get current play cursor and write cursor positions */
    hr = IDirectSoundBuffer8_GetCurrentPosition (dsoundbuffer->pDSB8,
        &dwCurrentPlayCursor, NULL);

    if (G_UNLIKELY (FAILED(hr))) {
      GST_WARNING("gst_directsound_write_proc: IDirectSoundBuffer8_GetCurrentPosition, hr = %X", hr);
      GST_DSOUND_UNLOCK (dsoundbuffer);
      return THREAD_ERROR_NO_POSITION;
    }

    /* calculate the free size of the circular buffer */
    if (dwCurrentPlayCursor <= dsoundbuffer->buffer_write_offset)
      freeBufferSize = dsoundbuffer->buffer_size - 
        (dsoundbuffer->buffer_write_offset - dwCurrentPlayCursor);
    else
      freeBufferSize = dwCurrentPlayCursor - dsoundbuffer->buffer_write_offset;

    if (!gst_ring_buffer_prepare_read (buf, &readseg, &readptr, &len))
      goto complete;

    len -= dsoundbuffer->segoffset;

    if (len > freeBufferSize)
      goto complete;
    
    /* lock it */
    hr = IDirectSoundBuffer8_Lock (dsoundbuffer->pDSB8,
       dsoundbuffer->buffer_write_offset, len, &pLockedBuffer1,
          &dwSizeBuffer1, &pLockedBuffer2, &dwSizeBuffer2, 0L);

    /* copy chunks */
    if (SUCCEEDED (hr)) {
      if (len <= dwSizeBuffer1) {
        memcpy (pLockedBuffer1, (LPBYTE) readptr + dsoundbuffer->segoffset, len);
      }
      else {
        memcpy (pLockedBuffer1, (LPBYTE) readptr + dsoundbuffer->segoffset, dwSizeBuffer1);
        memcpy (pLockedBuffer2, (LPBYTE) readptr + dsoundbuffer->segoffset + dwSizeBuffer1, len - dwSizeBuffer1);
      }
      
      IDirectSoundBuffer8_Unlock (dsoundbuffer->pDSB8, pLockedBuffer1,
         dwSizeBuffer1, pLockedBuffer2, dwSizeBuffer2);
    } else {
      GST_WARNING ("gst_directsound_write_proc: IDirectSoundBuffer8_Lock, hr = %X", hr);
    }

    /* update tracking data */
    dsoundbuffer->segoffset += dwSizeBuffer1 + (len - dwSizeBuffer1);
    
    dsoundbuffer->buffer_write_offset += dwSizeBuffer1 + (len - dwSizeBuffer1);
    dsoundbuffer->buffer_write_offset %= dsoundbuffer->buffer_size;

    freeBufferSize -= dwSizeBuffer1 + (len - dwSizeBuffer1);

    /* check if we read a whole segment */
    if (dsoundbuffer->segoffset == dsoundbuffer->segsize) {
      /* advance to next segment */
      gst_ring_buffer_clear (buf, readseg);
      gst_ring_buffer_advance (buf, 1);
      dsoundbuffer->segoffset = 0;
    }

    /* if our buffer is big enough, start playing it */
    if (!(dwStatus & DSBSTATUS_PLAYING) && 
        freeBufferSize < dsoundbuffer->min_buffer_size) {
      hr = IDirectSoundBuffer8_Play (dsoundbuffer->pDSB8, 0, 0, DSBPLAY_LOOPING);
      if (FAILED(hr)) {
        GST_WARNING ("gst_directsound_write_proc: IDirectSoundBuffer8_Play, hr = %X", hr);
      }
    }
    
  complete:
    should_run = dsoundbuffer->should_run;
    flushing = dsoundbuffer->flushing;
    retries = 0;

    GST_DSOUND_UNLOCK (dsoundbuffer);
    
    /* it's extremely important to sleep in without the lock! */
    if (freeBufferSize <= dsoundbuffer->min_buffer_size || flushing)
      Sleep (dsoundbuffer->min_sleep_time);
  }
  while(should_run);

  return 0;
}
