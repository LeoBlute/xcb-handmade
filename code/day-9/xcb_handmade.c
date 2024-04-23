#include <xcb/xcb.h>
#include <xcb/xkb.h> /*Require libxcb-xkb-dev package installed*/
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>

#define PI32 3.14159265359f

#define INTERNAL        static
#define LOCAL_PERSIST   static
#define GLOBAL_VARIABLE static

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef float  f32;
typedef double f64;

typedef struct xxcb_offscreen_buffer {
   xcb_pixmap_t pixmap;
   void*        pixels;
   u16          width;
   u16          height;
   u16          pitch;
} xxcb_offscreen_buffer;

typedef struct alsa_sound_output {
   int samples_per_second;
   int samples_per_write;
   int bytes_per_sample;
   int tone_hz;
   int tone_volume;
   u32 running_sample_index;
   int wave_period;
   s16* samples_buffer;
   f32 tsine;
} alsa_sound_output;

#define BYTES_PER_PIXEL 4

GLOBAL_VARIABLE unsigned              is_running;
GLOBAL_VARIABLE xxcb_offscreen_buffer global_backbuffer;
GLOBAL_VARIABLE unsigned              keys_down[200];

GLOBAL_VARIABLE xcb_connection_t* _connection;
GLOBAL_VARIABLE xcb_screen_t*     _screen;
GLOBAL_VARIABLE xcb_window_t      _window;
GLOBAL_VARIABLE xcb_gcontext_t    _gcontext;
GLOBAL_VARIABLE xcb_atom_t        _wm_protocols;
GLOBAL_VARIABLE xcb_atom_t        _wm_delete_protocol;

GLOBAL_VARIABLE snd_pcm_t* _pcm;

INTERNAL void
alsa_fill_sound_buffer(alsa_sound_output* sound_output, int samples_to_write) {
   s16* sample_out = sound_output->samples_buffer;

   for(int i=0; i<samples_to_write; ++i) {
      // f32 t = 2.0f*PI32*(f32)sound_output->running_sample_index / (f32)sound_output->wave_period;
      f32 sine_value = sinf(sound_output->tsine);
      s16 sample_value = (s16)(sine_value * sound_output->tone_volume);
      *sample_out++ = sample_value;
      *sample_out++ = sample_value;

      sound_output->tsine += 2.0f*PI32*1.0f / (f32)sound_output->wave_period;
      ++sound_output->running_sample_index;
   }

   int result;
   while((result = snd_pcm_writei(_pcm, sound_output->samples_buffer, samples_to_write)) != samples_to_write) {
      if(result == -EPIPE) {
         snd_pcm_prepare(_pcm);
      } else {
         //TODO(LAG): Diagnostic
      }
   }  
}

