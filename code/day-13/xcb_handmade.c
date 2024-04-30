#include <xcb/xcb.h>
#include <xcb/xkb.h> /*Require libxcb-xkb-dev package installed*/
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>
#include <x86gprintrin.h>

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

#include "handmade.h"
#include "handmade.c"

#include "xcb_handmade.h"

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
alsa_fill_sound_buffer(game_sound_output_buffer* sound_buffer) {
   int result;
   while((result = snd_pcm_writei(_pcm, sound_buffer->samples, sound_buffer->sample_count)) != sound_buffer->sample_count) {
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

INTERNAL void
joystick_input_process(game_button_state* old_state, game_button_state* new_state, s16 value) {
   new_state->ended_down = value;
   new_state->half_transition_count = (old_state->ended_down != new_state->ended_down) ? 1 : 0;
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

   alsa_sound_output sound_output = {};

   sound_output.samples_per_second = 48000;
   sound_output.samples_per_write  = sound_output.samples_per_second/30;
   sound_output.bytes_per_sample = 2 * sizeof(s16);
   sound_output.tone_hz = 256;

   alsa_init(sound_output.samples_per_second, sound_output.samples_per_write);

   s16 stick_y = 0;
   s16 stick_x = 0;
   
   struct timeval last_counter;
   gettimeofday(&last_counter, 0);

   u64 last_cycle_count = __rdtsc();

   is_running  = 1;

   s16* samples = mmap(0,
                       sound_output.samples_per_write * sound_output.bytes_per_sample,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);

   {
      game_sound_output_buffer sound_buffer = {};
      sound_buffer.samples_per_second = sound_output.samples_per_second;
      sound_buffer.sample_count = sound_output.samples_per_write;
      sound_buffer.samples = samples;
      s16* sample_out = samples;
      for(int i=0; i<sound_buffer.sample_count; ++i) {
         *sample_out++ = 0;
         *sample_out++ = 0;
      }
      alsa_fill_sound_buffer(&sound_buffer);
   }

   game_input input[2] = {};
   game_input* new_input = &input[0];
   game_input* old_input = &input[1];

   s32 button_down  = 0;
   s32 button_up    = 0;
   s32 button_right = 0;
   s32 button_left  = 0;

   while (is_running) {
      xcb_generic_event_t* _event;
      struct js_event      _joystick_event;

      while ((_event = xcb_poll_for_event(_connection))) {
         handle_event(_event);
      }
      game_controller_input* old_controller = &old_input->controllers[0];
      game_controller_input* new_controller = &new_input->controllers[0];
      while(read(_joystick_descriptor, &_joystick_event, sizeof(struct js_event)) == sizeof(struct js_event)) {
         _joystick_event.type &= ~JS_EVENT_INIT;
         switch(_joystick_event.type) {
            case JS_EVENT_BUTTON:
            {
               s16 button_value = _joystick_event.value;
               u8  button_code  = _joystick_event.number;
               if(button_code == 0) {
                  button_down = button_value;
               } else 
               if (button_code == 1){
                  button_right = button_value;
               } else 
               if (button_code == 2){
                  button_left = button_value;
               } else 
               if (button_code == 3){
                  button_up = button_value;
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

      new_controller->is_analog = 1;

      joystick_input_process(&old_controller->down,  &new_controller->down,  button_down);
      joystick_input_process(&old_controller->up,    &new_controller->up,    button_up);
      joystick_input_process(&old_controller->right, &new_controller->right, button_right);
      joystick_input_process(&old_controller->left,  &new_controller->left,  button_left);

      //Note(LAG): Deadzone not handled yet
      f32 norm_lx = (f32)stick_x / 32767.0f;
      f32 norm_ly = (f32)stick_y / 32767.0f;

      new_controller->minx = new_controller->maxx = new_controller->endx = norm_lx;
      new_controller->miny = new_controller->maxy = new_controller->endy = norm_ly;

      snd_pcm_sframes_t delay;
      snd_pcm_delay(_pcm, &delay);
      int samples_to_write = sound_output.samples_per_write - delay;

      game_sound_output_buffer sound_buffer = {};
      sound_buffer.samples_per_second = sound_output.samples_per_second;
      sound_buffer.sample_count = samples_to_write;
      sound_buffer.samples = samples;

      game_offscreen_buffer buffer = {};
      buffer.memory = global_backbuffer.pixels;
      buffer.width = global_backbuffer.width;
      buffer.height = global_backbuffer.height;
      buffer.pitch = global_backbuffer.pitch;
      game_update_render(new_input, &buffer, &sound_buffer);

      if(samples_to_write > 0) {
         alsa_fill_sound_buffer(&sound_buffer);
      }

      u16 width  = global_backbuffer.width;
      u16 height = global_backbuffer.height;
      unflushed_update_window(global_backbuffer, width, height);
      xcb_flush(_connection);

      s64 end_cycle_count = __rdtsc();

      struct timeval end_counter;
      gettimeofday(&end_counter, 0);

      u64 cycles_elapsed = end_cycle_count - last_cycle_count;
      s64 counter_elapsed = ((end_counter.tv_sec * 1000000) + end_counter.tv_usec) - ((last_counter.tv_sec * 1000000) + last_counter.tv_usec);
      f32 ms_per_frame = (f32)counter_elapsed / 1000.0f;
      f32 fps = (1000.0f*1000.0f) / (f32)counter_elapsed;
      f32 mcpf = (f32)(cycles_elapsed / (1000.0f*1000.0f));

#if 0
      char char_buffer[256];
      int length;
      length = sprintf(char_buffer, "%.2fms/f, %.2ff/s, %.2fmc/f\n", ms_per_frame, fps, mcpf);
      write(STDOUT_FILENO, char_buffer, length);
#endif

      last_counter = end_counter;
      last_cycle_count = end_cycle_count;

      game_input* temp = new_input;
      new_input = old_input;
      old_input = temp;
   }

   xcb_disconnect(_connection);
   return 0;
}