INTERNAL void
alsa_init(int samples_per_second, int samples_per_write) {
   snd_pcm_hw_params_t* _pcm_hw_params;

   if(snd_pcm_open(&_pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
      //TODO(LAG) Diagnostic
   }

   _pcm_hw_params = (snd_pcm_hw_params_t*)mmap(0,
                                               snd_pcm_hw_params_sizeof(),
                                               PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS,
                                               -1,
                                               0);
   snd_pcm_hw_params_any(_pcm, _pcm_hw_params);

   snd_pcm_hw_params_set_access(_pcm, _pcm_hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
   snd_pcm_hw_params_set_format(_pcm, _pcm_hw_params, SND_PCM_FORMAT_S16_LE);
   snd_pcm_hw_params_set_channels(_pcm, _pcm_hw_params, 2);
   snd_pcm_hw_params_set_rate(_pcm, _pcm_hw_params, samples_per_second, 0);
   snd_pcm_hw_params_set_buffer_size(_pcm, _pcm_hw_params, samples_per_write);
   snd_pcm_hw_params_set_period_time(_pcm, _pcm_hw_params, 100000, 0);

   snd_pcm_hw_params(_pcm, _pcm_hw_params);
}

INTERNAL void
render_weird_gradient(xxcb_offscreen_buffer buffer, u16 xoffset, u16 yoffset) {
   u16 width  = buffer.width;
   u16 height = buffer.height;

   u8* row = (u8*)buffer.pixels;
   for(u16 y = 0; y < height; ++y) {
      u32* pixel = (u32*)row;
      for(u16 x = 0; x < width; ++x) {
         u8 blue  = x + xoffset;
         u8 green = y + yoffset;
         *pixel++ =((green << 8) | blue);
      }
      row += buffer.pitch;
   }
}

INTERNAL void
unflushed_resize_backbuffer(xxcb_offscreen_buffer* buffer, u16 width, u16 height) {
   if(buffer->pixels) {
      munmap(buffer->pixels, buffer->width * buffer->height * BYTES_PER_PIXEL);
   }
   if(buffer->pixmap) {
      xcb_free_pixmap(_connection, buffer->pixmap);
   }

   buffer->width  = width;
   buffer->height = height;
   buffer->pitch  = width * BYTES_PER_PIXEL;
   buffer->pixels = mmap(0,
                         width * height * BYTES_PER_PIXEL,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);

   buffer->pixmap = xcb_generate_id(_connection);
   xcb_create_pixmap(_connection, _screen->root_depth, buffer->pixmap, _window, width, height);
}

INTERNAL void
unflushed_update_window(xxcb_offscreen_buffer buffer, u16 width, u16 height) {
   xcb_put_image(_connection,
                 XCB_IMAGE_FORMAT_Z_PIXMAP,
                 buffer.pixmap,
                 _gcontext,
                 width, height,
                 0, 0,
                 0,
                 _screen->root_depth,
                 width * height * BYTES_PER_PIXEL, (u8*)buffer.pixels);
   xcb_copy_area(_connection, buffer.pixmap, _window, _gcontext, 0, 0, 0, 0, width, height);
}


INTERNAL void
handle_event(xcb_generic_event_t* _event) {
   switch(_event->response_type &~0x80) {
      case XCB_CLIENT_MESSAGE:
      {
         xcb_client_message_event_t* _client_message = (xcb_client_message_event_t*)_event;
         if(_client_message->data.data32[0] == _wm_delete_protocol) {
            is_running = 0;
         }
      } break;
      case XCB_CONFIGURE_NOTIFY:
      {
         xcb_configure_notify_event_t* _configure_notify_event = (xcb_configure_notify_event_t*)_event;
         u16 width  = _configure_notify_event->width;
         u16 height = _configure_notify_event->height;

         unflushed_resize_backbuffer(&global_backbuffer, width, height);
         xcb_flush(_connection);
      } break;
      case XCB_EXPOSE:
      {
         xcb_expose_event_t* _expose_event = (xcb_expose_event_t*)_event;
         u16 width = _expose_event->width;
         u16 height = _expose_event->height;
      } break;
      case XCB_KEY_RELEASE:
      case XCB_KEY_PRESS:
      {
         xcb_key_press_event_t* _key_event = (xcb_key_press_event_t*)_event;
         u8 keycode = _key_event->detail;

         if((_event->response_type &~0x80) == XCB_KEY_PRESS && !keys_down[keycode]) {
            keys_down[keycode] = 1;
         } else if((_event->response_type &~0x80) == XCB_KEY_RELEASE) {
            keys_down[keycode] = 0;
         }
      } break;
   }
   free(_event);
}

int main() {
   _connection = xcb_connect(0, 0);
   if(xcb_connection_has_error(_connection)) {
      //TODO(LAG): Log error
   }

   _screen = xcb_setup_roots_iterator(xcb_get_setup(_connection)).data;

   {
      xcb_xkb_use_extension(_connection, 
                            XCB_XKB_MAJOR_VERSION, 
                            XCB_XKB_MINOR_VERSION);
      xcb_xkb_per_client_flags(_connection,
                               XCB_XKB_ID_USE_CORE_KBD,                                                       
                               XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
                               1,0,0,0);
   }

   {
      xcb_intern_atom_cookie_t _intern_atom_cookie;
      xcb_intern_atom_reply_t* _intern_atom_reply;

      _intern_atom_cookie = xcb_intern_atom(_connection, 1, 12, "WM_PROTOCOLS");
      _intern_atom_reply  = xcb_intern_atom_reply(_connection, _intern_atom_cookie, 0);
      _wm_protocols       = _intern_atom_reply->atom;
      free(_intern_atom_reply);

      _intern_atom_cookie = xcb_intern_atom(_connection, 1, 16, "WM_DELETE_WINDOW");
      _intern_atom_reply  = xcb_intern_atom_reply(_connection, _intern_atom_cookie, 0);
      _wm_delete_protocol = _intern_atom_reply->atom;
      free(_intern_atom_reply);
   }

   u32 _window_mask = XCB_CW_EVENT_MASK;
   u32 _window_values[] = {XCB_EVENT_MASK_EXPOSURE         |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_KEY_PRESS        |
                           XCB_EVENT_MASK_KEY_RELEASE};

   _window = xcb_generate_id(_connection);
   xcb_create_window(_connection,
                     XCB_COPY_FROM_PARENT,
                     _window,
                     _screen->root,
                     0, 0,
                     1280, 720,
                     0,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     _screen->root_visual,
                     _window_mask, _window_values);
   xcb_change_property(_connection,
                       XCB_PROP_MODE_REPLACE,
                       _window,
                       XCB_ATOM_WM_NAME,
                       XCB_ATOM_STRING,
                       8,
                       8, "Handmade");
   xcb_change_property(_connection,
                       XCB_PROP_MODE_REPLACE,
                       _window,
                       _wm_protocols,
                       XCB_ATOM_ATOM,
                       32,
                       1, &_wm_delete_protocol);
   xcb_map_window(_connection, _window);

   _gcontext = xcb_generate_id(_connection);
   xcb_create_gc(_connection, _gcontext, _screen->root, 0, 0);

   xcb_flush(_connection);


   int _joystick_descriptor = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
   if(_joystick_descriptor < 0) {
      //TODO(LAG): Handle Gamepad connected
   } else {
      //TODO(LAG): Handle Gamepad not found
   }

   u16 yoffset = 0;
   u16 xoffset = 0;

   alsa_sound_output sound_output = {};

   sound_output.samples_per_second = 48000;
   sound_output.samples_per_write  = sound_output.samples_per_second/15;
   sound_output.bytes_per_sample = 2 * sizeof(s16);
   sound_output.tone_hz = 256;
   sound_output.tone_volume = 3000;
   sound_output.running_sample_index;
   sound_output.wave_period = sound_output.samples_per_second/sound_output.tone_hz;
   sound_output.tsine = 0.0f;
   sound_output.samples_buffer = mmap(0,
                                      sound_output.samples_per_write * sound_output.bytes_per_sample,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE,
                                      -1,
                                      0);

   alsa_init(sound_output.samples_per_second, sound_output.samples_per_write);

   s16 stick_y = 0;
   s16 stick_x = 0;

   is_running  = 1;
   while (is_running) {
      xcb_generic_event_t* _event;
      struct js_event      _joystick_event;

      while ((_event = xcb_poll_for_event(_connection))) {
         handle_event(_event);
      }
      while(read(_joystick_descriptor, &_joystick_event, sizeof(struct js_event)) == sizeof(struct js_event)) {
         _joystick_event.type &= ~JS_EVENT_INIT;
         switch(_joystick_event.type) {
            case JS_EVENT_BUTTON:
            {
               if(_joystick_event.value) {
               } else {
               }
            } break;
            case JS_EVENT_AXIS:
            {
               s16 stick_value = _joystick_event.value;
               u8  stick_code  = _joystick_event.number;
               if(stick_code == 0) {
                  stick_x = stick_value;
               } else
               if(stick_code == 1) {
                  stick_y = stick_value;
               }
            } break;
         }
      }

      sound_output.tone_hz = 512 + (int)(256.0f*((f32)stick_y / 30000.0f));
      sound_output.wave_period = sound_output.samples_per_second/sound_output.tone_hz;
      xoffset += stick_x / 4096;
      yoffset += stick_y / 4096;

      render_weird_gradient(global_backbuffer, xoffset, yoffset);


      snd_pcm_sframes_t avail, delay;
      snd_pcm_avail_delay(_pcm, &avail, &delay);
      int samples_to_write = avail;
      if(samples_to_write + delay > sound_output.samples_per_write) {
         samples_to_write = sound_output.samples_per_write - delay;
      }
      if(samples_to_write > 0) {
         alsa_fill_sound_buffer(&sound_output, samples_to_write);
      }

      u16 width  = global_backbuffer.width;
      u16 height = global_backbuffer.height;
      unflushed_update_window(global_backbuffer, width, height);
      xcb_flush(_connection);

      ++xoffset;
   }

   xcb_disconnect(_connection);
   return 0;
}
